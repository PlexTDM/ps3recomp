/*
 * ps3recomp - cellSail HLE: the system's movie player (libsail)
 *
 * The title gives the player a URI and two adapters of its own:
 *
 *   - the graphics adapter: the title allocates the frame buffers (its alloc
 *     callback fills an array with their addresses) and, every frame it shows
 *     the movie, calls cellSailGraphicsAdapterGetFrame for the picture to
 *     draw;
 *   - the sound adapter: the title's own mixer calls
 *     cellSailSoundAdapterGetFrame for the next `samples` samples of
 *     interleaved float32 sound.
 *
 * Progress reaches the title through one callback, f(arg, event, arg0, arg1),
 * the event a u64 of major << 32 | minor: PLAYER_STATE_CHANGED (arg0 the new
 * state), PLAYER_CALL_COMPLETED (minor the call, arg0 its result, arg1 the
 * call's userParam) and SOURCE_EOS once the stream has played out. A title
 * waits for the completions: Bleach: Soul Resurreccion polls for Start's with
 * a 1 ms sleep, and takes SOURCE_EOS followed by the OPENED state as the end
 * of the movie. Each call's events are delivered inside the call; the end of
 * the stream is noticed, and announced, in GetFrame.
 *
 * Only a PAMF file (the PS3's MPEG program stream behind a PAMF header) with
 * H.264 video and ATRAC3plus sound is played, which is what a disc movie is.
 * A reader thread takes it a 2048-byte pack at a time:
 *
 *   - the video is cut into access units at its access unit delimiters and
 *     decoded by FFmpeg (vdec_ffmpeg.c), each picture then stored into one of
 *     the title's frame buffers, in the format it asked for;
 *   - the ATRAC3plus frames (in private stream 1, behind a 4-byte header,
 *     each frame behind an 8-byte 0x0FD0 header) are decoded by FFmpeg into a
 *     ring that the sound adapter drains.
 *
 * Without FFmpeg a stream opens and then ends at once, as a skipped movie.
 *
 * The pictures follow a clock (90 kHz, like the PTSs) set by the sound the
 * title has taken, less the time its mixer holds that sound before it is
 * heard (SAIL_AV_DELAY_MS, default 100: Bleach keeps 4096 samples, 85 ms, in
 * its stream buffer). Between two sound pulls the clock runs on the host's
 * timer; without sound the host's timer alone drives it. Pictures are chosen
 * by that clock read on the vblank grid (sail_video_time), so they fall on
 * the game's frames in an even pattern.
 *
 *   SAIL_LOG=1     a line per picture decoded and shown and per sound pull
 *                  (2: per GetFrame call too)
 *   SAIL_SKIP=1    every stream ends as it starts
 */

#include "ps3emu/guest_call.h"
#include "cellSail.h"
#include "cellVdec.h"          /* CELL_VDEC_CODEC_TYPE_AVC */
#include "vdec_ffmpeg.h"
#include "ffmpeg_runtime.h"
#include "../../runtime/platform/win32_compat.h"   /* threads, SRWLOCK, QPC */
#include "../../runtime/ppu/ppu_memory.h"          /* vm_read*, vm_write* */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ppu_loader.cpp: call a guest function and return its r3. */
uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
/* sys_fs.c: the host file a guest path names. */
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

/* SAIL_LOG: 1 a line per picture and sound pull, 2 per GetFrame call too. */
static int sail_verbose(void)
{
    static int level = -1;
    if (level < 0) {
        const char* e = getenv("SAIL_LOG");
        level = e ? (atoi(e) > 1 ? 2 : 1) : 0;
    }
    return level;
}

/* Host milliseconds, for the log. */
static double sail_host_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

#define PTS_HZ          90000
#define PACK_SIZE       2048

#define SAIL_SLOTS      8            /* frame buffers asked of the title */
#define SAIL_WAITING    16           /* decoded pictures waiting for a buffer: FFmpeg
                                      * hands over ~11 at once as the stream starts */
#define SAIL_RING       (48000 * 4)  /* the sound ring, in stereo sample frames */
#define SAIL_SOUND_MAX  256          /* sample frames a sound pull hands over at most */
#define SAIL_SESSION    1u

/* Bleach takes a GetFrame result of 255 or less as "nothing this time", for
 * pictures and sound alike: a result is a size, and never less than this. */
#define SAIL_MIN_RESULT 256

/* The title's callback tables (arrays of function descriptor addresses). */
enum { ALLOCATOR_ALLOC = 0, ALLOCATOR_FREE = 4 };               /* CellSailMemAllocatorFuncs */
enum { GFX_MAKEUP = 0, GFX_CLEANUP = 4, GFX_FORMAT_CHANGED = 8,  /* CellSailGraphicsAdapterFuncs */
       GFX_ALLOC_FRAME = 12, GFX_FREE_FRAME = 16 };
enum { SOUND_MAKEUP = 0, SOUND_CLEANUP = 4, SOUND_FORMAT_CHANGED = 8 };  /* CellSailSoundAdapterFuncs */

/* ---------------------------------------------------------------------------
 * The stream: demuxer, decoders, frame buffers, sound ring
 * -----------------------------------------------------------------------*/

enum { SLOT_FREE = 0, SLOT_FILLING, SLOT_READY, SLOT_SHOWN };
enum { AUDIO_UNTRIED = 0, AUDIO_PLAYING = 1, AUDIO_FAILED = -1 };

/* One of the title's frame buffers. */
typedef struct { u32 ea; s64 pts; int state; } SailSlot;

/* A decoded picture waiting for a frame buffer. */
typedef struct { u8* yuv; u32 width, height; s64 pts; } SailPicture;

/* Bytes of an elementary stream not yet cut into units. */
typedef struct { u8* data; size_t len, cap; } SailBytes;

typedef struct SailStream {
    FILE* file;
    u64 read_pos, data_end;             /* file offsets of the stream's data */

    /* Video, on the reader thread. */
    VdecFf* video_decoder;
    SailBytes video_es;
    size_t au_start;                    /* where the current access unit starts in video_es */
    int au_open;                        /* an access unit has started */
    s64 au_pts;                         /* its PTS; -1: none */
    s64 pes_pts;                        /* the last PES's PTS, for the next access unit */
    double next_pts;                    /* the PTS the next picture out of the decoder has
                                         * unless it brings its own; -1: none yet */
    int prev_stamped;                   /* the last picture out brought its own PTS */
    double frame_ticks;                 /* a picture's duration, 90 kHz */

    /* Sound, on the reader thread. */
    SailBytes audio_es;
    int audio_state;
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    const PS3FFmpeg* ff;
    AVCodecContext* audio_codec;
    AVPacket* audio_packet;
    AVFrame* audio_frame;
#endif
    u8* audio_input;                    /* a frame plus FFmpeg's input padding */

    /* Shared with the title's threads, under the lock. */
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    SailPicture waiting[SAIL_WAITING];
    int nwaiting;
    SailSlot slots[SAIL_SLOTS];
    u32 nslots, frame_bytes;
    int shown, previous;                /* the slots GetFrame last handed out; -1: none */
    u32 width, height;
    s64 first_video_pts, last_video_pts;
    float* ring;                        /* sound, interleaved stereo */
    u64 ring_written, ring_read;        /* sample frames, from the start */
    int audio_channels;
    u32 audio_rate;
    s64 first_audio_pts;
    int has_audio;
    int eof, eos, running, paused, stop;
    double clock_base;                  /* the clock, 90 kHz, at clock_base_qpc */
    LARGE_INTEGER clock_base_qpc, qpc_freq;
    double video_time;                  /* the pictures' clock, on the vblank grid */
    LARGE_INTEGER video_time_qpc;       /* the grid point it was last advanced to */
    int video_time_set;
    s64 av_delay;                       /* SAIL_AV_DELAY_MS, 90 kHz */
    HANDLE thread;
} SailStream;

static s64 read_pes_pts(const u8* p)
{
    return ((s64)((p[0] >> 1) & 7) << 30) | ((s64)p[1] << 22) | ((s64)(p[2] >> 1) << 15) |
           ((s64)p[3] << 7) | (s64)(p[4] >> 1);
}

static u32 read_be32(const u8* p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static int bytes_append(SailBytes* b, const u8* p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        while (cap < b->len + n) cap *= 2;
        u8* data = (u8*)realloc(b->data, cap);
        if (!data) return 0;
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

static void bytes_drop_front(SailBytes* b, size_t n)
{
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

/* An ATRAC3plus frame header's frame size: (its 10-bit field + 1) * 8. */
static size_t atrac3p_frame_bytes(const u8* header)
{
    return (size_t)(((((header[2] & 3) << 8) | header[3]) + 1) * 8);
}

static void sail_fill_slots(SailStream* s);

/* The token of an access unit whose PES brought no PTS. */
#define SAIL_NO_PTS 0xFFFFFFFFu

/* A picture's PTS. The decoder hands pictures over in display order, each
 * with the token of the access unit it was decoded from: that access unit's
 * PTS, when its PES had one. Most have none (the movie's muxer stamps one
 * access unit in about ten), and those follow the picture before by its
 * duration, which the decoder knows from the stream (Bleach's movie is
 * 23.976 fps film, 3753.75 ticks a picture). Counting in display order
 * matters: with B-frames the access units are stored out of display order,
 * and times counted in stored order came out swapped in pairs. */
static s64 sail_picture_pts(SailStream* s, const VdecFfPicture* pic)
{
    double pts;
    if (pic->token != SAIL_NO_PTS) {
        pts = (double)pic->token;
        /* A stream that gives no frame rate: the step between two stamped
         * pictures in a row, if smaller, is a picture's duration. */
        if (pic->duration <= 0 && s->prev_stamped && s->next_pts >= 0) {
            const double step = pts - (s->next_pts - s->frame_ticks);
            if (step > 0 && step < s->frame_ticks) s->frame_ticks = step;
        }
        s->prev_stamped = 1;
    } else {
        pts = s->next_pts >= 0 ? s->next_pts : 0;
        s->prev_stamped = 0;
    }
    if (pic->duration > 0) s->frame_ticks = pic->duration;
    s->next_pts = pts + s->frame_ticks;
    return (s64)(pts + 0.5);
}

/* A picture from the decoder, in display order. It joins the pictures
 * waiting for a frame buffer, which are kept in PTS order, so the next one
 * due is always the next one stored. Runs on the reader thread, which is
 * also the one that stores pictures into the frame buffers: with the queue
 * full it does that here, and waits for the title to hand a buffer back. */
static void sail_on_picture(void* arg, const VdecFfPicture* pic)
{
    SailStream* s = (SailStream*)arg;
    AcquireSRWLockExclusive(&s->lock);
    const s64 pts = sail_picture_pts(s, pic);
    if (sail_verbose())
        printf("[cellSail] %.1f ms: decoded %lld%s (%d waiting, %u buffers)\n", sail_host_ms(), (long long)pts,
               pic->token == SAIL_NO_PTS ? "" : " (stamped)", s->nwaiting, s->nslots);
    for (;;) {
        sail_fill_slots(s);
        /* Before the buffers exist (while the stream is being opened)
         * nothing empties the queue: a picture past it is dropped instead. */
        if (s->nwaiting < SAIL_WAITING || s->stop || !s->nslots) break;
        SleepConditionVariableSRW(&s->cv, &s->lock, 50, 0);
    }
    if (s->stop || s->nwaiting == SAIL_WAITING) {
        ReleaseSRWLockExclusive(&s->lock);
        free(pic->yuv);
        return;
    }
    int at = s->nwaiting;
    while (at > 0 && s->waiting[at - 1].pts > pts) at--;
    memmove(&s->waiting[at + 1], &s->waiting[at], sizeof s->waiting[0] * (size_t)(s->nwaiting - at));
    s->nwaiting++;
    SailPicture* p = &s->waiting[at];
    p->yuv = pic->yuv;
    p->width = pic->width;
    p->height = pic->height;
    p->pts = pts;
    if (!s->width) {
        s->width = pic->width;
        s->height = pic->height;
    }
    if (s->first_video_pts < 0 || pts < s->first_video_pts) s->first_video_pts = pts;
    if (pts > s->last_video_pts) s->last_video_pts = pts;
    ReleaseSRWLockExclusive(&s->lock);
}

static void sail_decode_access_unit(SailStream* s, const u8* au, size_t n)
{
    const u32 token = s->au_pts >= 0 ? (u32)s->au_pts : SAIL_NO_PTS;
    if (s->video_decoder) vdec_ff_decode(s->video_decoder, au, (u32)n, token, sail_on_picture, s);
}

/* Video payload: cut it into access units at each access unit delimiter
 * (00 00 01 09). An access unit takes the PTS of the PES it starts in. */
static void sail_video_bytes(SailStream* s, const u8* p, size_t n, s64 pts)
{
    if (pts >= 0) s->pes_pts = pts;
    SailBytes* es = &s->video_es;
    const size_t from = es->len;
    if (!bytes_append(es, p, n)) return;
    /* A delimiter can straddle the previous payload's end. */
    for (size_t k = from > 3 ? from - 3 : 0; k + 4 <= es->len; k++) {
        const u8* q = es->data + k;
        if (q[0] || q[1] || q[2] != 1 || (q[3] & 0x1F) != 9) continue;
        if (s->au_open && k > s->au_start) sail_decode_access_unit(s, es->data + s->au_start, k - s->au_start);
        s->au_start = k;
        s->au_open = 1;
        s->au_pts = s->pes_pts;
        s->pes_pts = -1;
        k += 3;
    }
    if (s->au_open && s->au_start) {
        bytes_drop_front(es, s->au_start);
        s->au_start = 0;
    }
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME
/* The ATRAC3plus decoder, set up from the first frame's header. */
static int sail_audio_open(SailStream* s, const u8* header)
{
    static const u32 rates[8] = { 32000, 44100, 48000, 88200, 96000, 0, 0, 0 };
    static const int channels[8] = { 0, 1, 2, 3, 4, 6, 7, 8 };
    const u32 rate = rates[header[2] >> 5];
    const int nch = channels[(header[2] >> 2) & 7];
    const int frame_bytes = (int)atrac3p_frame_bytes(header);
    if (!s->ff || !rate || !nch || nch > 2) {
        printf("[cellSail] ATRAC3plus %u Hz, %d channel(s): not played\n", rate, nch);
        return -1;
    }
    const AVCodec* codec = s->ff->avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!codec) {
        printf("[cellSail] libavcodec has no ATRAC3plus decoder\n");
        return -1;
    }
    s->audio_codec = s->ff->avcodec_alloc_context3(codec);
    s->audio_packet = s->ff->av_packet_alloc();
    s->audio_frame = s->ff->av_frame_alloc();
    s->audio_input = (u8*)calloc(1, 0x2000 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s->audio_codec || !s->audio_packet || !s->audio_frame || !s->audio_input) return -1;
    s->audio_codec->sample_rate = (int)rate;
    s->audio_codec->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    s->audio_codec->ch_layout.nb_channels = nch;
    s->audio_codec->block_align = frame_bytes;
    if (s->ff->avcodec_open2(s->audio_codec, codec, NULL) < 0) {
        printf("[cellSail] could not open the ATRAC3plus decoder\n");
        return -1;
    }
    AcquireSRWLockExclusive(&s->lock);
    s->audio_channels = nch;
    s->audio_rate = rate;
    ReleaseSRWLockExclusive(&s->lock);
    printf("[cellSail] ATRAC3plus: %u Hz, %d channel(s), %d-byte frames\n", rate, nch, frame_bytes);
    return 0;
}

/* Decode one frame into the ring, as stereo (mono goes to both sides). */
static void sail_audio_frame(SailStream* s, const u8* data, int n)
{
    memcpy(s->audio_input, data, (size_t)n);
    memset(s->audio_input + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s->audio_packet->data = s->audio_input;
    s->audio_packet->size = n;
    const int r = s->ff->avcodec_send_packet(s->audio_codec, s->audio_packet);
    s->audio_packet->data = NULL;
    s->audio_packet->size = 0;
    if (r < 0) return;
    while (s->ff->avcodec_receive_frame(s->audio_codec, s->audio_frame) >= 0) {
        const AVFrame* f = s->audio_frame;
        const int nch = s->audio_channels;
        const int planar = f->format == AV_SAMPLE_FMT_FLTP;
        if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT) {
            AcquireSRWLockExclusive(&s->lock);
            for (int i = 0; i < f->nb_samples && s->ring_written - s->ring_read < SAIL_RING; i++) {
                float* out = s->ring + (size_t)(s->ring_written % SAIL_RING) * 2;
                for (int c = 0; c < 2; c++) {
                    const int src = c < nch ? c : 0;
                    out[c] = planar ? ((const float*)f->extended_data[src])[i]
                                    : ((const float*)f->extended_data[0])[i * nch + src];
                }
                s->ring_written++;
            }
            ReleaseSRWLockExclusive(&s->lock);
        }
        s->ff->av_frame_unref(s->audio_frame);
    }
}
#endif

/* Sound payload (private stream 1, past its 4-byte header): ATRAC3plus
 * frames, each behind an 8-byte header that starts 0x0F 0xD0. */
static void sail_audio_bytes(SailStream* s, const u8* p, size_t n, s64 pts)
{
    if (s->audio_state == AUDIO_FAILED) return;
    AcquireSRWLockExclusive(&s->lock);
    if (s->first_audio_pts < 0 && pts >= 0) s->first_audio_pts = pts;
    ReleaseSRWLockExclusive(&s->lock);
    SailBytes* es = &s->audio_es;
    if (!bytes_append(es, p, n)) return;
    size_t at = 0;
    while (es->len - at >= 8) {
        const u8* header = es->data + at;
        if (header[0] != 0x0F || header[1] != 0xD0) { at++; continue; }   /* resync */
        const size_t len = 8 + atrac3p_frame_bytes(header);
        if (es->len - at < len) break;
#ifdef PS3RECOMP_FFMPEG_RUNTIME
        if (s->audio_state == AUDIO_UNTRIED) {
            s->audio_state = sail_audio_open(s, header) == 0 ? AUDIO_PLAYING : AUDIO_FAILED;
            AcquireSRWLockExclusive(&s->lock);
            s->has_audio = s->audio_state == AUDIO_PLAYING;
            ReleaseSRWLockExclusive(&s->lock);
        }
        if (s->audio_state == AUDIO_PLAYING) sail_audio_frame(s, header + 8, (int)(len - 8));
#endif
        at += len;
    }
    bytes_drop_front(es, at);
}

/* Read and demux one pack. 0 at the end of the stream's data. */
static int sail_demux_pack(SailStream* s)
{
    u8 pack[PACK_SIZE];
    if (s->read_pos + sizeof pack > s->data_end || fread(pack, 1, sizeof pack, s->file) != sizeof pack) return 0;
    s->read_pos += sizeof pack;
    size_t i = 0;
    while (i + 6 <= sizeof pack) {
        if (pack[i] || pack[i + 1] || pack[i + 2] != 1) break;
        const u8 id = pack[i + 3];
        if (id == 0xBA) { i += 14 + (pack[i + 13] & 7); continue; }   /* MPEG-2 pack header */
        if (id == 0xB9) break;                                          /* end code */
        const size_t len = ((size_t)pack[i + 4] << 8) | pack[i + 5];
        if (i + 6 + len > sizeof pack) break;
        const u8* pes = pack + i + 6;
        if ((id == 0xE0 || id == 0xBD) && len >= 3 && 3u + pes[2] <= len) {
            const size_t header_len = pes[2];
            const s64 pts = ((pes[1] & 0x80) && header_len >= 5) ? read_pes_pts(pes + 3) : -1;
            const u8* payload = pes + 3 + header_len;
            const size_t payload_len = len - 3 - header_len;
            if (id == 0xE0)
                sail_video_bytes(s, payload, payload_len, pts);
            else if (payload_len > 4 && (payload[0] & 0xF0) == 0)   /* ATRAC3plus: sub-streams 0x00..0x0F */
                sail_audio_bytes(s, payload + 4, payload_len - 4, pts);
        }
        i += 6 + len;
    }
    return 1;
}

/* The first sound's PTS, by a look ahead in the file (up to 8 MB) that
 * decodes nothing; -1 if there is none that early. The stream's clock starts
 * at whichever of the first picture and the first sound comes first, and
 * the sound is stored some way behind the pictures: started from the first
 * picture, the clock stepped back by ~0.4 s once the sound came in, and the
 * pictures stood still while it caught up. */
static s64 sail_scan_first_audio_pts(SailStream* s)
{
    s64 found = -1;
    u8 pack[PACK_SIZE];
    for (u64 pos = s->read_pos; found < 0 && pos + sizeof pack <= s->data_end && pos < s->read_pos + (8u << 20);
         pos += sizeof pack) {
        if (fread(pack, 1, sizeof pack, s->file) != sizeof pack) break;
        size_t i = 0;
        while (found < 0 && i + 6 <= sizeof pack) {
            if (pack[i] || pack[i + 1] || pack[i + 2] != 1) break;
            const u8 id = pack[i + 3];
            if (id == 0xBA) { i += 14 + (pack[i + 13] & 7); continue; }
            if (id == 0xB9) break;
            const size_t len = ((size_t)pack[i + 4] << 8) | pack[i + 5];
            if (i + 6 + len > sizeof pack) break;
            const u8* pes = pack + i + 6;
            if (id == 0xBD && len >= 3 && 3u + pes[2] < len && (pes[1] & 0x80) && pes[2] >= 5 &&
                (pes[3 + pes[2]] & 0xF0) == 0)
                found = read_pes_pts(pes + 3);
            i += 6 + len;
        }
    }
    fseek(s->file, (long)s->read_pos, SEEK_SET);
    return found;
}

/* Store waiting pictures into free frame buffers. Called with the lock held;
 * it is dropped around each conversion. */
static void sail_fill_slots(SailStream* s)
{
    while (s->nwaiting && s->nslots) {
        int k = -1;
        for (u32 i = 0; i < s->nslots; i++)
            if (s->slots[i].state == SLOT_FREE && (int)i != s->shown && (int)i != s->previous) { k = (int)i; break; }
        if (k < 0) return;
        const SailPicture pic = s->waiting[0];
        memmove(s->waiting, s->waiting + 1, sizeof s->waiting[0] * (size_t)(--s->nwaiting));
        s->slots[k].state = SLOT_FILLING;
        WakeAllConditionVariable(&s->cv);
        ReleaseSRWLockExclusive(&s->lock);
        if (pic.width == s->width && pic.height == s->height)
            vdec_store_picture(s->slots[k].ea, pic.yuv, pic.width, pic.height,
                               CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED, 1 /* BT.709 */, 0xFF);
        free(pic.yuv);
        AcquireSRWLockExclusive(&s->lock);
        s->slots[k].pts = pic.pts;
        s->slots[k].state = SLOT_READY;
    }
}

static DWORD WINAPI sail_reader_thread(LPVOID arg)
{
    SailStream* s = (SailStream*)arg;
    AcquireSRWLockExclusive(&s->lock);
    while (!s->stop) {
        sail_fill_slots(s);
        /* Read on while fewer than two pictures wait for a buffer, or up to
         * the queue's size while the sound runs low (under half a second):
         * the file carries a picture's sound some way after it, and the title
         * pulls sound in bursts. */
        const u64 buffered = s->ring_written - s->ring_read;
        const int ring_has_room = !s->has_audio || buffered + 3 * 2048 <= SAIL_RING;
        /* Sound is on its way from the start when the look ahead found some
         * (sail_scan_first_audio_pts), before its decoder is open. */
        const int expects_audio = s->has_audio || (s->first_audio_pts >= 0 && s->audio_state != AUDIO_FAILED);
        const u64 rate = s->audio_rate ? s->audio_rate : 48000;
        const int sound_low = expects_audio && buffered < rate / 2;
        const int want_more = s->nwaiting < 2 || (sound_low && s->nwaiting < SAIL_WAITING - 2);
        if (s->eof || !ring_has_room || !want_more) {
            SleepConditionVariableSRW(&s->cv, &s->lock, 20, 0);
            continue;
        }
        ReleaseSRWLockExclusive(&s->lock);
        const int more = sail_demux_pack(s);
        if (!more) {
            /* The end: the last access unit, then what the decoder holds. */
            if (s->au_open && s->video_es.len > s->au_start) {
                sail_decode_access_unit(s, s->video_es.data + s->au_start, s->video_es.len - s->au_start);
                s->au_open = 0;
            }
            if (s->video_decoder) vdec_ff_drain(s->video_decoder, sail_on_picture, s);
        }
        AcquireSRWLockExclusive(&s->lock);
        if (!more) s->eof = 1;
    }
    ReleaseSRWLockExclusive(&s->lock);
    return 0;
}

/* The clock, 90 kHz. Under the lock. */
static double sail_clock(SailStream* s)
{
    if (!s->running || s->paused) return s->clock_base;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return s->clock_base +
           (double)(now.QuadPart - s->clock_base_qpc.QuadPart) * PTS_HZ / (double)s->qpc_freq.QuadPart;
}

/* Set the clock to t now. Under the lock. */
static void sail_set_clock(SailStream* s, double t)
{
    s->clock_base = t;
    QueryPerformanceCounter(&s->clock_base_qpc);
}

/* The PTS the stream starts at: its first picture's, or its first sound's
 * if that comes earlier. */
static s64 sail_start_pts(const SailStream* s)
{
    return s->first_audio_pts >= 0 && s->first_audio_pts < s->first_video_pts ? s->first_audio_pts
                                                                               : s->first_video_pts;
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
    if (s->video_decoder) vdec_ff_close(s->video_decoder);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (s->audio_codec) s->ff->avcodec_free_context(&s->audio_codec);
    if (s->audio_packet) s->ff->av_packet_free(&s->audio_packet);
    if (s->audio_frame) s->ff->av_frame_free(&s->audio_frame);
#endif
    for (int i = 0; i < s->nwaiting; i++) free(s->waiting[i].yuv);
    if (s->file) fclose(s->file);
    free(s->video_es.data);
    free(s->audio_es.data);
    free(s->audio_input);
    free(s->ring);
    free(s);
}

/* Open the file and decode up to its first picture, so the frame buffers
 * can be asked for at the stream's size. NULL when there is nothing to play
 * (no file, not a PAMF, no FFmpeg): the stream then ends as it starts. */
static SailStream* sail_stream_open(const char* uri)
{
    if (getenv("SAIL_SKIP")) return NULL;
    const char* path = uri;
    if (!strncmp(path, "x-cell-fs://", 12)) path += 12;
    char host[1024];
    sys_fs_translate_path(path, host, (int)sizeof host);
    FILE* f = fopen(host, "rb");
    if (!f) {
        printf("[cellSail] cannot open %s (%s)\n", path, host);
        return NULL;
    }
    /* The PAMF header: the data's offset and size, in packs, at +8 and +12. */
    u8 header[16];
    if (fread(header, 1, sizeof header, f) != sizeof header || memcmp(header, "PAMF", 4)) {
        printf("[cellSail] %s is not a PAMF stream\n", path);
        fclose(f);
        return NULL;
    }
    SailStream* s = (SailStream*)calloc(1, sizeof *s);
    if (!s) {
        fclose(f);
        return NULL;
    }
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->cv);
    QueryPerformanceFrequency(&s->qpc_freq);
    s->file = f;
    s->read_pos = (u64)read_be32(header + 8) * PACK_SIZE;
    s->data_end = s->read_pos + (u64)read_be32(header + 12) * PACK_SIZE;
    fseek(f, (long)s->read_pos, SEEK_SET);
    s->shown = s->previous = -1;
    s->au_pts = s->pes_pts = s->next_pts = -1;
    s->first_video_pts = s->last_video_pts = -1;
    s->first_audio_pts = sail_scan_first_audio_pts(s);
    s->frame_ticks = 3003;                                  /* 29.97 fps until the stream says */
    {
        const char* e = getenv("SAIL_AV_DELAY_MS");
        s->av_delay = (s64)(e ? atoi(e) : 100) * (PTS_HZ / 1000);
    }
    s->ring = (float*)calloc((size_t)SAIL_RING * 2, sizeof(float));
    s->video_decoder = vdec_ff_open(CELL_VDEC_CODEC_TYPE_AVC);
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    s->ff = ps3_ffmpeg_get();
#endif
    if (!s->video_decoder || !s->ring) {
        printf("[cellSail] no H.264 decoder (FFmpeg): %s is skipped\n", path);
        sail_stream_free(s);
        return NULL;
    }
    unsigned packs = 0;
    while (!s->nwaiting && sail_demux_pack(s)) packs++;
    if (!s->nwaiting) {
        printf("[cellSail] %s: no picture decoded\n", path);
        sail_stream_free(s);
        return NULL;
    }
    printf("[cellSail] %s: H.264 %ux%u, a frame every %.2f ms%s; the first picture after %u packs\n", path,
           s->width, s->height, (double)s->frame_ticks / (PTS_HZ / 1000), s->has_audio ? ", ATRAC3plus sound" : "",
           packs);
    fflush(stdout);
    return s;
}

/* ---------------------------------------------------------------------------
 * Players and adapters
 * -----------------------------------------------------------------------*/

typedef struct SailGfx { int in_use; u32 self, funcs, arg; u8 preferred[32]; } SailGfx;
typedef struct SailSnd { int in_use; u32 self, funcs, arg; } SailSnd;

typedef struct SailPlayer {
    int in_use;
    u32 self;
    u32 alloc_funcs, alloc_arg;   /* the title's memory allocator */
    u32 cb, cb_arg;               /* its event callback */
    u32 gfx, snd;                 /* its adapters */
    u32 desc;                     /* the descriptor added */
    char uri[512];
    int state;
    SailStream* stream;
    /* Guest memory of the player's, from the title's allocator: the frame
     * buffer address array, the video and audio formats handed to the
     * adapters, and the sound handed out by GetFrame. */
    u32 frame_array, video_format, audio_format, sound_buffer;
    int eos_pending;              /* SOURCE_EOS + OPENED still to announce */
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];
static SailGfx s_gfx[CELL_SAIL_PLAYER_MAX];
static SailSnd s_snd[CELL_SAIL_PLAYER_MAX];

static SailPlayer* find_player(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].self == self) return &s_players[i];
    return NULL;
}

static SailPlayer* find_player_by_gfx(u32 gfx)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].gfx == gfx) return &s_players[i];
    return NULL;
}

static SailPlayer* find_player_by_snd(u32 snd)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_players[i].in_use && s_players[i].snd == snd) return &s_players[i];
    return NULL;
}

static SailGfx* find_gfx(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_gfx[i].in_use && s_gfx[i].self == self) return &s_gfx[i];
    return NULL;
}

static SailSnd* find_snd(u32 self)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_snd[i].in_use && s_snd[i].self == self) return &s_snd[i];
    return NULL;
}

/* An event to the title's callback, on the calling thread. */
static void sail_event(const SailPlayer* p, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (!p || !p->cb || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(p->cb, p->cb_arg, ((u64)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
}

static void sail_set_state(SailPlayer* p, int state)
{
    p->state = state;
    sail_event(p, CELL_SAIL_EVENT_PLAYER_STATE_CHANGED, 0, (u64)state, 0);
}

/* Call one of the title's callbacks from a table of function descriptors;
 * nothing when the entry is empty. */
static void call_title(u32 table, u32 entry, u64 a0, u64 a1, u64 a2, u64 a3)
{
    const u32 fn = vm_read32(table + entry);
    if (fn && g_ps3_guest_caller) g_ps3_guest_caller(fn, a0, a1, a2, a3, 0, 0, 0, 0);
}

/* The argument the allocator's callbacks are given. Bleach's allocator has
 * no argument of its own (0), and its callbacks need the player's callback
 * argument, so that is what they get then. */
static u32 sail_alloc_arg(const SailPlayer* p)
{
    return p->alloc_arg ? p->alloc_arg : p->cb_arg;
}

/* Zeroed guest memory from the title's allocator: alloc(arg, boundary, size). */
static u32 sail_alloc(const SailPlayer* p, u32 size)
{
    if (!p->alloc_funcs) return 0;
    const u32 fn = vm_read32(p->alloc_funcs + ALLOCATOR_ALLOC);
    if (!fn) return 0;
    const u32 ea = (u32)ppu_guest_call(fn, sail_alloc_arg(p), 128, size, 0, 0, 0, 0, 0);
    if (ea)
        for (u32 i = 0; i < size; i += 4) vm_write32(ea + i, 0);
    return ea;
}

static void sail_free(const SailPlayer* p, u32 ea)
{
    if (!ea || !p->alloc_funcs) return;
    const u32 fn = vm_read32(p->alloc_funcs + ALLOCATOR_FREE);
    if (fn) ppu_guest_call(fn, sail_alloc_arg(p), 128, ea, 0, 0, 0, 0, 0);
}

/* CellSailVideoFormat of the open stream. */
static void sail_write_video_format(const SailStream* s, u32 ea)
{
    /* CELL_SAIL_VIDEO_FRAME_RATE_*, 0 to 7: 23.976, 24, 25, 29.97, 30, 50,
     * 59.94 and 60 fps; the one nearest a picture's duration. */
    static const double durations[8] = { 3753.75, 3750, 3600, 3003, 3000, 1800, 1501.5, 1500 };
    u8 frame_rate = 0;
    for (u8 i = 1; i < 8; i++) {
        const double di = durations[i] - s->frame_ticks, best = durations[frame_rate] - s->frame_ticks;
        if (di * di < best * best) frame_rate = i;
    }
    vm_write8(ea + 0, CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED);
    vm_write8(ea + 1, 0);                               /* progressive */
    vm_write8(ea + 2, 8);                               /* bits per colour */
    vm_write8(ea + 3, frame_rate);
    vm_write16(ea + 4, (u16)s->width);
    vm_write16(ea + 6, (u16)s->height);
    vm_write32(ea + 8, s->width * 4);                   /* pitch */
    vm_write32(ea + 12, 0xFF);                          /* alpha */
    vm_write8(ea + 16, 1);                              /* BT.709 */
    vm_write8(ea + 17, 2);                              /* 16:9 */
    vm_write8(ea + 18, 0);                              /* limited range */
}

/* CellSailAudioFormat of the open stream (or the default, with none). */
static void sail_write_audio_format(const SailStream* s, u32 ea)
{
    vm_write8(ea + 0, CELL_SAIL_AUDIO_CODING_LPCM_FLOAT32);
    vm_write8(ea + 1, 2);                               /* channels handed over */
    vm_write16(ea + 2, SAIL_SOUND_MAX);                 /* samples a frame, at most */
    vm_write32(ea + 4, s && s->audio_rate ? s->audio_rate : 48000);
    vm_write32(ea + 8, 2);                              /* CH_LAYOUT_2CH_LR */
    vm_write32(ea + 12, 0xFFFFFFFFu);
    vm_write64(ea + 16, ~0ull);
}

/* CellSailGraphicsFrameInfo / CellSailSoundFrameInfo:
 * {u32 buffer, session, tag; s32 status; u64 pts}. */
static void sail_write_frame_info(u32 ea, u32 buffer, u32 tag, s64 pts)
{
    vm_write32(ea + 0, buffer);
    vm_write32(ea + 4, SAIL_SESSION);
    vm_write32(ea + 8, tag);
    vm_write32(ea + 12, 0);
    vm_write64(ea + 16, (u64)pts);
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
    SailPlayer* p = find_player(pSelf);
    for (int i = 0; !p && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_players[i].in_use) p = &s_players[i];
    if (!p) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(p, 0, sizeof *p);
    p->in_use = 1;
    p->self = pSelf;
    p->cb = pCallback;
    p->cb_arg = pCallbackArg;
    if (pAllocator) {
        p->alloc_funcs = vm_read32(pAllocator);
        p->alloc_arg = vm_read32(pAllocator + 4);
    }
    p->state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
    return CELL_OK;
}

/* End the open stream, handing the frame buffers back to the title. */
static void sail_close(SailPlayer* p)
{
    SailStream* s = p->stream;
    p->stream = NULL;
    if (s && s->nslots) {
        const SailGfx* g = find_gfx(p->gfx);
        if (g && g->funcs) call_title(g->funcs, GFX_FREE_FRAME, g->arg, s->nslots, p->frame_array, 0);
    }
    sail_stream_free(s);
    p->eos_pending = 0;
}

s32 cellSailPlayerFinalize(u32 pSelf)
{
    printf("[cellSail] PlayerFinalize(self=0x%08X)\n", pSelf);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_close(p);
    p->in_use = 0;
    return CELL_OK;
}

/* Not a libsail function: an older name for Finalize this file had. */
s32 cellSailPlayerDestroy(u32 pSelf) { return cellSailPlayerFinalize(pSelf); }

s32 cellSailPlayerSetSoundAdapter(u32 pSelf, u32 index, u32 pAdapter)
{
    printf("[cellSail] PlayerSetSoundAdapter(self=0x%08X index=%u adapter=0x%08X)\n", pSelf, index, pAdapter);
    SailPlayer* p = find_player(pSelf);
    if (p && index == 0) p->snd = pAdapter;
    return CELL_OK;
}

s32 cellSailPlayerSetGraphicsAdapter(u32 pSelf, u32 index, u32 pAdapter)
{
    printf("[cellSail] PlayerSetGraphicsAdapter(self=0x%08X index=%u adapter=0x%08X)\n", pSelf, index, pAdapter);
    SailPlayer* p = find_player(pSelf);
    if (p && index == 0) p->gfx = pAdapter;
    return CELL_OK;
}

s32 cellSailPlayerBoot(u32 pSelf, u64 userParam)
{
    printf("[cellSail] PlayerBoot(self=0x%08X)\n", pSelf);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_BOOT, 0, userParam);
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_CLOSED);
    return CELL_OK;
}

s32 cellSailPlayerCreateDescriptor(u32 pSelf, s32 streamType, u32 pMediaInfo, u32 pUri, u32 ppDesc)
{
    (void)pMediaInfo;
    SailPlayer* p = find_player(pSelf);
    char uri[512];
    size_t n = 0;
    while (pUri && n < sizeof uri - 1 && (uri[n] = (char)vm_read8(pUri + (u32)n)) != 0) n++;
    uri[n] = 0;
    printf("[cellSail] PlayerCreateDescriptor(self=0x%08X type=%d uri=\"%s\")\n", pSelf, streamType, uri);
    if (!p || !ppDesc) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    snprintf(p->uri, sizeof p->uri, "%s", uri);
    u32 desc = sail_alloc(p, 0x100);
    if (!desc) desc = pSelf;            /* any non-null handle: nothing reads it */
    vm_write32(ppDesc, desc);
    return CELL_OK;
}

s32 cellSailPlayerDestroyDescriptor(u32 pSelf, u32 pDesc)
{
    SailPlayer* p = find_player(pSelf);
    if (p && pDesc && pDesc != pSelf) sail_free(p, pDesc);
    if (p && p->desc == pDesc) p->desc = 0;
    return CELL_OK;
}

s32 cellSailPlayerAddDescriptor(u32 pSelf, u32 pDesc)
{
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    p->desc = pDesc;
    return CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(u32 pSelf, u32 ppDesc)
{
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (ppDesc) vm_write32(ppDesc, p->desc);
    p->desc = 0;
    return CELL_OK;
}

/* Ask the title for its frame buffers and tell its adapters the formats:
 * the graphics adapter's makeup, formatChanged and allocFrame (which fills an
 * array with the buffers' addresses, as many as its pool holds), then the
 * sound adapter's makeup and formatChanged. */
static void sail_setup_adapters(SailPlayer* p, SailStream* s, const SailGfx* g)
{
    s->frame_bytes = s->width * s->height * 4;
    if (!p->frame_array) p->frame_array = sail_alloc(p, SAIL_SLOTS * 4);
    if (!p->video_format) p->video_format = sail_alloc(p, 32);
    if (!p->audio_format) p->audio_format = sail_alloc(p, 32);

    call_title(g->funcs, GFX_MAKEUP, g->arg, 0, 0, 0);
    if (p->video_format) {
        sail_write_video_format(s, p->video_format);
        call_title(g->funcs, GFX_FORMAT_CHANGED, g->arg, p->video_format, SAIL_SESSION, 0);
    }
    if (p->frame_array && vm_read32(g->funcs + GFX_ALLOC_FRAME)) {
        for (u32 i = 0; i < SAIL_SLOTS; i++) vm_write32(p->frame_array + 4 * i, 0);
        call_title(g->funcs, GFX_ALLOC_FRAME, g->arg, s->frame_bytes, SAIL_SLOTS, p->frame_array);
        for (u32 i = 0; i < SAIL_SLOTS; i++) {
            const u32 ea = vm_read32(p->frame_array + 4 * i);
            if (!ea) break;
            s->slots[s->nslots].ea = ea;
            s->slots[s->nslots].state = SLOT_FREE;
            s->nslots++;
        }
    }

    const SailSnd* a = find_snd(p->snd);
    if (a && a->funcs) {
        call_title(a->funcs, SOUND_MAKEUP, a->arg, 0, 0, 0);
        if (p->audio_format) {
            sail_write_audio_format(s, p->audio_format);
            call_title(a->funcs, SOUND_FORMAT_CHANGED, a->arg, p->audio_format, SAIL_SESSION, 0);
        }
    }
    if (!p->sound_buffer) p->sound_buffer = sail_alloc(p, SAIL_SOUND_MAX * 2 * 4);
}

s32 cellSailPlayerOpenStream(u32 pSelf)
{
    SailPlayer* p = find_player(pSelf);
    printf("[cellSail] PlayerOpenStream(self=0x%08X uri=\"%s\")\n", pSelf, p ? p->uri : "");
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_OPEN_TRANSITION);
    sail_close(p);
    SailStream* s = p->uri[0] ? sail_stream_open(p->uri) : NULL;
    const SailGfx* g = find_gfx(p->gfx);
    if (s && g && g->funcs && g_ps3_guest_caller) {
        sail_setup_adapters(p, s, g);
        printf("[cellSail] %u frame buffer(s) of %u bytes from the title\n", s->nslots, s->frame_bytes);
        if (s->nslots < 3) {
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
        s->thread = CreateThread(NULL, 0, sail_reader_thread, s, 0, NULL);
    }
    p->stream = s;
    sail_event(p, CELL_SAIL_EVENT_STREAM_OPENED, 0, 0, 0);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_OPEN_STREAM, 0, 0);
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(u32 pSelf)
{
    printf("[cellSail] PlayerCloseStream(self=0x%08X)\n", pSelf);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_CLOSE_TRANSITION);
    sail_close(p);
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_CLOSE_STREAM, 0, 0);
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_CLOSED);
    return CELL_OK;
}

s32 cellSailPlayerStart(u32 pSelf, u32 pCommand, u64 userParam)
{
    (void)pCommand;
    printf("[cellSail] PlayerStart(self=0x%08X)\n", pSelf);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_START_TRANSITION);
    SailStream* s = p->stream;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        sail_set_clock(s, (double)(sail_start_pts(s) - s->av_delay));
        s->running = 1;
        s->paused = 0;
        ReleaseSRWLockExclusive(&s->lock);
    } else {
        p->eos_pending = 1;             /* nothing to play: it ends at the next frame */
    }
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_START, 0, userParam);
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_RUNNING);
    sail_event(p, CELL_SAIL_EVENT_SESSION_STARTED, 0, SAIL_SESSION, 0);
    return CELL_OK;
}

s32 cellSailPlayerStop(u32 pSelf, u64 userParam)
{
    printf("[cellSail] PlayerStop(self=0x%08X)\n", pSelf);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_STOP_TRANSITION);
    SailStream* s = p->stream;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        sail_set_clock(s, sail_clock(s));
        s->running = 0;
        ReleaseSRWLockExclusive(&s->lock);
    }
    p->eos_pending = 0;
    sail_event(p, CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED, CELL_SAIL_PLAYER_CALL_STOP, 0, userParam);
    sail_set_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerSetPaused(u32 pSelf, s32 paused)
{
    printf("[cellSail] PlayerSetPaused(self=0x%08X paused=%d)\n", pSelf, paused);
    SailPlayer* p = find_player(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailStream* s = p->stream;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        if (!!paused != s->paused) {
            sail_set_clock(s, sail_clock(s));
            s->paused = !!paused;
        }
        ReleaseSRWLockExclusive(&s->lock);
    }
    sail_event(p, CELL_SAIL_EVENT_PAUSE_STATE_CHANGED, 0, (u64)!!paused, 0);
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(u32 pSelf)
{
    const SailPlayer* p = find_player(pSelf);
    return p && p->stream && p->stream->paused ? 1 : 0;
}

s32 cellSailPlayerCancel(u32 pSelf) { (void)pSelf; return CELL_OK; }

s32 cellSailPlayerSetRepeatMode(u32 pSelf, s32 repeatMode, u32 pCommand)
{
    (void)pSelf; (void)pCommand;
    return repeatMode;                  /* the mode now in force */
}

/* ---------------------------------------------------------------------------
 * Graphics adapter
 * -----------------------------------------------------------------------*/

s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X arg=0x%08X)\n", pSelf, pCallbacks, pArg);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailGfx* g = find_gfx(pSelf);
    for (int i = 0; !g && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_gfx[i].in_use) g = &s_gfx[i];
    if (!g) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(g, 0, sizeof *g);
    g->in_use = 1;
    g->self = pSelf;
    g->funcs = pCallbacks;
    g->arg = pArg;
    return CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(u32 pSelf)
{
    SailGfx* g = find_gfx(pSelf);
    if (g) g->in_use = 0;
    return CELL_OK;
}

s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    printf("[cellSail] GraphicsAdapterSetPreferredFormat(self=0x%08X fmt=0x%08X)\n", pSelf, pFormat);
    SailGfx* g = find_gfx(pSelf);
    if (g && pFormat)
        for (u32 i = 0; i < sizeof g->preferred; i++) g->preferred[i] = vm_read8(pFormat + i);
    if (g && g->preferred[0] != CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED && g->preferred[0] != 0xFF)
        printf("[cellSail] the title asks for pictures in coding %d; they come as RGBA\n", (s8)g->preferred[0]);
    return CELL_OK;
}

/* The console's vblank, 59.94 Hz, in 90 kHz ticks. */
#define SAIL_VBLANK_TICKS 1501.5

/* The time a picture is chosen for: the clock, on the vblank grid. Between
 * two GetFrame calls it moves on by whole vblanks, so the pictures fall on
 * the game's frames in as even a pattern as the two rates allow (3, 2, 3, 2
 * frames for a 23.976 fps movie at 59.94). Read straight off the clock it
 * was a few milliseconds early or late with each game frame, and near a
 * picture's start that broke the pattern up.
 *
 * The game's frames keep to the vblank on average but not each one: Bleach
 * makes a movie frame after every third pull of sound (5.3 ms each), now and
 * then after a fourth, so its calls wander ~5 ms around the grid. The grid
 * follows the calls' average (a slow phase lock) so that wandering never
 * reaches the half-vblank where a call would count as one vblank too few or
 * too many.
 *
 * It is put back on the clock when the two are more than a vblank apart: at
 * the start, after a pause, after a stall of the game's, or once the sound's
 * clock has drifted that far from the host's. Under the lock. */
static double sail_video_time(SailStream* s, double now)
{
    if (!s->running || s->paused) {
        s->video_time_set = 0;
        return now;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    if (s->video_time_set) {
        const double qpc_per_vblank = SAIL_VBLANK_TICKS * (double)s->qpc_freq.QuadPart / PTS_HZ;
        const double since = (double)(qpc.QuadPart - s->video_time_qpc.QuadPart);
        const double vblanks = (double)(long long)(since / qpc_per_vblank + 0.5);
        s->video_time += vblanks * SAIL_VBLANK_TICKS;
        s->video_time_qpc.QuadPart += (LONGLONG)(vblanks * qpc_per_vblank);
        /* The phase lock: a sixteenth of the way to this call. */
        s->video_time_qpc.QuadPart += (qpc.QuadPart - s->video_time_qpc.QuadPart) / 16;
    }
    const double off = now - s->video_time;
    if (!s->video_time_set || off > SAIL_VBLANK_TICKS || off < -SAIL_VBLANK_TICKS) {
        s->video_time = now;
        s->video_time_qpc = qpc;
        s->video_time_set = 1;
    }
    return s->video_time;
}

/* The latest ready picture whose time has come; -1 when none has. */
static int sail_due_slot(const SailStream* s, double now)
{
    int best = -1;
    for (u32 i = 0; i < s->nslots; i++)
        if (s->slots[i].state == SLOT_READY && (double)s->slots[i].pts <= now &&
            (best < 0 || s->slots[i].pts > s->slots[best].pts))
            best = (int)i;
    return best;
}

/* Make slot k the one shown. The ready pictures it overtakes go back to the
 * pool, later ones stay; the slot shown before the last one is free again. */
static void sail_show_slot(SailStream* s, int k)
{
    for (u32 i = 0; i < s->nslots; i++)
        if (s->slots[i].state == SLOT_READY && s->slots[i].pts < s->slots[k].pts) s->slots[i].state = SLOT_FREE;
    if (s->previous >= 0 && s->previous != s->shown) s->slots[s->previous].state = SLOT_FREE;
    s->previous = s->shown;
    s->shown = k;
    s->slots[k].state = SLOT_SHOWN;
    WakeAllConditionVariable(&s->cv);
}

/* Everything decoded and shown, its time up and the sound played out. */
static int sail_played_out(const SailStream* s, double now)
{
    if (!s->running || !s->eof || s->nwaiting) return 0;
    for (u32 i = 0; i < s->nslots; i++)
        if (s->slots[i].state == SLOT_READY || s->slots[i].state == SLOT_FILLING) return 0;
    if (now < (double)(s->last_video_pts + s->frame_ticks)) return 0;
    return !s->has_audio || s->ring_read >= s->ring_written;
}

/* The picture due now. Returns its size (never under SAIL_MIN_RESULT), or
 * EMPTY before the first one. */
static s32 sail_get_frame(u32 pSelf, u32 pInfo, u32 pPrevInfo)
{
    SailPlayer* p = find_player_by_gfx(pSelf);
    if (!p) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    SailStream* s = p->stream;
    s32 rc = (s32)CELL_SAIL_ERROR_EMPTY;
    int announce_end = 0;
    if (s) {
        AcquireSRWLockExclusive(&s->lock);
        const double now = sail_clock(s);
        const double when = sail_video_time(s, now);
        const int due = sail_due_slot(s, when);
        if (due >= 0) {
            sail_show_slot(s, due);
            if (sail_verbose())
                printf("[cellSail] %.1f ms: picture %lld at %.0f, clock %.0f (slot %d)\n", sail_host_ms(),
                       (long long)s->slots[due].pts, when, now, due);
        } else if (sail_verbose() > 1) {
            printf("[cellSail] %.1f ms: frame call at %.0f, clock %.0f, no new picture\n", sail_host_ms(), when, now);
        }
        if (s->shown >= 0) {
            sail_write_frame_info(pInfo, s->slots[s->shown].ea, (u32)s->shown, s->slots[s->shown].pts);
            if (pPrevInfo && s->previous >= 0)
                sail_write_frame_info(pPrevInfo, s->slots[s->previous].ea, (u32)s->previous,
                                      s->slots[s->previous].pts);
            rc = (s32)(s->frame_bytes > SAIL_MIN_RESULT ? s->frame_bytes : SAIL_MIN_RESULT);
        }
        if (!s->eos && sail_played_out(s, now)) {
            s->eos = 1;
            announce_end = 1;
        }
        ReleaseSRWLockExclusive(&s->lock);
    } else if (p->eos_pending) {
        p->eos_pending = 0;
        announce_end = 1;
    }
    if (announce_end) {
        printf("[cellSail] end of the stream\n");
        sail_event(p, CELL_SAIL_EVENT_SOURCE_EOS, 0, 0, 0);
        sail_set_state(p, CELL_SAIL_PLAYER_STATE_OPENED);
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
    const SailPlayer* p = find_player_by_gfx(pSelf);
    if (!pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (!p || !p->stream) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    sail_write_video_format(p->stream, pFormat);
    return CELL_OK;
}

s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u64 pts) { (void)pSelf; (void)pts; return CELL_OK; }

/* A PTS as microseconds from the stream's start. */
static s32 sail_pts_to_time(const SailStream* s, u64 pts, u32 pTime)
{
    if (!pTime) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    if (!s) return (s32)CELL_SAIL_ERROR_INVALID_STATE;
    const s64 ticks = (s64)pts - sail_start_pts(s);
    vm_write64(pTime, (u64)(ticks > 0 ? ticks * 1000000 / PTS_HZ : 0));
    return CELL_OK;
}

s32 cellSailGraphicsAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime)
{
    const SailPlayer* p = find_player_by_gfx(pSelf);
    return sail_pts_to_time(p ? p->stream : NULL, pts, pTime);
}

/* ---------------------------------------------------------------------------
 * Sound adapter
 * -----------------------------------------------------------------------*/

s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X arg=0x%08X)\n", pSelf, pCallbacks, pArg);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailSnd* a = find_snd(pSelf);
    for (int i = 0; !a && i < CELL_SAIL_PLAYER_MAX; i++)
        if (!s_snd[i].in_use) a = &s_snd[i];
    if (!a) return (s32)CELL_SAIL_ERROR_FULLED;
    memset(a, 0, sizeof *a);
    a->in_use = 1;
    a->self = pSelf;
    a->funcs = pCallbacks;
    a->arg = pArg;
    return CELL_OK;
}

s32 cellSailSoundAdapterFinalize(u32 pSelf)
{
    SailSnd* a = find_snd(pSelf);
    if (a) a->in_use = 0;
    return CELL_OK;
}

s32 cellSailSoundAdapterSetPreferredFormat(u32 pSelf, u32 pFormat) { (void)pSelf; (void)pFormat; return CELL_OK; }

/* The next `samples` samples (at most SAIL_SOUND_MAX), interleaved float32
 * stereo, padded with silence when the ring runs short. Returns their size
 * (never under SAIL_MIN_RESULT), or EMPTY when there is no sound to give. */
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo)
{
    SailPlayer* p = find_player_by_snd(pSelf);
    if (!p || !pInfo) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    SailStream* s = p->stream;
    if (!s || !p->sound_buffer || !samples) return (s32)CELL_SAIL_ERROR_EMPTY;
    if (samples > SAIL_SOUND_MAX) samples = SAIL_SOUND_MAX;

    float out[SAIL_SOUND_MAX * 2];
    AcquireSRWLockExclusive(&s->lock);
    if (!s->running || s->paused || !s->has_audio || s->ring_read >= s->ring_written) {
        ReleaseSRWLockExclusive(&s->lock);
        return (s32)CELL_SAIL_ERROR_EMPTY;
    }
    const u64 pos = s->ring_read;
    u32 n = 0;
    for (; n < samples && s->ring_read < s->ring_written; n++, s->ring_read++) {
        const float* in = s->ring + (size_t)(s->ring_read % SAIL_RING) * 2;
        out[n * 2] = in[0];
        out[n * 2 + 1] = in[1];
    }
    for (u32 i = n; i < samples; i++) out[i * 2] = out[i * 2 + 1] = 0.0f;

    /* The pictures follow this sound: where it has got to, less what the
     * title holds before it is heard. A small difference is eased in, a large
     * one (over 1/30 s) taken at once. */
    const double heard = (double)s->first_audio_pts + (double)s->ring_read * PTS_HZ / (double)s->audio_rate -
                         (double)s->av_delay;
    const double now = sail_clock(s);
    const double off = heard - now;
    sail_set_clock(s, (off > 3000.0 || off < -3000.0) ? heard : now + off * 0.1);
    WakeAllConditionVariable(&s->cv);
    const u32 rate = s->audio_rate;
    ReleaseSRWLockExclusive(&s->lock);

    for (u32 i = 0; i < samples * 2; i++) {
        u32 bits;
        memcpy(&bits, &out[i], 4);
        vm_write32(p->sound_buffer + 4 * i, bits);
    }
    sail_write_frame_info(pInfo, p->sound_buffer, 0, s->first_audio_pts + (s64)(pos * PTS_HZ / rate));
    if (sail_verbose())
        printf("[cellSail] %.1f ms: sound %u of %u samples at %llu\n", sail_host_ms(), n, samples,
               (unsigned long long)pos);
    const u32 bytes = samples * 2 * 4;
    return (s32)(bytes > SAIL_MIN_RESULT ? bytes : SAIL_MIN_RESULT);
}

s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    const SailPlayer* p = find_player_by_snd(pSelf);
    if (!pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    sail_write_audio_format(p ? p->stream : NULL, pFormat);
    return CELL_OK;
}

s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u64 pts) { (void)pSelf; (void)pts; return CELL_OK; }

s32 cellSailSoundAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime)
{
    const SailPlayer* p = find_player_by_snd(pSelf);
    return sail_pts_to_time(p ? p->stream : NULL, pts, pTime);
}
