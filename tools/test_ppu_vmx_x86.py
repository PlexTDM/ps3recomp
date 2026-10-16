#!/usr/bin/env python3
"""The x86-64-v3 forms of runtime/ppu/ppu_vmx.h's helpers give what the scalar
ones give, bit for bit (runtime/ppu/tests/test_ppu_vmx_x86.c).

Builds the check with the host C compiler for x86-64-v3 and runs it. Off
x86-64, or with no compiler, it skips (under CI=1 a missing compiler fails).
"""
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("skip test_ppu_vmx_x86: not x86-64")
        return 0
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        if os.environ.get("CI"):
            print("FAIL test_ppu_vmx_x86: no C compiler")
            return 1
        print("skip test_ppu_vmx_x86: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "test_ppu_vmx_x86"
        src = ROOT / "runtime" / "ppu" / "tests" / "test_ppu_vmx_x86.c"
        cmd = [cc, "-std=gnu17", "-O2", "-march=x86-64-v3", "-ffp-contract=off", "-o", str(exe), str(src)]
        if os.name != "nt":
            cmd.append("-lm")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print("FAIL test_ppu_vmx_x86: build\n" + r.stdout + r.stderr)
            return 1
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode:
            print("FAIL test_ppu_vmx_x86")
            return 1
    print("ok test_ppu_vmx_x86")
    return 0


if __name__ == "__main__":
    sys.exit(main())
