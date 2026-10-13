#!/usr/bin/env python3
"""Regression test: --nonvolatile-locals computes what the plain lift computes.

ppu_nv_dirty.py rewrites every lifted body: registers in locals, written
back where they changed and are read, CR fields and the carry apart, one
page test for a prologue's saves and for a run of accesses off one
register. None of that may change what a function does. This test assembles
a small PowerPC program that uses each of those -- a prologue and epilogue
with a CR save, a loop on a compare, a carry chain, float and vector
arithmetic, loads and stores off one register and off the stack, a direct
and an indirect call -- lifts it three ways through the lifter's command
line:

  plain      without --nonvolatile-locals: every register in the context
  locals     with it
  verifying  with it and PPU_NV_VERIFY=1: the form that checks itself

builds each with a small runtime of its own, runs it on a few hundred
random register files and memories, and compares what the outside can see
afterwards: guest memory, the registers a function hands back (r3, r4,
f1-f4, v2), the ones it must preserve, and cr2-cr4. Scratch registers the
callers never read are allowed to differ: leaving those out is the point.
Some runs place the data in a page that takes the accessors' slow path, so
a failed page test is covered as well. The verifying form must also say
nothing.

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping).
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000


# ---- a few instruction encoders: just what the program below uses ----

def _d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def _ds(op, rt, ra, disp, xo):
    return (op << 26) | (rt << 21) | (ra << 16) | (disp & 0xFFFC) | xo


def _x(rt, ra, rb, xo, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def addi(rt, ra, imm): return _d(14, rt, ra, imm)
def li(rt, imm): return _d(14, rt, 0, imm)
def addic(rt, ra, imm): return _d(12, rt, ra, imm)
def mulli(rt, ra, imm): return _d(7, rt, ra, imm)
def lwz(rt, disp, ra): return _d(32, rt, ra, disp)
def stw(rs, disp, ra): return _d(36, rs, ra, disp)
def lbz(rt, disp, ra): return _d(34, rt, ra, disp)
def stb(rs, disp, ra): return _d(38, rs, ra, disp)
def lfs(frt, disp, ra): return _d(48, frt, ra, disp)
def stfs(frs, disp, ra): return _d(52, frs, ra, disp)
def ld(rt, disp, ra): return _ds(58, rt, ra, disp, 0)
def std(rs, disp, ra): return _ds(62, rs, ra, disp, 0)
def stdu(rs, disp, ra): return _ds(62, rs, ra, disp, 1)
def add(rt, ra, rb): return _x(rt, ra, rb, 266)
def adde(rt, ra, rb): return _x(rt, ra, rb, 138)
def subf(rt, ra, rb): return _x(rt, ra, rb, 40)
def neg(rt, ra): return _x(rt, ra, 0, 104)
def mr(ra, rs): return _x(rs, ra, rs, 444)                     # or ra,rs,rs
def cmpw(bf, ra, rb): return (31 << 26) | (bf << 23) | (ra << 16) | (rb << 11)
def cmpwi(bf, ra, imm): return (11 << 26) | (bf << 23) | (ra << 16) | (imm & 0xFFFF)
def mfcr(rt): return _x(rt, 0, 0, 19)
def mtcrf(fxm, rs): return (31 << 26) | (rs << 21) | (fxm << 12) | (144 << 1)
def mflr(rt): return 0x7C0802A6 | (rt << 21)
def mtlr(rs): return 0x7C0803A6 | (rs << 21)
def mtctr(rs): return 0x7C0903A6 | (rs << 21)
def blr(): return 0x4E800020
def bctrl(): return 0x4E800421
def bl(frm, to): return (18 << 26) | ((to - frm) & 0x03FFFFFC) | 1
def bc(bo, bi, frm, to): return (16 << 26) | (bo << 21) | (bi << 16) | ((to - frm) & 0xFFFC)
def fadds(frt, fra, frb): return (59 << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (21 << 1)
def fmuls(frt, fra, frc): return (59 << 26) | (frt << 21) | (fra << 16) | (frc << 6) | (25 << 1)
def fmadds(frt, fra, frc, frb): return (59 << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (29 << 1)
def lvx(vd, ra, rb): return _x(vd, ra, rb, 103)
def stvx(vs, ra, rb): return _x(vs, ra, rb, 231)
def lvlx(vd, ra, rb): return _x(vd, ra, rb, 519)
def _vx(vd, va, vb, xo): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo
def vaddfp(vd, va, vb): return _vx(vd, va, vb, 10)
def vmaxfp(vd, va, vb): return _vx(vd, va, vb, 1034)
def vxor(vd, va, vb): return _vx(vd, va, vb, 1220)
def vandc(vd, va, vb): return _vx(vd, va, vb, 1092)
def vsel(vd, va, vb, vc): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (vc << 6) | 42
def vcmpgtfp_(vd, va, vb): return _vx(vd, va, vb, 710) | (1 << 10)


F1, F2, F3 = BASE, BASE + 0x400, BASE + 0x500


def _program():
    """(code bytes, [(start, end, has a descriptor)])."""
    code = {}

    def emit(at, words):
        for i, w in enumerate(words):
            code[at + 4 * i] = w
        return at + 4 * len(words)

    # F1(r3 = object, r4 = count, f1): the body of the test
    pc = emit(F1, [
        mflr(0), std(0, 16, 1), std(31, -8, 1), std(30, -16, 1), std(29, -24, 1),
        mfcr(12), stw(12, 8, 1), stdu(1, -160, 1),
        mr(31, 3), mr(30, 4), li(29, 0),
    ])
    loop = pc
    pc = emit(pc, [
        mr(9, 31),                                                        # a pointer walked: two runs,
        lwz(5, 40, 9), lwz(6, 44, 9), lwz(7, 48, 9), lwz(8, 52, 9),       # the second off the moved one
        add(29, 29, 5), add(29, 29, 8), addi(9, 9, 8),
        lwz(5, 40, 9), lwz(6, 44, 9), lwz(7, 48, 9), lwz(8, 52, 9),
        add(29, 29, 6), add(29, 29, 7),
        lwz(5, 0, 31), lwz(6, 4, 31), lwz(7, 8, 31), lwz(8, 12, 31),      # a run of loads ...
        add(9, 5, 6), addic(10, 7, -1), adde(9, 9, 8),                    # the carry, from addic to adde
        stw(9, 16, 31), stw(10, 20, 31),                                  # ... and stores
        lfs(2, 24, 31), fadds(3, 1, 2), fmuls(4, 3, 3), fmadds(5, 3, 2, 4),
        stfs(4, 28, 31), lbz(11, 36, 31), stb(11, 37, 31),
        stfs(3, 112, 1), stfs(2, 116, 1), stw(9, 120, 1), stw(10, 124, 1), stfs(5, 128, 1),   # a run off the stack
        cmpw(7, 9, 10),
    ])
    skip = pc + 4 * 6
    pc = emit(pc, [
        bc(4, 7 * 4 + 2, pc, skip),                                       # bne cr7
        mr(3, 31), mr(4, 29), bl(pc + 12, F2),
        add(29, 29, 3), lwz(0, 120, 1),
    ])
    assert pc == skip
    pc = emit(pc, [
        cmpwi(2, 30, 2),                                                  # a field the function must restore
        lwz(11, 32, 31), mtctr(11), mr(3, 29), bctrl(),                   # through a pointer
        add(29, 29, 3), lwz(9, 124, 1), add(29, 29, 9),
        addi(30, 30, -1), cmpwi(0, 30, 0),
    ])
    pc = emit(pc, [bc(12, 0 * 4 + 1, pc, loop)])                          # bgt cr0
    after = pc + 4 * 14
    pc = emit(pc, [
        li(9, 48), lvx(1, 31, 9), li(9, 64), lvx(2, 31, 9), li(9, 99), lvlx(6, 31, 9),
        vaddfp(3, 1, 2), vmaxfp(4, 3, 1), vxor(5, 5, 5), vcmpgtfp_(7, 4, 2),
        bc(12, 6 * 4 + 0, pc + 40, after),                                # blt cr6: every lane greater
        vsel(2, 4, 5, 1), vandc(2, 2, 6), li(9, 0),
    ])
    assert pc == after
    pc = emit(pc, [
        li(9, 80), stvx(2, 31, 9), li(9, 96), stvx(4, 31, 9),
        mr(3, 29), addi(1, 1, 160), lwz(12, 8, 1), mtcrf(0x38, 12),
        ld(0, 16, 1), mtlr(0), ld(31, -8, 1), ld(30, -16, 1), ld(29, -24, 1), blr(),
    ])
    end1 = pc
    # F2(r3 = object, r4): a leaf that dirties scratch registers
    end2 = emit(F2, [
        lwz(5, 0, 3), add(3, 5, 4), li(0, 7), li(11, 9), cmpwi(0, 3, 0),
        bc(4, 0 * 4 + 0, F2 + 20, F2 + 28),                               # bge cr0
        neg(3, 3), blr(),
    ])
    # F3(r3): the function behind the pointer
    end3 = emit(F3, [addi(3, 3, 3), mulli(3, 3, 5), li(12, 1), li(5, -1), blr()])

    size = max(code) + 4 - BASE
    blob = bytearray(b"\x60\x00\x00\x00" * (size // 4))                   # nop
    for a, w in code.items():
        struct.pack_into(">I", blob, a - BASE, w & 0xFFFFFFFF)
    return bytes(blob), [(F1, end1, True), (F2, end2, False), (F3, end3, True)]


HARNESS = r'''
#include "ppu_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the lifted code asks of a runtime, as little of it as will do. */
static uint8_t g_mem[0x100000];
extern "C" { uint8_t* vm_base = g_mem; unsigned char g_ppu_vm_page[65536]; }
static uint64_t rd(uint64_t a, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | g_mem[(uint32_t)(a + i) & 0xFFFFF]; return v; }
static void wr(uint64_t a, uint64_t v, int n) { for (int i = 0; i < n; i++) g_mem[(uint32_t)(a + i) & 0xFFFFF] = (uint8_t)(v >> (8 * (n - 1 - i))); }
extern "C" {
uint8_t  vm_read8_slow (uint64_t a) { return (uint8_t)rd(a, 1); }
uint16_t vm_read16_slow(uint64_t a) { return (uint16_t)rd(a, 2); }
uint32_t vm_read32_slow(uint64_t a) { return (uint32_t)rd(a, 4); }
uint64_t vm_read64_slow(uint64_t a) { return rd(a, 8); }
void vm_write8_slow (uint64_t a, uint8_t v)  { wr(a, v, 1); }
void vm_write16_slow(uint64_t a, uint16_t v) { wr(a, v, 2); }
void vm_write32_slow(uint64_t a, uint32_t v) { wr(a, v, 4); }
void vm_write64_slow(uint64_t a, uint64_t v) { wr(a, v, 8); }
int ppu_stwcx32_ctx(struct ppu_context*, uint64_t, uint32_t, uint32_t) { abort(); }
int ppu_stdcx64_ctx(struct ppu_context*, uint64_t, uint64_t, uint64_t) { abort(); }
uint64_t ppu_timebase_now(void) { return 0; }
void lv2_syscall(ppu_context*) { abort(); }
void ps3_hle_call(uint32_t, ppu_context*) { abort(); }
void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
void ppu_unlifted_stub(ppu_context*) { abort(); }
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = 0;
void ps3_indirect_call(ppu_context* ctx)
{
    if ((uint32_t)ctx->ctr != F3_ADDR) { printf("indirect call to %08X\n", (unsigned)ctx->ctr); exit(3); }
    F3_NAME(ctx);
}
}

static uint64_t s_rng;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

int main(void)
{
    for (int run = 0; run < RUNS; run++) {
        s_rng = 0x9E3779B97F4A7C15ull * (uint64_t)(run + 1);
        static ppu_context c;
        for (size_t i = 0; i < sizeof c; i++) ((uint8_t*)&c)[i] = (uint8_t)rnd();
        for (size_t i = 0; i < sizeof g_mem; i += 8) { uint64_t v = rnd(); memcpy(g_mem + i, &v, 8); }
        /* pages 1..15 are ordinary memory; in some runs the object's page takes the slow path for
         * stores, in some for everything, and so does the stack's */
        memset(g_ppu_vm_page, 0, sizeof g_ppu_vm_page);
        for (int p = 1; p < 16; p++) g_ppu_vm_page[p] = 1 | 2 | 8;
        const uint32_t obj = 0x40000u + 0x10000u * (uint32_t)(run % 3) + 16u * (uint32_t)(rnd() % 200);
        if (run % 5 == 1) g_ppu_vm_page[obj >> 16] = 1;                    /* loads only */
        if (run % 5 == 2) g_ppu_vm_page[obj >> 16] = 0;
        if (run % 7 == 3) g_ppu_vm_page[7] = 0;                            /* the stack */
        /* an object that ends where its page does: the run's two ends lie in different pages */
        const uint32_t at = (run % 4 == 3) ? 0x5FFF0u : obj;
        for (int f = 0; f < 32; f++) { float x = (float)((int64_t)(rnd() % 20001) - 10000) / 16.0f; uint32_t w; memcpy(&w, &x, 4); wr(at + 4u * (uint32_t)f + 24u * (f == 0), w, 4); }
        wr(at + 24, 0x40490FDBu, 4);
        wr(at + 32, F3_ADDR, 4);
        c.gpr[1] = 0x7F000u - 0x100u * (uint32_t)(run % 4);
        c.gpr[3] = at; c.gpr[4] = 1 + (uint64_t)(run % 4);
        c.fpr[1] = (double)(float)((double)((int64_t)(rnd() % 4001) - 2000) / 8.0);
        const ppu_context before = c;
        F1_NAME(&c);
        /* what a caller can see */
        uint64_t h = 1469598103934665603ull;
        #define MIX(p, n) for (size_t i_ = 0; i_ < (size_t)(n); i_++) { h ^= ((const uint8_t*)(p))[i_]; h *= 1099511628211ull; }
        MIX(g_mem, sizeof g_mem)
        MIX(&c.gpr[1], 8) MIX(&c.gpr[2], 8) MIX(&c.gpr[3], 8) MIX(&c.gpr[13], 8 * 19)
        MIX(&c.fpr[1], 8) MIX(&c.fpr[14], 8 * 18) MIX(&c.vr[20], 16 * 12)
        const uint32_t cr = c.cr & 0x00FFF000u; MIX(&cr, 4)
        int kept = c.gpr[1] == before.gpr[1] && !memcmp(&c.gpr[14], &before.gpr[14], 8 * 18)
                   && ((c.cr ^ before.cr) & 0x00FFF000u) == 0;
        printf("%d %016llx r3=%016llx kept=%d\n", run, (unsigned long long)h, (unsigned long long)c.gpr[3], kept);
    }
    return 0;
}
'''


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("clang++") or shutil.which("g++"))


def _lift(td, name, blob, funcs, locals_, verify):
    out = Path(td) / name
    out.mkdir()
    raw = Path(td) / "code.bin"
    raw.write_bytes(blob)
    fj = Path(td) / "functions.json"
    fj.write_text(json.dumps([{"start": s, "end": e, **({"opd": True} if opd else {})} for s, e, opd in funcs]))
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PPU_NV_") or k in ("PPU_FRAME_ENTRY", "PPU_VM_RUNS", "PPU_VM_RUN_MIN", "PPU_DRAIN_ELIDE"):
            del env[k]
    if verify:
        env["PPU_NV_VERIFY"] = "1"
    cmd = [sys.executable, str(ROOT / "tools" / "ppu_lifter.py"), str(raw), "--raw", "--base", hex(BASE),
           "--functions", str(fj), "--jobs", "1", "-o", str(out)]
    if locals_:
        cmd.append("--nonvolatile-locals")
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    return out


def _build_and_run(cxx, td, out, runs):
    src = out / "harness.cpp"
    src.write_text(HARNESS.replace("F3_ADDR", hex(F3) + "u").replace("F3_NAME", f"func_{F3:08X}")
                   .replace("F1_NAME", f"func_{F1:08X}").replace("RUNS", str(runs)))
    exe = out / ("t.exe" if os.name == "nt" else "t")
    units = sorted(str(p) for p in out.glob("ppu_recomp*.cpp"))
    cmd = [cxx, "-std=c++17", "-O1", "-w", "-DPPU_INLINE_VM", "-ffp-contract=off",
           "-I", str(out), "-I", str(ROOT / "runtime" / "ppu"), "-o", str(exe), str(src), *units]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout + r.stderr)[-6000:]
    o = subprocess.run([str(exe)], capture_output=True, text=True)
    assert o.returncode == 0, f"{out.name}: exit {o.returncode}\n" + (o.stdout + o.stderr)[-3000:]
    return o.stdout.splitlines(), o.stderr


def test_locals_compute_what_the_plain_lift_computes():
    cxx = _cxx()
    if cxx is None:
        # same rule as the conformance suite: a skipped run must not pass in CI
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C++ compiler found")
        print("skip test_locals_compute_what_the_plain_lift_computes: no C++ compiler")
        return
    blob, funcs = _program()
    runs = 300
    with tempfile.TemporaryDirectory() as td:
        plain = _lift(td, "plain", blob, funcs, False, False)
        locs = _lift(td, "locals", blob, funcs, True, False)
        ver = _lift(td, "verifying", blob, funcs, True, True)
        text = "".join(p.read_text() for p in sorted(locs.glob("ppu_recomp*.cpp")))
        # the program was written to draw each rewrite: make sure it did
        for what in ("PPU_FRAME_INIT(", "PPU_RUN(31,", "PPU_RUN(1,", "PPU_RR32(", "PPU_RW32(", "uint32_t cr7", "xca",
                     "ppu_vr v1"):
            assert what in text, f"the locals lift has no {what}"
        assert text.count("PPU_RUN(9,") == 2, "the walked pointer should start a run of its own after it moves"
        want, _ = _build_and_run(cxx, td, plain, runs)
        assert len(want) == runs and all(l.endswith("kept=1") for l in want), "\n".join(want[:5])
        got, _ = _build_and_run(cxx, td, locs, runs)
        diff = [(a, b) for a, b in zip(want, got) if a != b]
        assert got == want, f"{len(diff)} of {runs} runs differ, the first:\n  plain  {diff[0][0]}\n  locals {diff[0][1]}"
        got, err = _build_and_run(cxx, td, ver, runs)
        assert got == want, "the verifying form computes something else"
        assert "nv-verify" not in err, err[-2000:]
    print(f"  {runs} runs, three lifts")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
