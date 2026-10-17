/*
 * rsx_replay - play a frame capture (libs/video/rsx_capture.h) back through the
 * register-file draw engine and the headless Metal backend, with no game.
 *
 *   rsx_replay <capture.rsxcap> [--out <prefix>] [--every <n>] [--last <n>]
 *              [--from <present>] [--surf <dir>]
 *
 *   --out    write presented frames as <prefix>.<present>.ppm
 *   --every  ...every n-th present (default 1)
 *   --last   ...only the last n presents (overrides --every's start)
 *   --from   ...only from this present on
 *   --surf   also dump every render target (RSX_SURF_DUMP_DIR) at the
 *            presents --out writes
 *   --bench  <n>: the capture decompressed into memory and played n times
 *            in one process, the first to warm the caches (pipelines,
 *            textures, vertex buffers); each pass reports wall and thread
 *            CPU time per present, and the CPU spent copying the capture's
 *            pages (the replay's own work, not the engine's). The steady
 *            passes are the engine's cost on these frames, the same every
 *            run: the benchmark for walker changes. RSX_REPLAY_PROF=1
 *            (Windows) samples this thread every millisecond during the
 *            steady passes and prints its hottest functions, self and
 *            inclusive, named through the PDB. The bench stands in for the
 *            game's page write-watch, as the game hashes only what was
 *            written: a page record is a write (the capture records a page
 *            only when its bytes changed). RSX_REPLAY_WATCH=0 turns that
 *            off, =1 turns it on outside the bench.
 *
 * Every switch the engine and backend read from the environment works here
 * too (RSX_SKIP_FP, RSX_DRAW_TRACE_FRAME, RSX_NO_SHADOW_CMP, ...), so an
 * experiment that took a 5-minute game run takes a few seconds.
 *
 * The capture's pages go into two sparse arenas, one per RSX location, as the
 * stream reaches them; the engine's guest reads are served from there, and a
 * read touching a page the capture never recorded fails as it did live.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <zlib.h>
#ifdef _WIN32
/* Windows: the arenas are reserved address space committed a page at a time
 * as the capture fills them; the backend is the D3D12 draw-engine one,
 * headless; setenv and the clock come from the CRT and QPC. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "../../libs/video/rsx_d3d12_engine.h"
#include "../../libs/video/rsx_vulkan_engine.h"
static int setenv(const char* k, const char* v, int overwrite)
{
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v) ? -1 : 0;
}
static int unsetenv(const char* k) { return _putenv_s(k, "") ? -1 : 0; }
#else
#include <sys/mman.h>
#endif

#include "ps3emu/ps3types.h"
#include "../../libs/video/rsx_capture.h"
#include "../../libs/video/rsx_draw_engine.h"
#include "../../libs/video/rsx_dispatch.h"

int rsx_metal_backend_init(u32 width, u32 height, const char* title);

/* The runtime's flat guest VM. Replay never touches it: the engine reads
 * through the reader below. */
uint8_t* vm_base = NULL;
#define ARENA_BYTES 0x100000000ull
#define NPAGES      (ARENA_BYTES / RSX_CAPTURE_PAGE)

static u8* s_arena[2];
static u8* s_have[2];   /* one bit per page: the capture recorded it */
static u32* s_gen[2];   /* per page: how often the capture rewrote it */

static const u8* replay_reader(void* user, u32 location, u32 offset, u32 len)
{
    (void)user;
    if (location > 1 || !len) return NULL;
    const u64 end = (u64)offset + len;
    if (end > ARENA_BYTES) return NULL;
    for (u64 pg = offset / RSX_CAPTURE_PAGE; pg <= (end - 1) / RSX_CAPTURE_PAGE; pg++)
        if (!(s_have[location][pg >> 3] & (1u << (pg & 7)))) return NULL;
    return s_arena[location] + offset;
}

/* The write-watch over the arenas (rsx_draw_engine_set_guest_watch): the
 * stamp sums the pages' rewrite counts, weighted by position, as vm_watch_arm
 * does with its write generations. */
static u64 replay_watch(void* user, u32 location, u32 offset, u32 len, u32* unwatched)
{
    (void)user;
    *unwatched = 1;
    if (location > 1 || !len) return 0;
    const u64 end = (u64)offset + len;
    if (end > ARENA_BYTES) return 0;
    const u64 p0 = offset / RSX_CAPTURE_PAGE;
    u64 stamp = 0;
    u32 un = 0;
    for (u64 pg = p0; pg <= (end - 1) / RSX_CAPTURE_PAGE; pg++) {
        if (!(s_have[location][pg >> 3] & (1u << (pg & 7)))) un++;
        stamp += (u64)s_gen[location][pg] * (pg - p0 + 1);
    }
    *unwatched = un;
    return stamp;
}

static gzFile s_gz;
/* --bench: the stream after the first pass, decompressed, read from memory. */
static u8* s_mem;
static size_t s_mem_len, s_mem_pos;
static int rd(void* p, unsigned n)
{
    if (s_mem) {
        if (s_mem_len - s_mem_pos < n) return 0;
        memcpy(p, s_mem + s_mem_pos, n);
        s_mem_pos += n;
        return 1;
    }
    return gzread(s_gz, p, n) == (int)n;
}

#ifdef _WIN32
/* RSX_REPLAY_PROF=1: a sampling profiler over the replay thread. Every
 * millisecond the thread is suspended, its stack walked (16 frames) and
 * resumed; a sample is kept only when the thread ran since the last one
 * (its cycle count moved), so GPU waits do not count. */
#include <dbghelp.h>
#include <timeapi.h>
#define PROF_DEPTH 16
#define PROF_MAX   400000
static HANDLE s_prof_target;
static volatile LONG s_prof_on, s_prof_stop;
static DWORD64 (*s_prof_pc)[PROF_DEPTH];
static unsigned char* s_prof_depth;
static volatile LONG s_prof_n;
static DWORD WINAPI prof_thread(LPVOID unused)
{
    (void)unused;
    timeBeginPeriod(1);
    ULONG64 last = 0;
    while (!s_prof_stop) {
        Sleep(1);
        if (!s_prof_on || s_prof_n >= PROF_MAX) continue;
        ULONG64 cyc = 0; QueryThreadCycleTime(s_prof_target, &cyc);
        if (cyc == last) continue;
        last = cyc;
        if (SuspendThread(s_prof_target) == (DWORD)-1) continue;
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(s_prof_target, &c)) {
            STACKFRAME64 f; memset(&f, 0, sizeof f);
            f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
            const LONG i = s_prof_n;
            int d = 0;
            while (d < PROF_DEPTH && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), s_prof_target, &f, &c,
                                                 NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
                if (!f.AddrPC.Offset) break;
                s_prof_pc[i][d++] = f.AddrPC.Offset;
            }
            if (d) { s_prof_depth[i] = (unsigned char)d; s_prof_n = i + 1; }
        }
        ResumeThread(s_prof_target);
    }
    return 0;
}
typedef struct { char name[96]; unsigned self, incl; } ProfFn;
static int prof_cmp_self(const void* a, const void* b) { return (int)((const ProfFn*)b)->self - (int)((const ProfFn*)a)->self; }
static int prof_cmp_incl(const void* a, const void* b) { return (int)((const ProfFn*)b)->incl - (int)((const ProfFn*)a)->incl; }
static void prof_report(void)
{
    const LONG n = s_prof_n;
    if (!n) return;
    ProfFn* fns = (ProfFn*)calloc(8192, sizeof *fns);
    unsigned nf = 0;
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    for (LONG i = 0; i < n; i++) {
        unsigned seen[PROF_DEPTH]; int ns = 0;
        for (int d = 0; d < s_prof_depth[i]; d++) {
            SYMBOL_INFO* si = (SYMBOL_INFO*)symbuf; memset(si, 0, sizeof *si);
            si->SizeOfStruct = sizeof *si; si->MaxNameLen = 255;
            DWORD64 disp = 0;
            char name[96];
            if (SymFromAddr(GetCurrentProcess(), s_prof_pc[i][d], &disp, si)) snprintf(name, sizeof name, "%s", si->Name);
            else snprintf(name, sizeof name, "?%llx", (unsigned long long)s_prof_pc[i][d] >> 12);
            unsigned k = 0;
            while (k < nf && strcmp(fns[k].name, name)) k++;
            if (k == nf) { if (nf == 8192) continue; snprintf(fns[nf].name, sizeof fns[nf].name, "%s", name); nf++; }
            if (d == 0) fns[k].self++;
            int dup = 0; for (int q = 0; q < ns; q++) if (seen[q] == k) dup = 1;
            if (!dup) { fns[k].incl++; seen[ns++] = k; }
        }
    }
    /* RSX_REPLAY_PROF_CALLERS=<name>: the callers of a function by sample
     * count (the first frame below it that is not itself). */
    const char* who = getenv("RSX_REPLAY_PROF_CALLERS");
    if (who && *who) {
        ProfFn* cl = (ProfFn*)calloc(512, sizeof *cl);
        unsigned nc = 0, tot = 0;
        for (LONG i = 0; i < n; i++) {
            int hit = -1;
            for (int d = 0; d < s_prof_depth[i] && hit < 0; d++) {
                SYMBOL_INFO* si = (SYMBOL_INFO*)symbuf; memset(si, 0, sizeof *si);
                si->SizeOfStruct = sizeof *si; si->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(GetCurrentProcess(), s_prof_pc[i][d], &disp, si) && !strcmp(si->Name, who)) hit = d;
            }
            if (hit < 0) continue;
            for (int d = hit + 1; d < s_prof_depth[i]; d++) {
                SYMBOL_INFO* si = (SYMBOL_INFO*)symbuf; memset(si, 0, sizeof *si);
                si->SizeOfStruct = sizeof *si; si->MaxNameLen = 255;
                DWORD64 disp = 0;
                char name[96];
                if (SymFromAddr(GetCurrentProcess(), s_prof_pc[i][d], &disp, si)) snprintf(name, sizeof name, "%s", si->Name);
                else snprintf(name, sizeof name, "?%llx", (unsigned long long)s_prof_pc[i][d] >> 12);
                if (!strcmp(name, who)) continue;
                unsigned k = 0;
                while (k < nc && strcmp(cl[k].name, name)) k++;
                if (k == nc && nc < 512) { snprintf(cl[nc].name, sizeof cl[nc].name, "%s", name); nc++; }
                if (k < nc) cl[k].self++;
                tot++;
                break;
            }
        }
        qsort(cl, nc, sizeof *cl, prof_cmp_self);
        fprintf(stderr, "[prof] callers of %s (%u samples):\n", who, tot);
        for (unsigned k = 0; k < nc && k < 15; k++)
            fprintf(stderr, "[prof]   %5.1f%%  %s\n", tot ? 100.0 * cl[k].self / tot : 0.0, cl[k].name);
        free(cl);
    }
    qsort(fns, nf, sizeof *fns, prof_cmp_self);
    fprintf(stderr, "[prof] %ld on-CPU samples; self:\n", (long)n);
    for (unsigned k = 0; k < nf && k < 40; k++)
        fprintf(stderr, "[prof]   %5.1f%%  %s\n", 100.0 * fns[k].self / n, fns[k].name);
    qsort(fns, nf, sizeof *fns, prof_cmp_incl);
    fprintf(stderr, "[prof] inclusive:\n");
    for (unsigned k = 0; k < nf && k < 50; k++)
        fprintf(stderr, "[prof]   %5.1f%%  %s\n", 100.0 * fns[k].incl / n, fns[k].name);
    free(fns);
}
#endif

/* CPU time of this thread in TSC-rate cycles, and the time-stamp counter:
 * the bench's per-pass CPU, and the share of it the page copies took. */
#if defined(_WIN32)
#include <intrin.h>
static u64 thread_cycles(void) { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return c; }
static u64 tsc_now(void) { return __rdtsc(); }
#elif defined(__x86_64__)
#include <x86intrin.h>
static u64 thread_cycles(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u64 tsc_now(void) { return __rdtsc(); }
#define BENCH_CPU_IS_NS 1
#else
static u64 thread_cycles(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static u64 tsc_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
#define BENCH_CPU_IS_NS 1
#define BENCH_TSC_IS_NS 1
#endif

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

int main(int argc, char** argv)
{
    const char* cap = NULL; const char* out = NULL; const char* surf = NULL;
    long every = 1, last = 0, from = 0, bench = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--last") && i + 1 < argc) last = atol(argv[++i]);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--surf") && i + 1 < argc) surf = argv[++i];
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atol(argv[++i]);
        else if (argv[i][0] != '-' && !cap) cap = argv[i];
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!cap) {
        fprintf(stderr, "usage: rsx_replay <capture> [--out <prefix>] [--every n] [--last n] [--from n] [--surf dir] [--bench n]\n");
        return 2;
    }
    if (every <= 0) every = 1;

    /* First pass: how many presents, so --last can be honoured. */
    long total = 0;
    s_gz = gzopen(cap, "rb");
    if (!s_gz) { fprintf(stderr, "cannot open %s\n", cap); return 1; }
    char magic[8]; u32 hdr[4];
    if (!rd(magic, 8) || memcmp(magic, RSX_CAPTURE_MAGIC, 8) || !rd(hdr, sizeof hdr)) {
        fprintf(stderr, "%s is not an RSX capture\n", cap); return 1;
    }
    const u32 nregs = hdr[0], nvp = hdr[1], nconst = hdr[2], ndb = hdr[3];
    gzseek(s_gz, (z_off_t)(4u * (nregs + nvp + nconst + ndb * 6u)), SEEK_CUR);
    for (;;) {
        u8 kind; if (!rd(&kind, 1)) break;
        if (kind == RSX_CAP_END) break;
        if (kind == RSX_CAP_METHOD) gzseek(s_gz, 8, SEEK_CUR);
        else if (kind == RSX_CAP_PAGE) gzseek(s_gz, 5 + RSX_CAPTURE_PAGE, SEEK_CUR);
        else if (kind == RSX_CAP_FLIP) { gzseek(s_gz, 4, SEEK_CUR); total++; }
        else if (kind == RSX_CAP_DISPBUF) gzseek(s_gz, 24, SEEK_CUR);
        else { fprintf(stderr, "corrupt capture (record kind %u)\n", kind); return 1; }
    }
    gzclose(s_gz);
    if (last > 0 && last < total && total - last > from) from = total - last;

    /* The backend dumps every `every`-th present it is handed; presents
     * before `from` are skipped by turning the dump off until then. */
    setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
    setenv("PS3RECOMP_D3D12_HEADLESS", "1", 1);
    /* The regression's references predate RSX_ANISO: compare without it
     * unless asked. */
    setenv("RSX_ANISO", "1", 0);
    if (surf) {   /* the engine counts presents from 0, as the loop below does */
        char v[32];
        setenv("RSX_SURF_DUMP_DIR", surf, 1);
        snprintf(v, sizeof v, "%ld", from);  setenv("RSX_SURF_DUMP_FROM", v, 1);
        snprintf(v, sizeof v, "%ld", every); setenv("RSX_SURF_DUMP_EVERY", v, 1);
        setenv("RSX_SURF_DUMP_COUNT", "100000", 1);
    }
    if (!getenv("PS3RECOMP_MSL_CACHE")) setenv("PS3RECOMP_MSL_CACHE", "cache/msl", 1);
    if (!getenv("PS3RECOMP_RSX_ENGINE")) setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    /* Frames are compared byte for byte, so every draw waits for its pipeline. */
    if (!getenv("RSX_ASYNC_SHADERS")) setenv("RSX_ASYNC_SHADERS", "0", 1);

    for (int l = 0; l < 2; l++) {
#ifdef _WIN32
        s_arena[l] = (u8*)VirtualAlloc(NULL, ARENA_BYTES, MEM_RESERVE, PAGE_READWRITE);
        s_have[l] = (u8*)calloc(NPAGES / 8, 1);
        s_gen[l] = (u32*)calloc(NPAGES, sizeof(u32));
        if (!s_arena[l] || !s_have[l] || !s_gen[l]) { fprintf(stderr, "arena allocation failed\n"); return 1; }
#else
        s_arena[l] = (u8*)mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
        s_have[l] = (u8*)calloc(NPAGES / 8, 1);
        s_gen[l] = (u32*)calloc(NPAGES, sizeof(u32));
        if (s_arena[l] == MAP_FAILED || !s_have[l] || !s_gen[l]) { fprintf(stderr, "arena allocation failed\n"); return 1; }
#endif
    }

#ifdef _WIN32
    /* RSX_BACKEND=vulkan replays through the Vulkan backend instead. */
    const int use_vulkan = rsx_vulkan_engine_wanted();
    if (use_vulkan ? rsx_vulkan_engine_init(1280, 720, "rsx_replay") != 0
                   : rsx_d3d12_engine_init(1280, 720, "rsx_replay") != 0) {
        fprintf(stderr, "%s engine backend init failed\n", use_vulkan ? "Vulkan" : "D3D12"); return 1;
    }
#else
    if (rsx_metal_backend_init(1280, 720, "rsx_replay") != 0) {
        fprintf(stderr, "Metal backend init failed\n"); return 1;
    }
#endif
    rsx_draw_engine_set_guest_memory(replay_reader, NULL);
    { const char* w = getenv("RSX_REPLAY_WATCH");
      if (w ? w[0] != '0' : bench > 0) rsx_draw_engine_set_guest_watch(replay_watch); }

    s_gz = gzopen(cap, "rb");
    gzbuffer(s_gz, 1u << 20);
    if (bench > 0) {
        size_t cap_bytes = 64u << 20;
        s_mem = (u8*)malloc(cap_bytes);
        for (int n; s_mem && (n = gzread(s_gz, s_mem + s_mem_len, (unsigned)(cap_bytes - s_mem_len))) > 0;) {
            s_mem_len += (size_t)n;
            if (s_mem_len == cap_bytes) { cap_bytes *= 2; s_mem = (u8*)realloc(s_mem, cap_bytes); }
        }
        gzclose(s_gz);
        if (!s_mem) { fprintf(stderr, "out of memory for --bench\n"); return 1; }
        fprintf(stderr, "[bench] %s: %.1f MB decompressed, %ld passes\n", cap, s_mem_len / 1048576.0, bench);
    }
    const double tsc_t0 = now_s(); const u64 tsc_c0 = tsc_now();
#ifdef _WIN32
    const int prof = bench > 1 && getenv("RSX_REPLAY_PROF") != NULL;
    if (prof) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_prof_target,
                        THREAD_ALL_ACCESS, FALSE, 0);
        s_prof_pc = (DWORD64(*)[PROF_DEPTH])calloc(PROF_MAX, sizeof *s_prof_pc);
        s_prof_depth = (unsigned char*)calloc(PROF_MAX, 1);
        CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
    }
#endif
    for (long pass = 0; pass < (bench > 0 ? bench : 1); pass++) {
    if (bench > 0) s_mem_pos = 0;
#ifdef _WIN32
    if (prof) s_prof_on = pass > 0;
#endif
    rd(magic, 8); rd(hdr, sizeof hdr);
    u32* regs = (u32*)malloc(nregs * 4u);
    u32* vp = (u32*)malloc(nvp * 4u);
    u32* cst = (u32*)malloc(nconst * 4u);
    if (!rd(regs, nregs * 4u) || !rd(vp, nvp * 4u) || !rd(cst, nconst * 4u)) {
        fprintf(stderr, "truncated header\n"); return 1;
    }
    rsx_draw_engine_seed_state(regs, nregs, vp, nvp, cst, nconst);
    for (u32 i = 0; i < ndb; i++) {
        u32 d[6]; if (!rd(d, sizeof d)) return 1;
        if (d[0]) rsx_draw_engine_set_display_buffer(i, d[1], d[2], d[3], d[4], d[5]);
    }

    if (pass == 0)
        fprintf(stderr, "[replay] %s: %ld presents; writing %s from present %ld every %ld\n",
                cap, total, out ? out : "(nothing)", from, every);
    free(regs); free(vp); free(cst);
    const double t0 = now_s();
    const u64 cyc0 = thread_cycles();
    u64 page_tsc = 0;
    long presents = 0; unsigned long long methods = 0, pages = 0;
    for (;;) {
        u8 kind; if (!rd(&kind, 1)) break;
        if (kind == RSX_CAP_END) break;
        if (kind == RSX_CAP_METHOD) {
            u32 w[2]; if (!rd(w, 8)) break;
            rsx_draw_engine_method(w[0], w[1]);
            methods++;
        } else if (kind == RSX_CAP_PAGE) {
            const u64 pt0 = bench > 0 ? tsc_now() : 0;
            u8 loc; u32 off;
            if (!rd(&loc, 1) || !rd(&off, 4) || loc > 1) break;
            const u64 pg = off / RSX_CAPTURE_PAGE;
#ifdef _WIN32
            if (!(s_have[loc][pg >> 3] & (1u << (pg & 7))) &&
                !VirtualAlloc(s_arena[loc] + off, RSX_CAPTURE_PAGE, MEM_COMMIT, PAGE_READWRITE)) break;
#endif
            if (!rd(s_arena[loc] + off, RSX_CAPTURE_PAGE)) break;
            s_have[loc][pg >> 3] |= (u8)(1u << (pg & 7));
            s_gen[loc][pg]++;
            pages++;
            if (bench > 0) page_tsc += tsc_now() - pt0;
        } else if (kind == RSX_CAP_FLIP) {
            u32 b; if (!rd(&b, 4)) break;
            /* Arm the dumps for this present only when it is one we want. */
            const int want = out && pass == 0 && presents >= from && ((presents - from) % every) == 0;
            if (want) {
                char path[1024];
                snprintf(path, sizeof path, "%s.%06ld.ppm", out, presents);
                setenv("RSX_REPLAY_DUMP_PATH", path, 1);
            } else {
                unsetenv("RSX_REPLAY_DUMP_PATH");
            }
            rsx_draw_engine_fifo_flip(b);
            presents++;
        } else if (kind == RSX_CAP_DISPBUF) {
            u32 d[6]; if (!rd(d, sizeof d)) break;
            rsx_draw_engine_set_display_buffer(d[0], d[1], d[2], d[3], d[4], d[5]);
        } else {
            fprintf(stderr, "corrupt capture (record kind %u)\n", kind); break;
        }
    }
    const double dt = now_s() - t0;
    if (bench > 0) {
        /* The TSC's rate from this run's own clock, for the cycle counts. */
        const double tsc_hz = (double)(tsc_now() - tsc_c0) / (now_s() - tsc_t0);
#ifdef BENCH_CPU_IS_NS
        const double cpu_ms = (double)(thread_cycles() - cyc0) * 1e-6;
#else
        const double cpu_ms = (double)(thread_cycles() - cyc0) * 1e3 / tsc_hz;
#endif
#ifdef BENCH_TSC_IS_NS
        const double page_ms = (double)page_tsc * 1e-6;
#else
        const double page_ms = (double)page_tsc * 1e3 / tsc_hz;
#endif
        const double np = presents ? (double)presents : 1.0;
        fprintf(stderr, "[bench] pass %ld: %ld presents, %llu methods: wall %.3f ms/present, CPU %.3f ms/present "
                "(page copies %.3f, engine %.3f)\n",
                pass, presents, methods, dt * 1e3 / np, cpu_ms / np, page_ms / np, (cpu_ms - page_ms) / np);
    } else {
        fprintf(stderr, "[replay] %ld presents, %llu methods, %llu pages in %.1f s (%.1f presents/s)\n",
                presents, methods, pages, dt, dt > 0 ? presents / dt : 0.0);
    }
    }   /* passes */
#ifdef _WIN32
    if (prof) { s_prof_on = 0; s_prof_stop = 1; Sleep(20); prof_report(); }
#endif
    if (!s_mem) gzclose(s_gz);
    free(s_mem);
#ifdef _WIN32
    if (use_vulkan) rsx_vulkan_engine_shutdown(); else rsx_d3d12_engine_shutdown();
#endif
    return 0;
}
