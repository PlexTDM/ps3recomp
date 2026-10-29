/*
 * ps3recomp - lifter torture-test harness (host side).
 *
 * Boots a self-checking guest ELF (tests/guest/*.c compiled with the real
 * PS3 SDK ppu-lv2-gcc) through the full pipeline: loader + lifted code + HLE
 * bridge + lv2 syscalls. The guest prints one line per test:
 *
 *     ok <n> <name>
 *     FAIL <name> got=<hex> want=<hex>
 *     TORTURE COMPLETE pass=<n> fail=<n>
 *
 * tests/run_torture.py parses that output. This binary's job is only to boot
 * the guest and make every failure mode LOUD:
 *   - crash        -> [CRASH] + module RVA (llvm-symbolizer against the .map)
 *   - guest hang   -> [TIMEOUT] after $TORTURE_TIMEOUT seconds (default 30),
 *                     thread RIP dump, exit code 2 (a hang IS a test failure:
 *                     e.g. the newlib dtoa infinite loop class of lifter bug)
 *   - clean finish -> exit 0; the text output carries pass/fail counts
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}


#define STACK_TOP  0x0FF00000u

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;
extern "C" void lv2_init_syscalls(void);

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{ ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7); }


int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <guest.elf>\n", argv[0]); return 64; }

    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    ppu_vfs_root = ".";

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();

    g_ps3_guest_caller = harness_guest_caller;

    fprintf(stderr, "[torture] dispatching entry OPD 0x%08X\n", entry);
    int rc = ppu_run(entry, STACK_TOP);
    fprintf(stderr, "[torture] ppu_run returned %d\n", rc);
    return 0;
}
