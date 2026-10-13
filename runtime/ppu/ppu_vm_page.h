/*
 * ps3recomp - the lifted code's page table: one byte per 64 KiB guest page
 *
 * The inline guest-memory accessors (ppu_vm_fast.h) decide with one test of
 * this byte whether a load or a store may touch memory directly or has to
 * call the runtime's function. Two parties own bits in it and change them
 * from different threads:
 *
 *   RD, WR   ppu_loader.cpp (ppu_vm_slow_any_update): the page is ordinary
 *            guest memory, and no diagnostic wants to see its loads/stores
 *   COH      spu_coherency.c: an SPU has reserved a line in the page, and a
 *            store has to go through the function, which tests the line
 *
 * A store needs "WR and not COH". Tested as such at every store that was a
 * load, a mask and a compare; so the byte carries the conjunction as a bit
 * of its own, ST, and whoever changes a bit recomputes it in the same
 * compare-and-swap.
 */
#ifndef PS3RECOMP_PPU_VM_PAGE_H
#define PS3RECOMP_PPU_VM_PAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Defined in spu_coherency.c. All zero, so every access is slow, until
 * ppu_vm_slow_any_update has run once. */
extern unsigned char g_ppu_vm_page[65536];
#ifdef __cplusplus
}
#endif

#define PPU_VM_PAGE_RD   1u   /* loads take the inline path                    */
#define PPU_VM_PAGE_WR   2u   /* stores may too, unless ...                    */
#define PPU_VM_PAGE_COH  4u   /* ... an SPU has reserved a line in the page    */
#define PPU_VM_PAGE_ST   8u   /* WR and not COH: stores take the inline path   */

/* Change a page's byte: clear the bits of `clear`, set those of `set`, and
 * recompute ST, in one step. */
static inline void ppu_vm_page_update(uint32_t page, unsigned char clear, unsigned char set)
{
    unsigned char old = __atomic_load_n(&g_ppu_vm_page[page], __ATOMIC_SEQ_CST);
    for (;;) {
        unsigned char nw = (unsigned char)((old & ~clear & ~PPU_VM_PAGE_ST) | set);
        if ((nw & (PPU_VM_PAGE_WR | PPU_VM_PAGE_COH)) == PPU_VM_PAGE_WR) nw |= PPU_VM_PAGE_ST;
        if (__atomic_compare_exchange_n(&g_ppu_vm_page[page], &old, nw, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    }
}

#endif /* PS3RECOMP_PPU_VM_PAGE_H */
