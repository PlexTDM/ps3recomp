#!/usr/bin/env python3
"""Regression test: the AltiVec operations that are one helper call each.

lvlx/lvrx, lvsl/lvsr, vmaxfp/vminfp, vctsxs/vctuxs and the bitwise
operations (vand, vandc, vor, vxor, vnor, vsel) are lifted to a call of a
helper in the source preamble, which has an SSE form for x86-64 and a
portable one. This test lifts each instruction, compiles the emitted C with
the lifter's own helpers -- once as the host would have them and once with
PPU_NO_SSE_VMX, the portable forms -- runs it and compares vD with a result
worked out here from the instruction's definition:

  lvlx      bytes EA&15 .. 15 of the aligned quadword, left-justified, zeros
            after; lvrx its bytes 0 .. (EA&15)-1, right-justified, zeros before
  lvsl      the bytes sh, sh+1, ..., sh+15; lvsr 16-sh, ..., 31-sh
  vmaxfp    a NaN operand is the result, quieted, vA's before vB's; of two
            zeros the maximum is +0 unless both are -0, the minimum -0 unless
            both are +0
  vctsxs    vB * 2^UIMM toward zero, saturated to int32; a NaN gives 0
  vctuxs    the same to uint32

It needs a host C compiler (under CI=1 a missing one fails instead of skipping).
"""
from __future__ import annotations

import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")


def _lift_one(mnemonic, operands):
    insns = [SimpleNamespace(addr=0x1000, raw=None, mnemonic=mnemonic, operands=operands),
             SimpleNamespace(addr=0x1004, raw=None, mnemonic="blr", operands="")]
    func = L.PPULifter().lift_function(insns, 0x1000, 0x1008)
    return "\n".join(ln for ln in func.body_lines if "return" not in ln)


def _accessors():
    blob = "\n".join(L.PPULifter()._preamble_lines())
    start = blob.index("/* AltiVec register byte order")
    return blob[start:blob.index("/* The guest timebase")]


# ---- the instructions' definitions, on big-endian lanes as 32-bit words ----

def _f32(w):
    return struct.unpack(">f", struct.pack(">I", w))[0]


def _is_nan(w):
    return (w & 0x7F800000) == 0x7F800000 and (w & 0x007FFFFF) != 0


def _vmaxfp(a, b):
    if _is_nan(a):
        return a | 0x00400000
    if _is_nan(b):
        return b | 0x00400000
    if _f32(a) > _f32(b):
        return a
    if _f32(b) > _f32(a):
        return b
    return a & b                    # equal: the same word, or zeros of either sign


def _vminfp(a, b):
    if _is_nan(a):
        return a | 0x00400000
    if _is_nan(b):
        return b | 0x00400000
    if _f32(a) < _f32(b):
        return a
    if _f32(b) < _f32(a):
        return b
    return a | b


def _vctsxs(w, uimm):
    if _is_nan(w):
        return 0
    v = _f32(w) * float(1 << uimm)  # exact in a double
    if v >= 2147483648.0:
        return 0x7FFFFFFF
    if v <= -2147483648.0:
        return 0x80000000
    return int(v) & 0xFFFFFFFF


def _vctuxs(w, uimm):
    if _is_nan(w):
        return 0
    v = _f32(w) * float(1 << uimm)
    if v >= 4294967296.0:
        return 0xFFFFFFFF
    if v <= 0.0:
        return 0
    return int(v)


def _words(ws):
    return struct.pack(">4I", *ws)


SPECIAL = [
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000,     # zeros, ones, infinities
    0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFA00000, 0x7FC12345,                 # NaNs, quiet and signalling
    0x00000001, 0x80000001, 0x7F7FFFFF, 0xFF7FFFFF,                             # denormals, the largest
    0x4F000000, 0xCF000000, 0x4EFFFFFF, 0xCF000001, 0x4F800000, 0x4F7FFFFF,     # 2^31, 2^32 and neighbours
    0x3F000000, 0xBF000000, 0x3FFFFFFF, 0xC0200000, 0x41200000, 0x4B800000,     # fractions, small integers
]


def _cases():
    """(mnemonic, operands, registers {n: 16 bytes}, gprs {n: value}, memory, expected vD)."""
    rnd = random.Random(12345)
    out = []
    mem = bytes(rnd.randrange(256) for _ in range(64))

    def rv():
        return bytes(rnd.randrange(256) for _ in range(16))

    # the loads, at every offset in the quadword at mem + 16
    for ea in range(16, 32):
        sh = ea & 15
        q = mem[16:32]
        out.append(("lvlx", "v3,r4,r5", {3: rv()}, {4: 8, 5: ea - 8}, mem, q[sh:] + bytes(sh)))
        out.append(("lvlx", "v3,0,r5", {3: rv()}, {5: ea}, mem, q[sh:] + bytes(sh)))
        out.append(("lvrx", "v3,r4,r5", {3: rv()}, {4: 8, 5: ea - 8}, mem, bytes(16 - sh) + q[:sh]))
        out.append(("lvsl", "v3,r4,r5", {3: rv()}, {4: 8, 5: ea - 8}, mem, bytes((sh + i) & 0xFF for i in range(16))))
        out.append(("lvsr", "v3,0,r5", {3: rv()}, {5: ea}, mem, bytes((16 - sh + i) & 0xFF for i in range(16))))

    # the float forms: every pair of special values in one lane, the others random
    lanes = list(SPECIAL)
    for i, a in enumerate(lanes):
        for j, b in enumerate(lanes):
            wa = [rnd.choice(lanes) for _ in range(4)]
            wb = [rnd.choice(lanes) for _ in range(4)]
            k = (i + j) & 3
            wa[k], wb[k] = a, b
            regs = {1: _words(wa), 2: _words(wb)}
            out.append(("vmaxfp", "v3,v1,v2", regs, {}, mem, _words([_vmaxfp(x, y) for x, y in zip(wa, wb)])))
            out.append(("vminfp", "v3,v1,v2", regs, {}, mem, _words([_vminfp(x, y) for x, y in zip(wa, wb)])))
    wa, wb = [0x3F800000, 0x80000000, 0x7FC00000, 0x40000000], [0xBF800000, 0x00000000, 0x3F800000, 0xFFC00001]
    out.append(("vmaxfp", "v3,v3,v2", {3: _words(wa), 2: _words(wb)}, {}, mem,
                _words([_vmaxfp(x, y) for x, y in zip(wa, wb)])))
    out.append(("vminfp", "v3,v1,v3", {1: _words(wa), 3: _words(wb)}, {}, mem,
                _words([_vminfp(x, y) for x, y in zip(wa, wb)])))
    for uimm in (0, 1, 8, 31):
        for i in range(0, len(lanes), 4):
            w = (lanes[i:i + 4] + lanes[:4])[:4]
            out.append(("vctsxs", f"v3,v1,{uimm}", {1: _words(w)}, {}, mem, _words([_vctsxs(x, uimm) for x in w])))
            out.append(("vctuxs", f"v3,v1,{uimm}", {1: _words(w)}, {}, mem, _words([_vctuxs(x, uimm) for x in w])))
        for _ in range(12):
            # floats around the integer ranges, either sign
            w = [(0x4A000000 + rnd.randrange(0x06800000)) | (rnd.randrange(2) << 31) for _ in range(4)]
            out.append(("vctsxs", f"v3,v1,{uimm}", {1: _words(w)}, {}, mem, _words([_vctsxs(x, uimm) for x in w])))
            out.append(("vctuxs", f"v3,v1,{uimm}", {1: _words(w)}, {}, mem, _words([_vctuxs(x, uimm) for x in w])))

    # the bitwise forms, apart and in place
    def n(b):
        return int.from_bytes(b, "big")

    def by(x):
        return (x & ((1 << 128) - 1)).to_bytes(16, "big")

    for _ in range(4):
        a, b, c = rv(), rv(), rv()
        regs = {1: a, 2: b, 4: c, 3: rv()}
        out.append(("vand", "v3,v1,v2", regs, {}, mem, by(n(a) & n(b))))
        out.append(("vandc", "v3,v1,v2", regs, {}, mem, by(n(a) & ~n(b))))
        out.append(("vor", "v3,v1,v2", regs, {}, mem, by(n(a) | n(b))))
        out.append(("vor", "v3,v1,v1", regs, {}, mem, a))
        out.append(("vxor", "v3,v1,v2", regs, {}, mem, by(n(a) ^ n(b))))
        out.append(("vxor", "v3,v1,v1", regs, {}, mem, bytes(16)))
        out.append(("vxor", "v3,v3,v3", regs, {}, mem, bytes(16)))
        out.append(("vnor", "v3,v1,v2", regs, {}, mem, by(~(n(a) | n(b)))))
        out.append(("vsel", "v3,v1,v2,v4", regs, {}, mem, by((n(a) & ~n(c)) | (n(b) & n(c)))))
        d = regs[3]
        out.append(("vandc", "v3,v1,v3", regs, {}, mem, by(n(a) & ~n(d))))
        out.append(("vsel", "v3,v3,v2,v4", regs, {}, mem, by((n(d) & ~n(c)) | (n(b) & n(c)))))
        out.append(("vsel", "v3,v1,v2,v3", regs, {}, mem, by((n(a) & ~n(d)) | (n(b) & n(d)))))
    return out


def _cc():
    return shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")


def _arr(b):
    return "{" + ",".join(str(x) for x in b) + "}"


def _program(cases):
    bodies = {}
    src = [
        "#include <stdint.h>", "#include <string.h>", "#include <stdio.h>",
        _accessors(),
        "typedef struct { uint8_t vr[32][16]; uint64_t gpr[32]; uint32_t cr; } ctx_t;",
        "static uint8_t mem[64]; static uint8_t* vm_base = mem; static ctx_t c;",
        "static int check(const ctx_t* ctx, int n, const char* what, const unsigned char* want) {",
        "  if (!memcmp(ctx->vr[3], want, 16)) return 0;",
        "  printf(\"case %d (%s): got \", n, what);",
        "  for (int i = 0; i < 16; i++) printf(\"%02X\", ctx->vr[3][i]);",
        "  printf(\" want \"); for (int i = 0; i < 16; i++) printf(\"%02X\", want[i]);",
        "  printf(\"\\n\"); return 1; }",
    ]
    calls = []
    for i, (mn, ops, regs, gprs, mem, want) in enumerate(cases):
        key = (mn, ops)
        if key not in bodies:
            bodies[key] = len(bodies)
            src.append(f"static void op{bodies[key]}(ctx_t* ctx) {{\n{_lift_one(mn, ops)}\n}}")
        src.append(f"static int case{i}(void) {{ ctx_t* ctx = &c;")
        src.append(f"  static const unsigned char M[64] = {_arr(mem)}; memcpy(mem, M, 64);")
        for r, b in regs.items():
            src.append(f"  {{ static const unsigned char R[16] = {_arr(b)}; memcpy(ctx->vr[{r}], R, 16); }}")
        for r, v in gprs.items():
            src.append(f"  ctx->gpr[{r}] = {v};")
        src.append(f"  static const unsigned char W[16] = {_arr(want)};")
        src.append(f"  op{bodies[key]}(ctx); return check(ctx, {i}, \"{mn} {ops}\", W); }}")
        calls.append(f"  bad += case{i}();")
    src.append("int main(void) { int bad = 0;")
    src.extend(calls)
    src.append("  return bad != 0; }")
    return "\n".join(src)


def test_vmx_helpers_match_the_definitions():
    cc = _cc()
    if cc is None:
        # same rule as the conformance suite: a skipped run must not pass in CI
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_vmx_helpers_match_the_definitions: no C compiler")
        return
    cases = _cases()
    src = _program(cases)
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "t.c"
        c.write_text(src)
        for flags in ([], ["-DPPU_NO_SSE_VMX"]):
            exe = Path(td) / ("t.exe" if os.name == "nt" else "t")
            r = subprocess.run([cc, "-std=c11", "-O1", *flags, "-o", str(exe), str(c)],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr[-4000:]
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            assert out.returncode == 0, f"{' '.join(flags) or 'host forms'}:\n" + out.stdout[-4000:]
    print(f"  {len(cases)} cases, host and portable forms")


def test_bitwise_destination_is_only_written():
    # the write-back pass (ppu_nv_dirty) reads these statements: vD is the
    # first argument and nothing else names it unless it is an operand too
    assert _lift_one("vxor", "v3,v3,v3").strip() == "memset(&ctx->vr[3], 0, 16);"
    for mn in ("vand", "vandc", "vor", "vxor", "vnor"):
        body = _lift_one(mn, "v3,v1,v2").strip()
        assert body == f"ppu_{mn}(&ctx->vr[3], &ctx->vr[1], &ctx->vr[2]);", body


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
