/*
 * ps3recomp - integrated PPU boot harness.
 *
 * Links the whole PPU runtime half into one executable and starts executing a
 * recompiled program's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the PPU ELF named on the command line, registers the lifted
 * functions and the HLE NID handlers, starts the frame clock, then dispatches
 * the entry OPD. Execution runs real guest code until it reaches a function
 * outside the lifted subset (logged by the unlifted stub), an unimplemented
 * firmware import (logged by ps3_hle_call), or an lv2 syscall (logged by
 * lv2_syscall) -- telling us exactly what to implement next.
 *
 * Nothing here is title-specific: the ELF path, the VFS root derived from it
 * and the window title all come from the command line or the environment. Two
 * things drive it today -- a game port, which supplies its own lifted
 * ppu_recomp.c and its own HLE overrides, and the smoke title in
 * tests/smoke/, which is the same harness with a synthetic program in it and
 * is what proves this path runs at all on POSIX.
 */
#include <stdarg.h>
#include "ppu_recomp.h"
#include "ps3emu/milestone.h"   /* ps3_ms / ps3_ms_kv -- boot milestone log */
/* PPU_THREAD_LOCAL only -- NOT ppu_context.h, which would redefine the struct
 * a generated ppu_recomp.h already declares. Ports generated before the
 * qualifier existed do not define it, so the scaffold must carry its own
 * reachable definition. */
#include "../ppu_tls.h"
#include "../../platform/win32_compat.h"      /* Sleep, GetTickCount64, threads on POSIX */
#include "../../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */
/* The lifted ppu_recomp.h already defines `struct ppu_context` (identical to
 * runtime/ppu/ppu_context.h by design -- "keep the two in sync"). A syscall
 * header included later (sys_ppu_thread.h -> lv2_syscall_table.h -> ppu_context.h)
 * would redefine it, so claim ppu_context.h's guard now: this TU has the struct,
 * and boot_main uses none of that header's PPU_CR or XER bit macros. */
#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
uint32_t ps3_hle_count(void);   /* handlers actually registered, for the milestone log */
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* The status a guest handed to sys_process_exit, published by
 * libs/system/sysPrxForUser.c before it calls the host exit(). A title that
 * exits that way never returns through ppu_run, so this is only read on the
 * path where the guest fell out of its entry function instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>



/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */
extern "C" void np_psnr_setup(const char* username, const char* server);  /* libs/network/np_psnr.c */

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" int  cellGcm_take_flip_pending(void);  /* hoisted to file scope: clang-cl rejects block-scope extern "C" */

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{ ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7); }

/* RSX present backend: the null backend's headless software path -- the same
 * selection runtime/host/host_posix.c makes, so the ticker below is
 * backend-agnostic. */
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" void cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken by a kick */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" int sys_event_queue_inject(unsigned int, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

/* Present / pump through the backend. */
static void rsx_present_frame(void)
{
    rsx_backend_present();
}
static int rsx_pump_messages(void)
{ return rsx_backend_pump(); }

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished, which is the number a port means by "is anything
 * reaching the screen". The presents the ticker makes before the guest's first
 * flip (so a fresh window is not left white through a long boot) are
 * deliberately not counted: they carry no guest frame. */
static volatile LONG g_frames_presented = 0;
extern "C" unsigned ppu_boot_frames_presented(void) { return (unsigned)g_frames_presented; }
static void present_guest_frame(void)
{
    rsx_present_frame();
    /* The ticker increments, a guest thread reads: interlocked rather than a
     * volatile ++, which is neither atomic nor a fence on arm64. */
    InterlockedIncrement(&g_frames_presented);
}

/* ---- Guest-PC sampling profiler (PS3_GUEST_PROF=1) -----------------------
 * Samples every guest thread's ctx.cia every ~5ms and dumps the top sites
 * every 5s. cia is refreshed at every syscall, and guest spin loops issue a
 * syscall per iteration (yield/usleep), so a thread stuck in a wait loop
 * shows its loop's PC dominating the histogram. This is the tool that
 * answers "where does thread X spend its wall time" without a debugger. */
#include "../../syscalls/sys_ppu_thread.h"
static DWORD WINAPI guest_prof_thread(LPVOID)
{
    struct Site { uint32_t tid, cia; uint32_t hits; };
    static Site sites[256];
    int nsites = 0;
    ULONGLONG t0 = GetTickCount64();
    unsigned total = 0;
    for (;;) {
        Sleep(5);
        for (int i = 0; i < PPU_THREAD_MAX; i++) {
            ppu_thread_info* t = &g_ppu_threads[i];
            if (t->state != 1 /*RUNNING-ish; harmless if enum differs*/ && t->state != 2) continue;
            uint32_t cia = (uint32_t)t->ctx.cia;
            if (!cia) continue;
            uint32_t tid = (uint32_t)(i + 1);
            total++;
            int f = -1;
            for (int k = 0; k < nsites; k++)
                if (sites[k].tid == tid && sites[k].cia == cia) { f = k; break; }
            if (f < 0 && nsites < 256) { f = nsites++; sites[f].tid = tid; sites[f].cia = cia; sites[f].hits = 0; }
            if (f >= 0) sites[f].hits++;
        }
        ULONGLONG now = GetTickCount64();
        if (now - t0 >= 5000) {
            /* top 10 by hits */
            fprintf(stderr, "[gprof] %u samples/5s; top sites:\n", total);
            for (int rank = 0; rank < 10; rank++) {
                int best = -1; uint32_t bh = 0;
                for (int k = 0; k < nsites; k++)
                    if (sites[k].hits > bh) { bh = sites[k].hits; best = k; }
                if (best < 0 || bh == 0) break;
                fprintf(stderr, "[gprof]   tid=%-3u cia=0x%08X  %5.1f%%  (%s)\n",
                        sites[best].tid, sites[best].cia,
                        100.0 * sites[best].hits / (total ? total : 1),
                        g_ppu_threads[sites[best].tid - 1].name);
                sites[best].hits = 0;   /* consume */
            }
            fflush(stderr);
            nsites = 0; total = 0; t0 = now;
        }
    }
}

extern "C" const char* cellGame_get_title(void);   /* PARAM.SFO TITLE, for the caption */

static DWORD WINAPI vblank_ticker(LPVOID)
{
    /* Window caption: $PS3_TITLE wins, else the TITLE field PARAM.SFO already
     * gave cellGame, else a neutral name. It used to fall back to a specific
     * other title, so every port announced itself as that game. */
    const char* _title = getenv("PS3_TITLE");
    if (!_title || !*_title) {
        const char* sfo = cellGame_get_title();
        if (sfo && *sfo && strcmp(sfo, "Unknown Title") != 0) _title = sfo;
    }
    if (!_title || !*_title) _title = "ps3recomp";
    /* A title that renders into something other than 1280x720 must say so:
     * RSX_W / RSX_H. */
    uint32_t rsx_w = 1280, rsx_h = 720;
    if (const char* e = getenv("RSX_W")) rsx_w = (uint32_t)strtoul(e, 0, 0);
    if (const char* e = getenv("RSX_H")) rsx_h = (uint32_t)strtoul(e, 0, 0);
    int rsx_ok = (rsx_backend_init(rsx_w, rsx_h, _title) == 0);
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. A hidden or
     * occluded window can throttle presentation, which previously stalled these
     * ticks and paced the game's main loop to ~0.5fps. Drive the ticks off REAL
     * elapsed time and catch up in a bounded burst so present latency never
     * slows the game. */
    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        /* Was Sleep(4). The FIFO kick event existed, was signalled on a dry
         * fence queue, and had no waiter -- so a guest starved for `get` or a
         * fence still paid the whole tick. Waiting on it keeps the same 4 ms
         * ceiling when nothing kicks, and drains on demand when something
         * does. */
        cellGcm_fifo_kick_wait(4);
        ULONGLONG now = GetTickCount64();
        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put frame boundary (guest thread), so the batch held
             * right now is exactly the completed frame. Presenting on a raw
             * flip-count change after the drain raced the guest's next-frame
             * writes and showed empty or mixed batches. */
            {
                if (rsx_ok && cellGcm_take_flip_pending()) {
                    present_guest_frame();
                    last_flip = cellGcm_flip_request_count();
                }
            }
            /* Drain the game's GCM FIFO every tick -- this writes the RSX sync-fence
             * labels (e.g. @0x03000410) that the game's per-frame logic blocks on.
             * Doing it here (not after present) keeps those fences advancing at 60Hz
             * even when present() throttles on a hidden or occluded window. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;                 /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -> resync */
        /* Drain at the outer ~4ms cadence too (not just the 16ms tick):
         * titles fence EVERY render pass on an RSX label the drain writes;
         * at 16ms per fence wave's six passes paced the guest to ~7 fps.
         * The real RSX writes those fences in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();
        }
        /* YDKJ_INJECT_Q3: the main thread polls q3 (load/state-complete) each frame but
         * it's never posted (producer signals a condvar, never enqueues). Inject a q3
         * event periodically to test whether delivering the completion advances the
         * game's state machine to instantiate + display the (force-parsed) menu movie. */
        if (getenv("YDKJ_INJECT_Q3")) {
            static int s_q3=0; const char* qe=getenv("YDKJ_INJECT_Q3"); uint32_t qid=(uint32_t)strtoul(qe,0,0); if(qid==0||qid==1) qid=3;
            if(++s_q3 % 8 == 0){ int r=sys_event_queue_inject(qid, 0x1234, 0, 0, 0);
              static int _n=0; if(_n++<12) fprintf(stderr,"[INJQ3] injected q%u event rc=%d\n",qid,r); } }
        if (rsx_ok) {
            if (rsx_pump_messages() != 0) { rsx_ok = 0; }
            if (getenv("YDKJ_PACETRACE")) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64(); rsx_present_frame(); ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000) {
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* Present only on a guest flip (frame boundary). A fixed-clock
                 * present can catch the drain mid-frame -- notably while the
                 * guest is blocked in the FIFO-wrap recycle callback -- and
                 * flash a partial frame (clear + a few draws). Before the
                 * first flip present freely so the window isn't stuck white
                 * during boot. */
                unsigned fc = cellGcm_flip_request_count();
                if (fc != last_flip || fc == 0) {
                    if (fc) present_guest_frame(); else rsx_present_frame();
                    last_flip = fc;
                } else if (fc == 0) {
                    rsx_backend_present();
                }
            }
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */


int main(int argc, char** argv)
{
    /* <PPU ELF> [--username NAME] [--psnr host[:port]], flags in any order. */
    const char* elf = NULL;
    const char* username = NULL;
    const char* psnr = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char** flag = !strncmp(a, "--username", 10) ? &username
                          : !strncmp(a, "--psnr", 6)      ? &psnr : NULL;
        if (!flag) { if (!elf) elf = a; continue; }
        const char* eq = strchr(a, '=');
        if (eq) *flag = eq + 1;
        else if (i + 1 < argc) *flag = argv[++i];
    }
    if (!elf) {
        printf("usage: %s <PPU ELF> [--username NAME] [--psnr host[:port]]\n", argv[0]);
        return 2;
    }
    np_psnr_setup(username, psnr);


    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything). On real PS3 those addresses are RAM, and the game
     * writes to them (its null-object inits land on page 0); calloc backs them
     * so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(elf);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Real title id, from the game's own PARAM.SFO. cellGame has been able to
     * read this since 2026-06-21, but only a title's own main() ever called it,
     * so every port that moved to this harness silently kept the BLES00000
     * placeholder -- and that id is what cellGame / cellSaveData / trophy build
     * their /dev_hdd0/game/<id> paths from, so saves landed in a directory
     * belonging to no title. Same shape as the live-draw engine before 1a050be:
     * a working facility with no caller in the shared harness. */
    {
        /* PARAM.SFO sits in a different place depending on how the VFS is rooted.
         * A disc-style root has it under PS3_GAME/; a title whose content is
         * opened by RELATIVE path needs the root AT USRDIR, and then PARAM.SFO is
         * one level up instead. Only the first layout was tried, so rooting at
         * USRDIR silently lost the title id and name -- which is how the window
         * caption ended up on its hardcoded fallback. Try each and take the
         * first that exists. */
        static const char* const kSfoLayouts[] = {
            "%s/PS3_GAME/PARAM.SFO",   /* vfs root = disc root      */
            "%s/../PARAM.SFO",         /* vfs root = .../USRDIR     */
            "%s/PARAM.SFO",            /* vfs root = the game dir   */
        };
        char sfo[1100];
        int found = 0;
        for (size_t i = 0; i < sizeof kSfoLayouts / sizeof kSfoLayouts[0]; i++) {
            snprintf(sfo, sizeof sfo, kSfoLayouts[i], ppu_vfs_root);
            FILE* f = fopen(sfo, "rb");
            if (f) { fclose(f); found = 1; break; }
        }
        if (!found)
            snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */

    /* Two scalars that make a milestone diff interpretable: a lift that found
     * fewer functions, or a build that registered fewer HLE handlers, explains
     * a whole run going missing without anyone having to bisect the stream. */
    ps3_ms_kv("load:functions", (long long)function_table_count);
    ps3_ms_kv("load:hle_registered", (long long)ps3_hle_count());
    ps3_ms("boot:init_done");
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
    /* CreateThread is a pthread wrapper (win32_compat.h), so the frame clock
     * runs on this host. Without it the host never ticks vblank or drains the
     * FIFO, and the guest waits on fences forever. */
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    if (getenv("PS3_GUEST_PROF"))
        CreateThread(NULL, 0, guest_prof_thread, NULL, 0, NULL);

    ps3_ms("boot:entry");
    /* PS3_MAIN_STACK_LV2=1: give the main thread its stack where lv2 does, in
     * the 0xD0000000 stack region (the first allocation there, which is also
     * what sys_ppu_thread_get_stack_information already reports for it).
     * Guest code can tell: Tornado Outbreak's SPU memcpy (func_000D60E0) copies
     * on the PPU when the destination shares the stack's top nibble, then waits
     * for an SPU reply only the SPU path sends -- with the stack at 0x0FF00000,
     * every low address "is on the stack" and the boot hangs. Opt-in until the
     * regression gate has run with it on. */
    uint32_t stack_top = STACK_TOP;
    { const char* ms = getenv("PS3_MAIN_STACK_LV2");
      if (ms && *ms && *ms != '0') {
          uint32_t base = vm_stack_allocate(&g_vm_stack_alloc, VM_PPU_STACK_SIZE);
          if (base) stack_top = base + VM_PPU_STACK_SIZE;
      } }
    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, stack_top);
    int rc = ppu_run(entry, stack_top);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    /* A guest that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published one
     * -- returning a hardcoded 0 reported success for a run that never got
     * anywhere, which is exactly the kind of green a boot harness must not
     * produce. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
