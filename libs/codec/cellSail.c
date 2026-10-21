/*
 * ps3recomp - cellSail HLE: the system's movie player (libsail)
 *
 * The player is handed a URI and two adapters the title implements:
 *
 *   - the graphics adapter: the title allocates the frame buffers (its alloc
 *     callback fills an array of their addresses) and, every frame it shows
 *     the movie, calls cellSailGraphicsAdapterGetFrame for the picture to
 *     draw;
 *   - the sound adapter: the title's own mixer calls
 *     cellSailSoundAdapterGetFrame for the next `samples` samples of
 *     interleaved float32 sound.
 *
 * Progress reaches the title through one callback, f(arg, event, arg0, arg1)
 * with the event a u64 major << 32 | minor: PLAYER_STATE_CHANGED (arg0 the
 * state), PLAYER_CALL_COMPLETED (minor the call, arg0 its result, arg1 the
 * call's userParam) and SOURCE_EOS when the stream has played out. A title
 * waits for the completions: Bleach: Soul Resurreccion polls for Start's
 * with a 1 ms sleep, and takes SOURCE_EOS followed by the OPENED state as
 * the end of the movie. Each call's events are delivered inside the call;
 * the end of the stream is noticed, and announced, in GetFrame.
 *
 * Only a PAMF file (the PS3's MPEG program stream with a PAMF header) with
 * H.264 video and ATRAC3plus sound is played -- which is what a disc movie
 * is. A thread reads it a 2048-byte pack at a time: the video is cut into
 * access units at its access unit delimiters and decoded by FFmpeg
 * (vdec_ffmpeg.c) straight into the title's frame buffers in the format it
 * asked for; the ATRAC3plus frames (each behind a 0x0FD0 header, in private
 * stream 1 behind a 4-byte header) are decoded by FFmpeg into a ring the
 * sound adapter drains. Where FFmpeg is not available a stream opens and
 * then ends at once, as the movie being skipped.
 *
 * The clock the pictures follow is the sound the title has taken, less the
 * time its mixer holds that sound before it is heard (SAIL_AV_DELAY_MS,
 * default 100: Bleach keeps 4096 samples, 85 ms, in its stream buffer); run
 * between the pulls on the host's clock, so a picture is chosen to the
 * millisecond. Without sound the host's clock alone.
 *
 *   SAIL_LOG=1     a line per picture and per sound pull
 *   SAIL_SKIP=1    every stream ends as it starts (the old behaviour)
 */

#include "ps3emu/guest_call.h"
#include "cellSail.h"
#include "cellVdec.h"          /* CELL_VDEC_CODEC_TYPE_AVC */
#include "vdec_ffmpeg.h"
#include "ffmpeg_runtime.h"
#include "../../runtime/platform/win32_compat.h"   /* threads, SRWLOCK, QPC */
#include "../../runtime/ppu/ppu_memory.h"          /* vm_read*, vm_write*, vm_memcpy_to */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ppu_loader.cpp: call a guest function and return its r3 (the allocator). */
uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
/* sys_fs.c: a guest path to the host file it is. */
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

static int sail_log(void)
{
    static int on = -1;
    if (on < 0) on = getenv("SAIL_LOG") ? 1 : 0;
    return on;
}

/* ---------------------------------------------------------------------------
 * The stream: demuxer, decoders, frame slots, sound ring
 * -----------------------------------------------------------------------*/

#define SAIL_SLOTS      8          /* frame buffers asked for */
#define SAIL_PEND       16         /* decoded pictures waiting for a buffer: FFmpeg hands
                                    * over ~11 at once as the stream starts */
#define SAIL_RING       (48000 * 4) /* sound ring, sample frames */
#define SAIL_SESSION    1u

enum { SLOT_FREE = 0, SLOT_FILLING, SLOT_READY, SLOT_SHOWN };

typedef struct { u32 ea; s64 pts; int st; } SailSlot;
typedef struct { u8* yuv; u32 w, h; s64 pts; } SailPic;

typedef struct SailStream {
    FILE* f;
    u64 pos, end;                       /* the data's file offsets */

    /* video (decoder thread only, but for what the lock covers) */
    VdecFf* vd;
    u8* vbuf; size_t vlen, vcap;        /* ES not yet cut into access units */
    size_t au_start;                    /* the current AU's start in vbuf */
    int have_au;
    s64 au_pts, pes_pts, last_au_pts;   /* -1: none */
    s64 vdur;                           /* a frame's duration, 90 kHz */
    u32 w, h;

    /* audio (decoder thread only) */
    u8* abuf; size_t alen, acap;
    int a_ok;                           /* the decoder is open */
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    const PS3FFmpeg* ff;
    AVCodecContext* ad;
    AVPacket* apkt;
    AVFrame* afrm;
#endif
    u8* apad;                           /* a frame plus FFmpeg's input padding */

    /* shared with the title's threads, under lock */
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    SailPic pend[SAIL_PEND]; int npend;
    SailSlot slot[SAIL_SLOTS]; u32 nslot, frame_bytes;
    int cur, prev;                      /* the slots GetFrame last returned */
    s64 vfirst, vlast;                  /* PTS of the first / last picture made */
    float* ring; u64 awritten, aread; int ach; u32 afs; s64 afirst;
    int has_audio;
    int eof, eos, running, paused, stop;
    double anchor;                      /* the clock, 90 kHz, at anchor_qpc */
    LARGE_INTEGER anchor_qpc, qpf;
    s64 delay;                          /* SAIL_AV_DELAY_MS in 90 kHz ticks */
    HANDLE thread;
} SailStream;

static s64 pes_pts(const u8* p)
{
    return ((s64)((p[0] >> 1) & 7) << 30) | ((s64)p[1] << 22) | ((s64)(p[2] >> 1) << 15) |
           ((s64)p[3] << 7) | (s64)(p[4] >> 1);
}

static int grow(u8** buf, size_t* cap, size_t need)
{
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 65536;
    while (n < need) n *= 2;
    u8* b = (u8*)realloc(*buf, n);
    if (!b) return 0;
    *buf = b; *cap = n;
    return 1;
}

static void sail_fill_slots(SailStream* s);

/* The picture the decoder made, in display order, its PTS as the token. On
 * the decoder thread, which is also the one that moves pictures into the
 * frame buffers: with the queue full it does that here, and waits for the
 * title to give a buffer back. */
static void sail_emit(void* arg, const VdecFfPicture* pic)
{
    SailStream* s = (SailStream*)arg;
    AcquireSRWLockExclusive(&s->lock);
    if (sail_log()) printf("[cellSail] decoded %lld (%d waiting, %u buffers)\n", (long long)pic->token, s->npend, s->nslot);
    for (;;) {
        sail_fill_slots(s);
        /* Before the buffers exist (the stream being opened) nothing frees
         * the queue: a picture past it is dropped instead. */
        if (s->npend < SAIL_PEND || s->stop || !s->nslot) break;
        SleepConditionVariableSRW(&s->cv, &s->lock, 50, 0);
    }
    if (s->stop || s->npend == SAIL_PEND) { ReleaseSRWLockExclusive(&s->lock); free(pic->yuv); return; }
    SailPic* p = &s->pend[s->npend++];
    p->yuv = pic->yuv; p->w = pic->width; p->h = pic->height; p->pts = (s64)pic->token;
    if (!s->w) { s->w = pic->width; s->h = pic->height; }
    if (s->vfirst < 0 || p->pts < s->vfirst) s->vfirst = p->pts;
    if (p->pts > s->vlast) s->vlast = p->pts;
    ReleaseSRWLockExclusive(&s->lock);
}

static void sail_decode_au(SailStream* s, const u8* au, size_t n)
{
    s64 pts = s->au_pts;
    if (pts < 0) pts = s->last_au_pts >= 0 ? s->last_au_pts + s->vdur : 0;
    else if (s->last_au_pts >= 0 && pts > s->last_au_pts && pts - s->last_au_pts < s->vdur)
        s->vdur = pts - s->last_au_pts;      /* the smallest step seen: a frame */
    s->last_au_pts = pts;
    if (s->vd) vdec_ff_decode(s->vd, au, (u32)n, (u32)pts, sail_emit, s);
}

/* Video payload: cut it into access units at each access unit delimiter
 * (00 00 01 09). A PES's PTS is the PTS of the first AU starting in it. */
static void sail_video_bytes(SailStream* s, const u8* p, size_t n, s64 pts)
{
    if (pts >= 0) s->pes_pts = pts;
    if (!grow(&s->vbuf, &s->vcap, s->vlen + n)) return;
    const size_t from = s->vlen;
    memcpy(s->vbuf + s->vlen, p, n);
    s->vlen += n;
    for (size_t k = from > 3 ? from - 3 : 0; k + 4 <= s->vlen; k++) {
        const u8* q = s->vbuf + k;
        if (q[0] || q[1] || q[2] != 1 || (q[3] & 0x1F) != 9) continue;
        if (s->have_au && k > s->au_start) sail_decode_au(s, s->vbuf + s->au_start, k - s->au_start);
        s->au_start = k; s->have_au = 1;
        s->au_pts = s->pes_pts; s->pes_pts = -1;
        k += 3;
    }
    if (s->have_au && s->au_start) {
        memmove(s->vbuf, s->vbuf + s->au_start, s->vlen - s->au_start);
        s->vlen -= s->au_start; s->au_start = 0;
    }
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME
static int sail_audio_open(SailStream* s, const u8* hdr)
{
    static const u32 fs_tab[8] = { 32000, 44100, 48000, 88200, 96000, 0, 0, 0 };
    static const int ch_tab[8] = { 0, 1, 2, 3, 4, 6, 7, 8 };
    const u32 fs = fs_tab[hdr[2] >> 5];
    const int ch = ch_tab[(hdr[2] >> 2) & 7];
    const int bytes = ((((hdr[2] & 3) << 8) | hdr[3]) + 1) * 8;
    if (!s->ff || !fs || !ch || ch > 2) {
        printf("[cellSail] ATRAC3plus %u Hz, %d channel(s): not played\n", fs, ch);
        return -1;
    }
    const AVCodec* c = s->ff->avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!c) { printf("[cellSail] libavcodec has no ATRAC3plus decoder\n"); return -1; }
    s->ad = s->ff->avcodec_alloc_context3(c);
    s->apkt = s->ff->av_packet_alloc();
    s->afrm = s->ff->av_frame_alloc();
    s->apad = (u8*)calloc(1, 0x2000 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s->ad || !s->apkt || !s->afrm || !s->apad) return -1;
    s->ad->sample_rate = (int)fs;
    s->ad->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    s->ad->ch_layout.nb_channels = ch;
    s->ad->block_align = bytes;
    if (s->ff->avcodec_open2(s->ad, c, NULL) < 0) {
        printf("[cellSail] could not open the ATRAC3plus decoder\n");
        return -1;
    }
    AcquireSRWLockExclusive(&s->lock);
    s->ach = ch; s->afs = fs;
    ReleaseSRWLockExclusive(&s->lock);
    printf("[cellSail] ATRAC3plus: %u Hz, %d channel(s), %d-byte frames\n", fs, ch, bytes);
    return 0;
}

static void sail_audio_frame(SailStream* s, const u8* data, int n)
{
    memcpy(s->apad, data, (size_t)n);
    memset(s->apad + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s->apkt->data = s->apad; s->apkt->size = n;
    int r = s->ff->avcodec_send_packet(s->ad, s->apkt);
    s->apkt->data = NULL; s->apkt->size = 0;
    if (r < 0) return;
    while (s->ff->avcodec_receive_frame(s->ad, s->afrm) >= 0) {
        AVFrame* f = s->afrm;
        const int ns = f->nb_samples, ch = s->ach;
        const int planar = f->format == AV_SAMPLE_FMT_FLTP;
        if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT) {
            AcquireSRWLockExclusive(&s->lock);
            for (int i = 0; i < ns && s->awritten - s->aread < SAIL_RING; i++) {
                float* o = s->ring + (size_t)(s->awritten % SAIL_RING) * 2;
                for (int c = 0; c < 2; c++) {
                    const int src = c < ch ? c : 0;
                    o[c] = planar ? ((const float*)f->extended_data[src])[i]
                                  : ((const float*)f->extended_data[0])[i * ch + src];
                }
                s->awritten++;
            }
            ReleaseSRWLockExclusive(&s->lock);
        }
        s->ff->av_frame_unref(f);
    }
}
#endif

/* Sound payload (private stream 1, behind its 4-byte header): ATRAC3plus
 * frames, each 8 bytes of 0x0FD0 header and then the frame, whose size is
 * (the header's 10-bit field + 1) * 8 bytes. */
static void sail_audio_bytes(SailStream* s, const u8* p, size_t n, s64 pts)
{
    if (s->a_ok < 0) return;
    AcquireSRWLockExclusive(&s->lock);
    if (s->afirst < 0 && pts >= 0) s->afirst = pts;
    ReleaseSRWLockExclusive(&s->lock);
    if (!grow(&s->abuf, &s->acap, s->alen + n)) return;
    memcpy(s->abuf + s->alen, p, n);
    s->alen += n;
    size_t o = 0;
    while (s->alen - o >= 8) {
        const u8* h = s->abuf + o;
        if (h[0] != 0x0F || h[1] != 0xD0) { o++; continue; }     /* resync */
        const size_t len = (size_t)(((((h[2] & 3) << 8) | h[3]) + 1) * 8) + 8;
        if (s->alen - o < len) break;
#ifdef PS3RECOMP_FFMPEG_RUNTIME
        if (!s->a_ok) {
            s->a_ok = sail_audio_open(s, h) == 0 ? 1 : -1;
            AcquireSRWLockExclusive(&s->lock);
            s->has_audio = s->a_ok > 0;
            ReleaseSRWLockExclusive(&s->lock);
        }
        if (s->a_ok > 0) sail_audio_frame(s, h + 8, (int)(len - 8));
#endif
        o += len;
    }
    memmove(s->abuf, s->abuf + o, s->alen - o);
    s->alen -= o;
}

/* One 2048-byte pack. 0 at the end of the data. */
static int sail_demux_pack(SailStream* s)
{
    u8 pk[2048];
    if (s->pos + sizeof pk > s->end || fread(pk, 1, sizeof pk, s->f) != sizeof pk) return 0;
    s->pos += sizeof pk;
    size_t i = 0;
    while (i + 6 <= sizeof pk) {
        if (pk[i] || pk[i + 1] || pk[i + 2] != 1) break;
        const u8 id = pk[i + 3];
        if (id == 0xBA) { i += 14 + (pk[i + 13] & 7); continue; }   /* MPEG-2 pack header */
        if (id == 0xB9) break;                                        /* end code */
        const size_t len = ((size_t)pk[i + 4] << 8) | pk[i + 5];
        if (i + 6 + len > sizeof pk) break;
        const u8* b = pk + i + 6;
        if ((id == 0xE0 || id == 0xBD) && len >= 3 && 3u + b[2] <= len) {
            const size_t hl = b[2];
            const s64 pts = ((b[1] & 0x80) && hl >= 5) ? pes_pts(b + 3) : -1;
            const u8* pay = b + 3 + hl;
            const size_t plen = len - 3 - hl;
            if (id == 0xE0)
                sail_video_bytes(s, pay, plen, pts);
            else if (plen > 4 && (pay[0] & 0xF0) == 0)    /* ATRAC3plus sub-streams 0x00..0x0F */
                sail_audio_bytes(s, pay + 4, plen - 4, pts);
        }
        i += 6 + len;
    }
    return 1;
}

/* The pictures waiting for a buffer, into the free ones. Called with the lock
 * held; it is dropped around each conversion. */
static void sail_fill_slots(SailStream* s)
{
    while (s->npend && s->nslot) {
        int k = -1;
        for (u32 i = 0; i < s->nslot; i++)
            if (s->slot[i].st == SLOT_FREE && (int)i != s->cur && (int)i != s->prev) { k = (int)i; break; }
        if (k < 0) return;
        SailPic pic = s->pend[0];
        memmove(s->pend, s->pend + 1, sizeof s->pend[0] * (size_t)(--s->npend));
        s->slot[k].st = SLOT_FILLING;
        WakeAllConditionVariable(&s->cv);
        ReleaseSRWLockExclusive(&s->lock);
        if (pic.w == s->w && pic.h == s->h)
            vdec_store_picture(s->slot[k].ea, pic.yuv, pic.w, pic.h,
                               CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED, 1 /* BT.709 */, 0xFF);
        free(pic.yuv);
        AcquireSRWLockExclusive(&s->lock);
        s->slot[k].pts = pic.pts;
        s->slot[k].st = SLOT_READY;
    }
}

static DWORD WINAPI sail_thread(LPVOID arg)
{
    SailStream* s = (SailStream*)arg;
    AcquireSRWLockExclusive(&s->lock);
    while (!s->stop) {
        sail_fill_slots(s);
        /* Read on while a picture or two wait for a buffer -- or more, up to
         * the queue, while the sound runs low: the file holds a picture's
         * sound some way after it, and the title pulls sound in bursts. */
        const u64 buffered = s->awritten - s->aread;
        const int ring_ok = !s->has_audio || buffered + 3 * 2048 <= SAIL_RING;
        const int low = s->has_audio && buffered < (u64)s->afs / 2;
        if (!s->eof && ring_ok && (s->npend < 2 || (low && s->npend < SAIL_PEND - 2))) {
            ReleaseSRWLockExclusive(&s->lock);
            const int more = sail_demux_pack(s);
            if (!more) {
                if (s->have_au && s->vlen > s->au_start) {
                    sail_decode_au(s, s->vbuf + s->au_start, s->vlen - s->au_start);
                    s->have_au = 0;
                }
                if (s->vd) vdec_ff_drain(s->vd, sail_emit, s);
            }
            AcquireSRWLockExclusive(&s->lock);
            if (!more) s->eof = 1;
            continue;
        }
        SleepConditionVariableSRW(&s->cv, &s->lock, 20, 0);
    }
    ReleaseSRWLockExclusive(&s->lock);
    return 0;
}

static double sail_clock(SailStream* s)       /* under lock */
{
    if (!s->running || s->paused) return s->anchor;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return s->anchor + (double)(now.QuadPart - s->anchor_qpc.QuadPart) * 90000.0 / (double)s->qpf.QuadPart;
}

static void sail_set_anchor(SailStream* s, double t)   /* under lock */
{
    s->anchor = t;
    QueryPerformanceCounter(&s->anchor_qpc);
}

static void sail_stream_free(SailStream* s)
{
    if (!s) return;
    if (s->thread) {
        AcquireSRWLockExclusive(&s->lock);
        s->stop = 1;
        WakeAllConditionVariable(&s->cv);
        ReleaseSRWLockExclusive(&s->lock);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
    }
    if (s->vd) vdec_ff_close(s->vd);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (s->ad) s->ff->avcodec_free_context(&s->ad);
    if (s->apkt) s->ff->av_packet_free(&s->apkt);
    if (s->afrm) s->ff->av_frame_free(&s->afrm);
#endif
    for (int i = 0; i < s->npend; i++) free(s->pend[i].yuv);
    if (s->f) fclose(s->f);
    free(s->vbuf); free(s->abuf); free(s->apad); free(s->ring);
    free(s);
}

/* ---------------------------------------------------------------------------
 * Players and adapters
 * -----------------------------------------------------------------------*/

typedef struct SailGfx { int in_use; u32 self, funcs, arg; u8 pref[32]; } SailGfx;
typedef struct SailSnd { int in_use; u32 self, funcs, arg; } SailSnd;

typedef struct SailPlayer {
    int in_use;
    u32 self, alloc_funcs, alloc_arg, cb, cb_arg;
    u32 gfx, snd;                 /* the adapters' EAs */
    u32 desc;                     /* the descriptor added */
    char uri[512];
    int state;
    SailStream* st;
    u32 ppframe, vfmt, afmt, sndbuf; /* guest memory of ours, from the title's allocator */
    int eos_pending;              /* SOURCE_EOS + OPENED to announce */
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];
static SailGfx s_gfx[CELL_SAIL_PLAYER_MAX];
static SailSnd s_snd[CELL_SAIL_PLAYER_MAX];

static SailPlayer* player_of(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].self == self) return &s_players[i];
    return NULL;
}
static SailGfx* gfx_of(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_gfx[i].in_use && s_gfx[i].self == self) return &s_gfx[i];
    return NULL;
}
static SailSnd* snd_of(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_snd[i].in_use && s_snd[i].self == self) return &s_snd[i];
    return NULL;
}
static SailPlayer* player_of_gfx(u32 gfx)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].gfx == gfx) return &s_players[i];
    return NULL;
}
static SailPlayer* player_of_snd(u32 snd)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].snd == snd) return &s_players[i];
    return NULL;
}

/* An event to the title's callback, on the calling thread. */
static void sail_event(const SailPlayer* p, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (!p || !p->cb || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(p->cb, p->cb_arg, ((u64)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
}
static void sail_state(SailPlayer* p, int state)
{
    p->state = state;
    sail_event(p, CELL_SAIL_EVENT_PLAYER_STATE_CHANGED, 0, (u64)state, 0);
}

/* Guest memory from the title's allocator: alloc(arg, boundary, size). The
 * allocator's own pArg is 0 in Bleach, whose alloc callback needs the
 * player's callback argument, so that is what it is given then. */
static u32 sail_alloc(const SailPlayer* p, u32 size)
{
    if (!p->alloc_funcs) return 0;
    const u32 fn = vm_read32(p->alloc_funcs);
    if (!fn) return 0;
    const u32 ea = (u32)ppu_guest_call(fn, p->alloc_arg ? p->alloc_arg : p->cb_arg, 128, size, 0, 0, 0, 0, 0);
    if (ea) for (u32 i = 0; i < size; i += 4) vm_write32(ea + i, 0);
    return ea;
}
static void sail_free(const SailPlayer* p, u32 ea)
{
    if (!ea || !p->alloc_funcs) return;
    const u32 fn = vm_read32(p->alloc_funcs + 4);
    if (fn) ppu_guest_call(fn, p->alloc_arg ? p->alloc_arg : p->cb_arg, 128, ea, 0, 0, 0, 0, 0);
}

/* CellSailVideoFormat / CellSailAudioFormat of the open stream. */
static void sail_write_vformat(const SailStream* s, u32 ea)
{
    vm_write8(ea + 0, CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED);
    vm_write8(ea + 1, 0);                               /* progressive */
    vm_write8(ea + 2, 8);                               /* bits per colour */
    /* CELL_SAIL_VIDEO_FRAME_RATE_: 0 23.976, 1 24, 2 25, 3 29.97, 4 30, 5 50, 6 59.94, 7 60 */
    const s64 d = s->vdur;
    vm_write8(ea + 3, (u8)(d >= 3754 ? 0 : d >= 3750 ? 1 : d >= 3600 ? 2 : d >= 3003 ? 3 :
                           d >= 3000 ? 4 : d >= 1800 ? 5 : d >= 1501 ? 6 : 7));
    vm_write16(ea + 4, (u16)s->w);
    vm_write16(ea + 6, (u16)s->h);
    vm_write32(ea + 8, s->w * 4);                       /* pitch */
    vm_write32(ea + 12, 0xFF);                          /* alpha */
    vm_write8(ea + 16, 1);                              /* BT.709 */
    vm_write8(ea + 17, 2);                              /* 16:9 */
    vm_write8(ea + 18, 0);                              /* limited range */
}
static void sail_write_aformat(const SailStream* s, u32 ea)
{
    vm_write8(ea + 0, CELL_SAIL_AUDIO_CODING_LPCM_FLOAT32);
    vm_write8(ea + 1, 2);                               /* channels handed over */
    vm_write16(ea + 2, 256);                            /* samples a frame, at most */
    vm_write32(ea + 4, s && s->afs ? s->afs : 48000);
    vm_write32(ea + 8, 2);                              /* CH_LAYOUT_2CH_LR */
    vm_write32(ea + 12, 0xFFFFFFFFu);
    vm_write64(ea + 16, ~0ull);
}

/* Open the descriptor's file and decode up to its first picture, so the
 * frame buffers can be asked for at the stream's size. NULL: nothing to play
 * (no file, not a PAMF, no FFmpeg) -- the stream then ends as it starts. */
static SailStream* sail_stream_open(const char* uri)
{
    if (getenv("SAIL_SKIP")) return NULL;
    const char* path = uri;
    if (!strncmp(path, "x-cell-fs://", 12)) path += 12;
    char host[1024];
    sys_fs_translate_path(path, host, (int)sizeof host);
    FILE* f = fopen(host, "rb");
    if (!f) { printf("[cellSail] cannot open %s (%s)\n", path, host); return NULL; }
    u8 h[16];
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "PAMF", 4)) {
        printf("[cellSail] %s is not a PAMF stream\n", path);
        fclose(f);
        return NULL;
    }
    SailStream* s = (SailStream*)calloc(1, sizeof *s);
    if (!s) { fclose(f); return NULL; }
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->cv);
    QueryPerformanceFrequency(&s->qpf);
    s->f = f;
    s->pos = (u64)((u32)h[8] << 24 | (u32)h[9] << 16 | (u32)h[10] << 8 | h[11]) * 2048u;
    s->end = s->pos + (u64)((u32)h[12] << 24 | (u32)h[13] << 16 | (u32)h[14] << 8 | h[15]) * 2048u;
    fseek(f, (long)s->pos, SEEK_SET);
    s->cur = s->prev = -1;
    s->au_pts = s->pes_pts = s->last_au_pts = -1;
    s->vfirst = -1; s->vlast = -1; s->afirst = -1;
    s->vdur = 3003;
    { const char* e = getenv("SAIL_AV_DELAY_MS"); s->delay = (s64)(e ? atoi(e) : 100) * 90; }
    s->ring = (float*)calloc((size_t)SAIL_RING * 2, sizeof(float));
    s->vd = vdec_ff_open(CELL_VDEC_CODEC_TYPE_AVC);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    s->ff = ps3_ffmpeg_get();
#endif
    if (!s->vd || !s->ring) {
        printf("[cellSail] no H.264 decoder (FFmpeg): %s is skipped\n", path);
        sail_stream_free(s);
        return NULL;
    }
    unsigned packs = 0;
    while (!s->npend && sail_demux_pack(s)) packs++;
    if (!s->npend) {
        printf("[cellSail] %s: no picture decoded\n", path);
        sail_stream_free(s);
        return NULL;
    }
    printf("[cellSail] %s: H.264 %ux%u, a frame every %.2f ms%s; the first picture after %u packs\n", path,
           s->w, s->h, (double)s->vdur / 90.0, s->has_audio ? ", ATRAC3plus sound" : "", packs);
    fflush(stdout);
    return s;
}

/* ---------------------------------------------------------------------------
 * Memory allocator and player
 * -----------------------------------------------------------------------*/

s32 cellSailMemAllocatorInitialize(u32 pSelf, u32 pCallbacks)
{
    printf("[cellSail] MemAllocatorInitialize(allocator=0x%08X funcs=0x%08X)\n", pSelf, pCallbacks);
    if (!pSelf || !pCallbacks) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    vm_write32(pSelf, pCallbacks);
    return CELL_OK;
}

s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pCallbackArg, u32 pAttribute, u32 pResource)
{
    (void)pAttribute; (void)pResource;
    printf("[cellSail] PlayerInitialize2(self=0x%08X alloc=0x%08X cb=0x%08X arg=0x%08X)\n",
           pSelf, pAllocator, pCallback, pCallbackArg);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailPlayer* p = player_of(pSelf);
    for (int i = 0; !p && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_players[i].in_use) p = &s_players[i];
    if (!p) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(p, 0, sizeof *p);
    p->in_use = 1;
    p->self = pSelf;
    p->cb = pCallback;
    p->cb_arg = pCallbackArg;
    if (pAllocator) { p->alloc_funcs = vm_read32(pAllocator); p->alloc_arg = vm_read32(pAllocator + 4); }
    p->state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
    return CELL_OK;
}

static void sail_close(SailPlayer* p)
{
    SailStream* s = p->st;
    p->st = NULL;
    if (s && s->nslot) {
        const SailGfx* g = gfx_of(p->gfx);
        if (g && g->funcs) {
            const u32 fr = vm_read32(g->funcs + 16);          /* free(arg, num, ppFrame) */
            if (fr && g_ps3_guest_caller) g_ps3_guest_caller(fr, g->arg, s->nslot, p->ppframe, 0, 0, 0, 0, 0);
        }
    }
    sail_stream_free(s);
    p->eos_pending = 0;
}

s32 cellSailPlayerFinalize(u32 pSelf)
{
    printf("[cellSail] PlayerFinalize(self=0x%08X)\n", pSelf);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_close(p);
    p->in_use = 0;
    return CELL_OK;
}

/* Not a libsail function; an older name for Finalize this file had. */
s32 cellSailPlayerDestroy(u32 pSelf) { return cellSailPlayerFinalize(pSelf); }

s32 cellSailPlayerSetSoundAdapter(u32 pSelf, u32 index, u32 pAdapter)
{
    printf("[cellSail] PlayerSetSoundAdapter(self=0x%08X index=%u adapter=0x%08X)\n", pSelf, index, pAdapter);
    SailPlayer* p = player_of(pSelf);
    if (p && index == 0) p->snd = pAdapter;
    return CELL_OK;
}

s32 cellSailPlayerSetGraphicsAdapter(u32 pSelf, u32 index, u32 pAdapter)
{
    printf("[cellSail] PlayerSetGraphicsAdapter(self=0x%08X index=%u adapter=0x%08X)\n", pSelf, index, pAdapter);
    SailPlayer* p = player_of(pSelf);
    if (p && index == 0) p->gfx = pAdapter;
    return CELL_OK;
}

s32 cellSailPlayerBoot(u32 pSelf, u64 userParam)
{
    printf("[cellSail] PlayerBoot(self=0x%08X)\n", pSelf);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_state(p, CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_BOOT, 0, userParam);
    sail_state(p, CELL_SAIL_PLAYER_STATE_CLOSED);
    return CELL_OK;
}

s32 cellSailPlayerCreateDescriptor(u32 pSelf, s32 streamType, u32 pMediaInfo, u32 pUri, u32 ppDesc)
{
    (void)pMediaInfo;
    SailPlayer* p = player_of(pSelf);
    char uri[512]; size_t n = 0;
    while (pUri && n < sizeof uri - 1 && (uri[n] = (char)vm_read8(pUri + (u32)n)) != 0) n++;
    uri[n] = 0;
    printf("[cellSail] PlayerCreateDescriptor(self=0x%08X type=%d uri=\"%s\")\n", pSelf, streamType, uri);
    if (!p || !ppDesc) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    snprintf(p->uri, sizeof p->uri, "%s", uri);
    u32 d = sail_alloc(p, 0x100);
    if (!d) d = pSelf;              /* any non-null handle: nothing reads it */
    vm_write32(ppDesc, d);
    return CELL_OK;
}

s32 cellSailPlayerDestroyDescriptor(u32 pSelf, u32 pDesc)
{
    SailPlayer* p = player_of(pSelf);
    if (p && pDesc && pDesc != pSelf) sail_free(p, pDesc);
    if (p && p->desc == pDesc) p->desc = 0;
    return CELL_OK;
}

s32 cellSailPlayerAddDescriptor(u32 pSelf, u32 pDesc)
{
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    p->desc = pDesc;
    return CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(u32 pSelf, u32 ppDesc)
{
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (ppDesc) vm_write32(ppDesc, p->desc);
    p->desc = 0;
    return CELL_OK;
}

s32 cellSailPlayerOpenStream(u32 pSelf)
{
    printf("[cellSail] PlayerOpenStream(self=0x%08X uri=\"%s\")\n", pSelf, player_of(pSelf) ? player_of(pSelf)->uri : "");
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_state(p, CELL_SAIL_PLAYER_STATE_OPEN_TRANSITION);
    sail_close(p);
    SailStream* s = p->uri[0] ? sail_stream_open(p->uri) : NULL;
    const SailGfx* g = gfx_of(p->gfx);
    if (s && g && g->funcs && g_ps3_guest_caller) {
        /* The title's frame buffers: alloc(arg, size, num, ppFrame) fills an
         * array of their addresses, as many as its pool holds. */
        s->frame_bytes = s->w * s->h * 4;
        if (!p->ppframe) p->ppframe = sail_alloc(p, SAIL_SLOTS * 4);
        if (!p->vfmt) p->vfmt = sail_alloc(p, 32);
        if (!p->afmt) p->afmt = sail_alloc(p, 32);
        const u32 makeup = vm_read32(g->funcs + 0), fmt = vm_read32(g->funcs + 8),
                  alloc = vm_read32(g->funcs + 12);
        if (makeup) g_ps3_guest_caller(makeup, g->arg, 0, 0, 0, 0, 0, 0, 0);
        if (p->vfmt) sail_write_vformat(s, p->vfmt);
        if (fmt && p->vfmt) g_ps3_guest_caller(fmt, g->arg, p->vfmt, SAIL_SESSION, 0, 0, 0, 0, 0);
        if (alloc && p->ppframe) {
            for (u32 i = 0; i < SAIL_SLOTS; i++) vm_write32(p->ppframe + 4 * i, 0);
            g_ps3_guest_caller(alloc, g->arg, s->frame_bytes, SAIL_SLOTS, p->ppframe, 0, 0, 0, 0);
            for (u32 i = 0; i < SAIL_SLOTS; i++) {
                const u32 ea = vm_read32(p->ppframe + 4 * i);
                if (!ea) break;
                s->slot[s->nslot].ea = ea;
                s->slot[s->nslot].st = SLOT_FREE;
                s->nslot++;
            }
        }
        const SailSnd* a = snd_of(p->snd);
        if (a && a->funcs) {
            const u32 smake = vm_read32(a->funcs + 0), sfmt = vm_read32(a->funcs + 8);
            if (smake) g_ps3_guest_caller(smake, a->arg, 0, 0, 0, 0, 0, 0, 0);
            if (p->afmt) sail_write_aformat(s, p->afmt);
            if (sfmt && p->afmt) g_ps3_guest_caller(sfmt, a->arg, p->afmt, SAIL_SESSION, 0, 0, 0, 0, 0);
        }
        if (!p->sndbuf) p->sndbuf = sail_alloc(p, 256 * 2 * 4);
        printf("[cellSail] %u frame buffer(s) of %u bytes from the title\n", s->nslot, s->frame_bytes);
        if (s->nslot < 3) {
            printf("[cellSail] too few frame buffers to play into: skipped\n");
            sail_stream_free(s);
            s = NULL;
        }
    } else if (s) {
        printf("[cellSail] no graphics adapter: skipped\n");
        sail_stream_free(s);
        s = NULL;
    }
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        sail_fill_slots(s);
        ReleaseSRWLockExclusive(&s->lock);
        s->thread = CreateThread(NULL, 0, sail_thread, s, 0, NULL);
    }
    p->st = s;
    sail_event(p, CELL_SAIL_EVENT_STREAM_OPENED, 0, 0, 0);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_OPEN_STREAM, 0, 0);
    sail_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(u32 pSelf)
{
    printf("[cellSail] PlayerCloseStream(self=0x%08X)\n", pSelf);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_state(p, CELL_SAIL_PLAYER_STATE_CLOSE_TRANSITION);
    sail_close(p);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_CLOSE_STREAM, 0, 0);
    sail_state(p, CELL_SAIL_PLAYER_STATE_CLOSED);
    return CELL_OK;
}

s32 cellSailPlayerStart(u32 pSelf, u32 pCommand, u64 userParam)
{
    (void)pCommand;
    printf("[cellSail] PlayerStart(self=0x%08X)\n", pSelf);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_state(p, CELL_SAIL_PLAYER_STATE_START_TRANSITION);
    SailStream* s = p->st;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        const s64 t0 = s->afirst >= 0 && s->afirst < s->vfirst ? s->afirst : s->vfirst;
        sail_set_anchor(s, (double)(t0 - s->delay));
        s->running = 1; s->paused = 0;
        ReleaseSRWLockExclusive(&s->lock);
    } else {
        p->eos_pending = 1;          /* nothing to play: it ends at the next frame */
    }
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_START, 0, userParam);
    sail_state(p, CELL_SAIL_PLAYER_STATE_RUNNING);
    sail_event(p, CELL_SAIL_EVENT_SESSION_STARTED, 0, SAIL_SESSION, 0);
    return CELL_OK;
}

s32 cellSailPlayerStop(u32 pSelf, u64 userParam)
{
    printf("[cellSail] PlayerStop(self=0x%08X)\n", pSelf);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_state(p, CELL_SAIL_PLAYER_STATE_STOP_TRANSITION);
    SailStream* s = p->st;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        sail_set_anchor(s, sail_clock(s));
        s->running = 0;
        ReleaseSRWLockExclusive(&s->lock);
    }
    p->eos_pending = 0;
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_STOP, 0, userParam);
    sail_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerSetPaused(u32 pSelf, s32 paused)
{
    printf("[cellSail] PlayerSetPaused(self=0x%08X paused=%d)\n", pSelf, paused);
    SailPlayer* p = player_of(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailStream* s = p->st;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        if (!!paused != s->paused) {
            sail_set_anchor(s, sail_clock(s));
            s->paused = !!paused;
        }
        ReleaseSRWLockExclusive(&s->lock);
    }
    sail_event(p, CELL_SAIL_EVENT_PAUSE_STATE_CHANGED, 0, (u64)!!paused, 0);
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(u32 pSelf)
{
    const SailPlayer* p = player_of(pSelf);
    return p && p->st && p->st->paused ? 1 : 0;
}

s32 cellSailPlayerCancel(u32 pSelf) { (void)pSelf; return CELL_OK; }

s32 cellSailPlayerSetRepeatMode(u32 pSelf, s32 repeatMode, u32 pCommand)
{
    (void)pSelf; (void)pCommand;
    return repeatMode;            /* the mode now in force */
}

/* ---------------------------------------------------------------------------
 * Graphics adapter
 * -----------------------------------------------------------------------*/

s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X arg=0x%08X)\n", pSelf, pCallbacks, pArg);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailGfx* g = gfx_of(pSelf);
    for (int i = 0; !g && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_gfx[i].in_use) g = &s_gfx[i];
    if (!g) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(g, 0, sizeof *g);
    g->in_use = 1; g->self = pSelf; g->funcs = pCallbacks; g->arg = pArg;
    return CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(u32 pSelf)
{
    SailGfx* g = gfx_of(pSelf);
    if (g) g->in_use = 0;
    return CELL_OK;
}

s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    printf("[cellSail] GraphicsAdapterSetPreferredFormat(self=0x%08X fmt=0x%08X)\n", pSelf, pFormat);
    SailGfx* g = gfx_of(pSelf);
    if (g && pFormat) for (u32 i = 0; i < 32; i++) g->pref[i] = vm_read8(pFormat + i);
    if (g && g->pref[0] != CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED && g->pref[0] != 0xFF)
        printf("[cellSail] the title asks for pictures in coding %d; they come as RGBA\n", (s8)g->pref[0]);
    return CELL_OK;
}

/* The picture due now: the latest ready one whose time has come. Returns its
 * size (Bleach takes a result over 255 as a picture), or EMPTY. */
static s32 sail_get_frame(u32 pSelf, u32 pInfo, u32 pPrevInfo)
{
    SailPlayer* p = player_of_gfx(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    SailStream* s = p->st;
    s32 rc = (s32)CELL_SAIL_ERROR_EMPTY;
    int announce = 0;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        const double now = sail_clock(s);
        int best = -1;
        for (u32 i = 0; i < s->nslot; i++)
            if (s->slot[i].st == SLOT_READY && (double)s->slot[i].pts <= now &&
                (best < 0 || s->slot[i].pts > s->slot[best].pts)) best = (int)i;
        if (best >= 0) {
            /* Later pictures stay; the ones it overtakes go back to the pool. */
            for (u32 i = 0; i < s->nslot; i++)
                if (s->slot[i].st == SLOT_READY && s->slot[i].pts < s->slot[best].pts) s->slot[i].st = SLOT_FREE;
            if (s->prev >= 0 && s->prev != s->cur) s->slot[s->prev].st = SLOT_FREE;
            s->prev = s->cur;
            s->cur = best;
            s->slot[best].st = SLOT_SHOWN;
            WakeAllConditionVariable(&s->cv);
            if (sail_log()) printf("[cellSail] picture %lld at %.0f (slot %d)\n", (long long)s->slot[best].pts, now, best);
        }
        if (s->cur >= 0) {
            const SailSlot* c = &s->slot[s->cur];
            vm_write32(pInfo + 0, c->ea);
            vm_write32(pInfo + 4, SAIL_SESSION);
            vm_write32(pInfo + 8, (u32)s->cur);
            vm_write32(pInfo + 12, 0);
            vm_write64(pInfo + 16, (u64)c->pts);
            if (pPrevInfo && s->prev >= 0) {
                vm_write32(pPrevInfo + 0, s->slot[s->prev].ea);
                vm_write32(pPrevInfo + 4, SAIL_SESSION);
                vm_write32(pPrevInfo + 8, (u32)s->prev);
                vm_write32(pPrevInfo + 12, 0);
                vm_write64(pPrevInfo + 16, (u64)s->slot[s->prev].pts);
            }
            rc = (s32)(s->frame_bytes > 256 ? s->frame_bytes : 256);
        }
        /* The end: everything decoded and shown, its time up, the sound out. */
        int waiting = s->npend;
        for (u32 i = 0; i < s->nslot; i++)
            if (s->slot[i].st == SLOT_READY || s->slot[i].st == SLOT_FILLING) waiting++;
        if (!s->eos && s->running && s->eof && !waiting && now >= (double)(s->vlast + s->vdur) &&
            (!s->has_audio || s->aread >= s->awritten)) {
            s->eos = 1;
            announce = 1;
        }
        ReleaseSRWLockExclusive(&s->lock);
    } else if (p->eos_pending) {
        p->eos_pending = 0;
        announce = 1;
    }
    if (announce) {
        printf("[cellSail] end of the stream\n");
        sail_event(p, CELL_SAIL_EVENT_SOURCE_EOS, 0, 0, 0);
        sail_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
    }
    return rc;
}

s32 cellSailGraphicsAdapterGetFrame(u32 pSelf, u32 pInfo)
{
    return pInfo ? sail_get_frame(pSelf, pInfo, 0) : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailGraphicsAdapterGetFrame2(u32 pSelf, u32 pInfo, u32 pPrevInfo, u32 pFlipTime, u64 flags)
{
    (void)flags;
    if (pFlipTime) vm_write64(pFlipTime, 0);
    return pInfo ? sail_get_frame(pSelf, pInfo, pPrevInfo) : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailGraphicsAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    const SailPlayer* p = player_of_gfx(pSelf);
    if (!pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (!p || !p->st) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    sail_write_vformat(p->st, pFormat);
    return CELL_OK;
}

s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u64 pts) { (void)pSelf; (void)pts; return CELL_OK; }

static s32 sail_pts_to_time(const SailStream* s, u64 pts, u32 pTime)
{
    if (!pTime) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (!s) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    const s64 t0 = s->afirst >= 0 && s->afirst < s->vfirst ? s->afirst : s->vfirst;
    const s64 d = (s64)pts - t0;
    vm_write64(pTime, (u64)(d > 0 ? d * 1000000 / 90000 : 0));   /* microseconds */
    return CELL_OK;
}

s32 cellSailGraphicsAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime)
{
    const SailPlayer* p = player_of_gfx(pSelf);
    return sail_pts_to_time(p ? p->st : NULL, pts, pTime);
}

/* ---------------------------------------------------------------------------
 * Sound adapter
 * -----------------------------------------------------------------------*/

s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X arg=0x%08X)\n", pSelf, pCallbacks, pArg);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailSnd* a = snd_of(pSelf);
    for (int i = 0; !a && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_snd[i].in_use) a = &s_snd[i];
    if (!a) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(a, 0, sizeof *a);
    a->in_use = 1; a->self = pSelf; a->funcs = pCallbacks; a->arg = pArg;
    return CELL_OK;
}

s32 cellSailSoundAdapterFinalize(u32 pSelf)
{
    SailSnd* a = snd_of(pSelf);
    if (a) a->in_use = 0;
    return CELL_OK;
}

s32 cellSailSoundAdapterSetPreferredFormat(u32 pSelf, u32 pFormat) { (void)pSelf; (void)pFormat; return CELL_OK; }

/* The next `samples` samples, interleaved float32 stereo, short of the end
 * padded with silence. Returns their size, at least 256 (Bleach takes 255
 * or less as no sound), or EMPTY when there is none. */
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo)
{
    SailPlayer* p = player_of_snd(pSelf);
    if (!p || !pInfo) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailStream* s = p->st;
    if (!s || !p->sndbuf || !samples) return (s32)CELL_SAIL_ERROR_EMPTY;
    if (samples > 256) samples = 256;
    float out[256 * 2];
    AcquireSRWLockExclusive(&s->lock);
    if (!s->running || s->paused || !s->has_audio || s->aread >= s->awritten) {
        ReleaseSRWLockExclusive(&s->lock);
        return (s32)CELL_SAIL_ERROR_EMPTY;
    }
    const u64 pos = s->aread;
    u32 n = 0;
    for (; n < samples && s->aread < s->awritten; n++, s->aread++) {
        const float* q = s->ring + (size_t)(s->aread % SAIL_RING) * 2;
        out[n * 2] = q[0]; out[n * 2 + 1] = q[1];
    }
    for (u32 i = n; i < samples; i++) out[i * 2] = out[i * 2 + 1] = 0.0f;
    /* The pictures follow this sound: where it is, less what the title holds
     * before it is heard. Small differences are eased in. */
    const double heard = (double)s->afirst + (double)s->aread * 90000.0 / (double)s->afs - (double)s->delay;
    const double wall = sail_clock(s);
    sail_set_anchor(s, (heard - wall > 3000.0 || wall - heard > 3000.0) ? heard : wall + (heard - wall) * 0.1);
    WakeAllConditionVariable(&s->cv);
    const u32 afs = s->afs;
    ReleaseSRWLockExclusive(&s->lock);
    for (u32 i = 0; i < samples * 2; i++) {
        u32 bits; memcpy(&bits, &out[i], 4);
        vm_write32(p->sndbuf + 4 * i, bits);
    }
    vm_write32(pInfo + 0, p->sndbuf);
    vm_write32(pInfo + 4, SAIL_SESSION);
    vm_write32(pInfo + 8, 0);
    vm_write32(pInfo + 12, 0);
    vm_write64(pInfo + 16, (u64)(s->afirst + (s64)(pos * 90000 / afs)));
    if (sail_log()) printf("[cellSail] sound %u of %u samples at %llu\n", n, samples, (unsigned long long)pos);
    const u32 bytes = samples * 2 * 4;
    return (s32)(bytes > 256 ? bytes : 256);
}

s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    const SailPlayer* p = player_of_snd(pSelf);
    if (!pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_write_aformat(p ? p->st : NULL, pFormat);
    return CELL_OK;
}

s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u64 pts) { (void)pSelf; (void)pts; return CELL_OK; }

s32 cellSailSoundAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime)
{
    const SailPlayer* p = player_of_snd(pSelf);
    return sail_pts_to_time(p ? p->st : NULL, pts, pTime);
}
