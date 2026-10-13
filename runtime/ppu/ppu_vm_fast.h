/*
 * ps3recomp - inline fast path for the lifted code's guest memory accessors
 *
 * Every lifted load and store calls vm_readN / vm_writeN in ppu_loader.cpp: an
 * out-of-line function that tests the bounds, the read/write watches, the
 * null page, the raw-SPU problem-state window, the SPU reservation set and
 * a handful of env-armed diagnostics before it touches memory. Drakengard 3's
 * render thread spent ~60% of its time inside those functions.
 *
 * With PPU_INLINE_VM defined for the lifted translation units, ppu_recomp.h
 * maps the accessor names onto these: an ordinary access is one lookup in a
 * table of 64 KiB pages, one branch, and a byte-swapped load or store.
 * Anything the slow path exists for clears the page's bit, so it still takes
 * the function:
 *   - below 0x10000 (null page reports / null-store sweep)
 *   - out of the guest VM (vm_oob)
 *   - the raw SPU window at 0xE0000000 (register side effects)
 *   - the page of the GCM ref register (load side; see vm_fastb_read32)
 *   - a page with a line an SPU holds a reservation on (store side; the
 *     function then tests the line itself)
 *   - any env-armed read or store diagnostic (g_ppu_vm_slow_*), or the
 *     run-time word watch g_ww_dyn: every page
 * The slow functions keep their full behaviour, so a run with PPU_WWATCH etc.
 * sees exactly what it always did.
 *
 * One branch, and not the four or five separate tests this used to be,
 * because the tests are repeated at every load and store in the lifted code
 * -- hundreds of thousands of sites -- and each conditional branch at each
 * site wants its own predictor entry. Measured on Drakengard 3 (x86-64, a
 * switch flipped every few seconds inside one run): taking the five-test
 * inline store path instead of calling the function cost the game and render
 * threads 9-11% MORE CPU per frame, although it executes fewer instructions.
 *
 * A store's test is one bit of the page's byte, as a load's is: the byte
 * carries "may be stored to inline" ready-made (PPU_VM_PAGE_ST, kept by
 * ppu_vm_page.h) where each store used to work it out from two others.
 *
 * The accessors take the base of guest memory as an argument (vm_fastb_*).
 * vm_base is a global, and the lifted code stores through pointers that may
 * alias anything, so the compiler reloaded it after every guest store. The
 * lifter gives each function a local of the same name, which the accessor
 * macros in ppu_recomp.h then pick up, and the reloads are gone.
 *
 * The accesses are volatile: guest memory changes under other PPU threads
 * and the SPUs, and a lifted spin on a flag has no call between two loads
 * for the compiler to respect -- the out-of-line calls used to provide that
 * by accident. (Unaligned guest addresses are fine on x86-64 and arm64.)
 */
#ifndef PS3RECOMP_PPU_VM_FAST_H
#define PS3RECOMP_PPU_VM_FAST_H

#include <stdint.h>
#include <string.h>
#include "ppu_vm_page.h"   /* g_ppu_vm_page and its bits */

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t*      vm_base;

uint8_t  vm_read8_slow (uint64_t addr);
uint16_t vm_read16_slow(uint64_t addr);
uint32_t vm_read32_slow(uint64_t addr);
uint64_t vm_read64_slow(uint64_t addr);
void     vm_write8_slow (uint64_t addr, uint8_t  val);
void     vm_write16_slow(uint64_t addr, uint16_t val);
void     vm_write32_slow(uint64_t addr, uint32_t val);
void     vm_write64_slow(uint64_t addr, uint64_t val);
#ifdef __cplusplus
}
#endif

/* Always inline: in the largest lifted functions (thousands of lines of
 * float code) clang's inliner gives up on these and emits real calls --
 * func_00272F58, the hottest function in heavy destruction scenes, had 242
 * call sites to an out-of-line vm_fast_read32/write32. */
#if defined(__GNUC__) || defined(__clang__)
#define PPU_VM_INLINE static inline __attribute__((always_inline))
#else
#define PPU_VM_INLINE static __forceinline
#endif

PPU_VM_INLINE int ppu_vm_load_fast_ok(uint32_t a)
{
    return g_ppu_vm_page[a >> 16] & PPU_VM_PAGE_RD;
}

PPU_VM_INLINE int ppu_vm_store_fast_ok(uint32_t a)
{
    return g_ppu_vm_page[a >> 16] & PPU_VM_PAGE_ST;
}

PPU_VM_INLINE uint8_t vm_fastb_read8(const uint8_t* base, uint64_t ea)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_load_fast_ok(a), 1)) return *(volatile const uint8_t*)(base + a);
    return vm_read8_slow(ea);
}
PPU_VM_INLINE uint16_t vm_fastb_read16(const uint8_t* base, uint64_t ea)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_load_fast_ok(a), 1)) {
        uint16_t v = *(volatile const uint16_t*)(base + a); return __builtin_bswap16(v);
    }
    return vm_read16_slow(ea);
}
PPU_VM_INLINE uint32_t vm_fastb_read32(const uint8_t* base, uint64_t ea)
{
    uint32_t a = (uint32_t)ea;
    /* A read of the GCM ref register is not a plain load: the slow path
     * publishes the next queued RSX fence on it (GCM_REFPOLL), and a title
     * spinning on it in cellGcmFinish waits forever without that. Its page
     * has no RD bit. */
    if (__builtin_expect(ppu_vm_load_fast_ok(a), 1)) {
        uint32_t v = *(volatile const uint32_t*)(base + a); return __builtin_bswap32(v);
    }
    return vm_read32_slow(ea);
}
PPU_VM_INLINE uint64_t vm_fastb_read64(const uint8_t* base, uint64_t ea)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_load_fast_ok(a), 1)) {
        uint64_t v = *(volatile const uint64_t*)(base + a); return __builtin_bswap64(v);
    }
    return vm_read64_slow(ea);
}
PPU_VM_INLINE void vm_fastb_write8(uint8_t* base, uint64_t ea, uint8_t v)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_store_fast_ok(a), 1)) { *(volatile uint8_t*)(base + a) = v; return; }
    vm_write8_slow(ea, v);
}
PPU_VM_INLINE void vm_fastb_write16(uint8_t* base, uint64_t ea, uint16_t v)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_store_fast_ok(a), 1)) { *(volatile uint16_t*)(base + a) = __builtin_bswap16(v); return; }
    vm_write16_slow(ea, v);
}
PPU_VM_INLINE void vm_fastb_write32(uint8_t* base, uint64_t ea, uint32_t v)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_store_fast_ok(a), 1)) { *(volatile uint32_t*)(base + a) = __builtin_bswap32(v); return; }
    vm_write32_slow(ea, v);
}
PPU_VM_INLINE void vm_fastb_write64(uint8_t* base, uint64_t ea, uint64_t v)
{
    uint32_t a = (uint32_t)ea;
    if (__builtin_expect(ppu_vm_store_fast_ok(a), 1)) { *(volatile uint64_t*)(base + a) = __builtin_bswap64(v); return; }
    vm_write64_slow(ea, v);
}
/* The same on whatever `vm_base` names where they are used: the global, or
 * a lifted function's local copy of it. */
#define vm_fast_read8(ea)      vm_fastb_read8(vm_base, (ea))
#define vm_fast_read16(ea)     vm_fastb_read16(vm_base, (ea))
#define vm_fast_read32(ea)     vm_fastb_read32(vm_base, (ea))
#define vm_fast_read64(ea)     vm_fastb_read64(vm_base, (ea))
#define vm_fast_write8(ea, v)  vm_fastb_write8(vm_base, (ea), (v))
#define vm_fast_write16(ea, v) vm_fastb_write16(vm_base, (ea), (v))
#define vm_fast_write32(ea, v) vm_fastb_write32(vm_base, (ea), (v))
#define vm_fast_write64(ea, v) vm_fastb_write64(vm_base, (ea), (v))

/* The frame accesses of a function's entry.
 *
 * A compiled function begins by storing what it has to preserve -- up to
 * eighteen registers, the link register, CR, its arguments' home slots --
 * at fixed offsets from the stack pointer, one after another. Each of those
 * stores tested its page. They all fall in one stretch of stack a few
 * hundred bytes long, so one test of the stretch's two ends says the same
 * for all of them: PPU_FRAME_INIT(sp, lo, hi), placed by the lifter at the
 * top of the function (ppu_nv_dirty.frame_entry), makes that test for the
 * bytes sp+lo .. sp+hi-1 and keeps the host address of sp, and the
 * accessors below then store and load at a constant offset from it. Where
 * the test fails -- a page that wants the function, or a stretch off either
 * end of the 32-bit space -- each access takes its ordinary path, test and
 * all. The stretch is under 64 KiB (the lifter leaves a larger one alone),
 * so its bytes lie in the two pages tested. */
PPU_VM_INLINE int ppu_vm_frame_ok(uint64_t sp, long lo, long hi)
{
    const int64_t a = (int64_t)(uint32_t)sp + lo, b = (int64_t)(uint32_t)sp + hi - 1;
    if (__builtin_expect(((uint64_t)(a | b) >> 32) != 0, 0)) return 0;
    return (g_ppu_vm_page[a >> 16] & g_ppu_vm_page[b >> 16] & (PPU_VM_PAGE_RD | PPU_VM_PAGE_ST))
           == (PPU_VM_PAGE_RD | PPU_VM_PAGE_ST);
}
#define PPU_FRAME_INIT(sp, lo, hi) \
    const int _fk = ppu_vm_frame_ok((sp), (lo), (hi)); \
    uint8_t* const _fb = vm_base + (uint32_t)(sp);
PPU_VM_INLINE uint8_t ppu_frame_read8(int ok, const uint8_t* p, const uint8_t* base, uint64_t ea)
{
    if (__builtin_expect(ok, 1)) return *(volatile const uint8_t*)p;
    return vm_fastb_read8(base, ea);
}
PPU_VM_INLINE uint16_t ppu_frame_read16(int ok, const uint8_t* p, const uint8_t* base, uint64_t ea)
{
    if (__builtin_expect(ok, 1)) { uint16_t v = *(volatile const uint16_t*)p; return __builtin_bswap16(v); }
    return vm_fastb_read16(base, ea);
}
PPU_VM_INLINE uint32_t ppu_frame_read32(int ok, const uint8_t* p, const uint8_t* base, uint64_t ea)
{
    if (__builtin_expect(ok, 1)) { uint32_t v = *(volatile const uint32_t*)p; return __builtin_bswap32(v); }
    return vm_fastb_read32(base, ea);
}
PPU_VM_INLINE uint64_t ppu_frame_read64(int ok, const uint8_t* p, const uint8_t* base, uint64_t ea)
{
    if (__builtin_expect(ok, 1)) { uint64_t v = *(volatile const uint64_t*)p; return __builtin_bswap64(v); }
    return vm_fastb_read64(base, ea);
}
PPU_VM_INLINE void ppu_frame_write8(int ok, uint8_t* p, uint8_t* base, uint64_t ea, uint8_t v)
{
    if (__builtin_expect(ok, 1)) { *(volatile uint8_t*)p = v; return; }
    vm_fastb_write8(base, ea, v);
}
PPU_VM_INLINE void ppu_frame_write16(int ok, uint8_t* p, uint8_t* base, uint64_t ea, uint16_t v)
{
    if (__builtin_expect(ok, 1)) { *(volatile uint16_t*)p = __builtin_bswap16(v); return; }
    vm_fastb_write16(base, ea, v);
}
PPU_VM_INLINE void ppu_frame_write32(int ok, uint8_t* p, uint8_t* base, uint64_t ea, uint32_t v)
{
    if (__builtin_expect(ok, 1)) { *(volatile uint32_t*)p = __builtin_bswap32(v); return; }
    vm_fastb_write32(base, ea, v);
}
PPU_VM_INLINE void ppu_frame_write64(int ok, uint8_t* p, uint8_t* base, uint64_t ea, uint64_t v)
{
    if (__builtin_expect(ok, 1)) { *(volatile uint64_t*)p = __builtin_bswap64(v); return; }
    vm_fastb_write64(base, ea, v);
}
/* e: the access's offset from the sp PPU_FRAME_INIT was given. */
#define PPU_FR8(e, ea)      ppu_frame_read8 (_fk, _fb + (e), vm_base, (ea))
#define PPU_FR16(e, ea)     ppu_frame_read16(_fk, _fb + (e), vm_base, (ea))
#define PPU_FR32(e, ea)     ppu_frame_read32(_fk, _fb + (e), vm_base, (ea))
#define PPU_FR64(e, ea)     ppu_frame_read64(_fk, _fb + (e), vm_base, (ea))
#define PPU_FW8(e, ea, v)   ppu_frame_write8 (_fk, _fb + (e), vm_base, (ea), (v))
#define PPU_FW16(e, ea, v)  ppu_frame_write16(_fk, _fb + (e), vm_base, (ea), (v))
#define PPU_FW32(e, ea, v)  ppu_frame_write32(_fk, _fb + (e), vm_base, (ea), (v))
#define PPU_FW64(e, ea, v)  ppu_frame_write64(_fk, _fb + (e), vm_base, (ea), (v))

/* The same for a run of accesses off any register (ppu_nv_dirty.mem_runs):
 * PPU_RUN(k, reg, lo, span, bits) before the first of them tests the pages
 * of the bytes reg+lo .. reg+lo+span-1 for `bits` -- PPU_VM_PAGE_RD where
 * the run only loads, PPU_VM_PAGE_ST where it only stores, both otherwise --
 * and keeps the host address of reg+lo; k names the pair of locals
 * (PPU_RUN_VARS(k), at the top of the function) and is the register's
 * number. Effective addresses are taken modulo 2^32, as each access takes
 * its own: a stretch that wraps fails the test. */
PPU_VM_INLINE int ppu_vm_run_ok(uint32_t a, uint32_t span, unsigned bits)
{
    const uint32_t b = a + span - 1u;
    return (b >= a) & ((g_ppu_vm_page[a >> 16] & g_ppu_vm_page[b >> 16] & bits) == bits);
}
#define PPU_RUN_VARS(k) int _rk##k; uint8_t* _rb##k;
#define PPU_RUN(k, reg, lo, span, bits) \
    (_rk##k = ppu_vm_run_ok((uint32_t)((reg) + (lo)), (span), (bits)), \
     _rb##k = vm_base + (uint32_t)((reg) + (lo)))
/* PPU_RUN_VERIFY (the lifter's verifying form defines it): every access of a
 * run compares the address it was given with the one it would have worked
 * out itself, and says so where they differ -- a run that outlived its
 * register's value, or was entered from below its test. */
#ifdef PPU_RUN_VERIFY
#include <stdio.h>
PPU_VM_INLINE uint8_t* ppu_run_checked(int ok, uint8_t* p, uint8_t* base, uint64_t ea)
{
    if (ok && p != base + (uint32_t)ea) {
        static int said;
        if (said++ < 64)
            fprintf(stderr, "[nv-verify] run: host address %lld bytes off at guest 0x%08X\n",
                    (long long)(p - (base + (uint32_t)ea)), (unsigned)(uint32_t)ea);
    }
    return p;
}
#define PPU_RUN_P(k, e, ea) ppu_run_checked(_rk##k, _rb##k + (e), vm_base, (ea))
#else
#define PPU_RUN_P(k, e, ea) (_rb##k + (e))
#endif
/* e: the access's offset from the run's first byte. */
#define PPU_RR8(k, e, ea)      ppu_frame_read8 (_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea))
#define PPU_RR16(k, e, ea)     ppu_frame_read16(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea))
#define PPU_RR32(k, e, ea)     ppu_frame_read32(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea))
#define PPU_RR64(k, e, ea)     ppu_frame_read64(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea))
#define PPU_RW8(k, e, ea, v)   ppu_frame_write8 (_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea), (v))
#define PPU_RW16(k, e, ea, v)  ppu_frame_write16(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea), (v))
#define PPU_RW32(k, e, ea, v)  ppu_frame_write32(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea), (v))
#define PPU_RW64(k, e, ea, v)  ppu_frame_write64(_rk##k, PPU_RUN_P(k, e, ea), vm_base, (ea), (v))

/* Store-conditionals with the caller's own context (see ppu_stwcx32_ctx). The
 * lifted code always has `ctx` in scope. */
struct ppu_context;
#ifdef __cplusplus
extern "C" {
#endif
int ppu_stwcx32_ctx(struct ppu_context* self, uint64_t ea, uint32_t expected, uint32_t val);
int ppu_stdcx64_ctx(struct ppu_context* self, uint64_t ea, uint64_t expected, uint64_t val);
#ifdef __cplusplus
}
#endif
#define ppu_stwcx32(ea, e, v) ppu_stwcx32_ctx(ctx, (ea), (e), (v))
#define ppu_stdcx64(ea, e, v) ppu_stdcx64_ctx(ctx, (ea), (e), (v))
#endif /* PS3RECOMP_PPU_VM_FAST_H */
