#!/usr/bin/env python3
"""ppu_frame_locals.py -- a lifted function's own stack frame in host locals.

    python tools/ppu_frame_locals.py <lifted dir> <out dir> --functions <functions.json>

Rewrites the lifter's output (ppu_recomp_*.cpp, written with
--nonvolatile-locals) so that the slots of a function's own frame that
nothing outside the function can see live in C locals instead of guest
memory: the callee-saved register saves and restores (r14-r31, f14-f31, the
LR word), and the compiler's spill slots. Each such access was a page test,
a byte swap and a load or store through the guest address space; a third of
all guest-memory operations in a lift are these.

What is promoted, per function:

  * callee-save slots: a `std rN, off(r1)` / `stfd fN, off(r1)` written once
    whose only reads are the epilogue restores (the lifter already restores
    r14-r31 from its `_cs_` snapshots; f14-f31 and LR get the same);
  * spill slots: an offset every access of which is a literal `r1 + off` of
    one width, written before it is read, and that no callee or other thread
    can reach (below).

A function is left alone when its frame is not its own to reason about: a
tail entry (its `_cs_` snapshots read a parent's slots), a body that
continues in another fragment (`g_trampoline_fn`), a function another
function starts inside, one without a primary frame allocation or with two,
one that copies or indexes r1 in a way this tool does not follow.

The callee rules: a callee may write our frame only in the parameter area
(0x30..0x6F, where it homes its register arguments) and through a pointer we
gave it. So with calls, slots in that area are not promoted, red-zone slots
(negative offsets) are not either, and a slot at or above an address we
passed (`addi rN, r1, K` reaching a call) is promoted only if every write to
it comes after the last call and the function has no backward branch. A
pointer stored to memory (another thread may follow it) keeps everything at
or above its offset in memory for good.

The transformation keeps every guest-visible value the same except the
frame's own words, which no correct program reads from outside.
"""
import argparse, bisect, glob, json, os, re, sys

RE_HDR = re.compile(r"^void (func_[0-9A-Fa-f]+)(?:_lifted)?\(ppu_context\* ctx\) \{$")
RE_ACCESS = re.compile(r"vm_(read|write)(8|16|32|64)\(r1 \+ (-?0x[0-9A-Fa-f]+)(?:, ([^;]*?))?\)(;)?")
RE_ALIAS_ADDI = re.compile(r"^r(\d+) = r1 \+ \(int64_t\)\((-?0x[0-9A-Fa-f]+|-?\d+)\);$")
RE_ALIAS_ADDIC = re.compile(r"^\{ uint64_t _a = r1; uint64_t _r = _a \+ \(uint64_t\)\(int64_t\)\((-?0x[0-9A-Fa-f]+|-?\d+)\); r(\d+) = _r; xer = ")
RE_ALIAS_ADDC = re.compile(r"^\{ uint64_t result = r1 \+ r(\d+); xer = .*; r(\d+) = result; \}$")
RE_CONST = re.compile(r"^r(\d+) = \(int64_t\)\(int32_t\)\((-?0x[0-9A-Fa-f]+|-?\d+)\);$")
RE_ALIAS_COPY = re.compile(r"^r(\d+) = r(\d+)(?: \| 0x0)?;$")
RE_ASSIGN = re.compile(r"^r(\d+) = ")
RE_CALL = re.compile(r"\bfunc_[0-9A-Fa-f]+\(ctx\)|ps3_indirect_call\(ctx\)|ps3_hle_call\(|lv2_syscall\(ctx\)")
RE_LABEL = re.compile(r"^loc_([0-9A-Fa-f]+):")
RE_GOTO = re.compile(r"goto loc_([0-9A-Fa-f]+);")
RE_ALLOC = re.compile(r"^vm_write64\(r1 \+ -0x[0-9A-Fa-f]+, r1\); r1 \+= -0x[0-9A-Fa-f]+;$")
RE_GSAVE = re.compile(r"^vm_write64\(r1 \+ (-?0x[0-9A-Fa-f]+), r(1[4-9]|2\d|3[01])\);$")
RE_GREST = re.compile(r"^r(1[4-9]|2\d|3[01]) = _cs_\d+; ctx->gpr\[\d+\] = _cs_\d+;$")
RE_FSAVE = re.compile(r"^\{ uint64_t tmp; memcpy\(&tmp, &f(1[4-9]|2\d|3[01]), 8\); vm_write64\(r1 \+ (-?0x[0-9A-Fa-f]+), tmp\); \}$")
RE_FLOAD = re.compile(r"^\{ uint64_t tmp = vm_read64\(r1 \+ (-?0x[0-9A-Fa-f]+)\); memcpy\(&f(1[4-9]|2\d|3[01]), &tmp, 8\); \}$")
RE_LRSAVE = re.compile(r"^vm_write64\(r1 \+ (-?0x[0-9A-Fa-f]+), r(\d+)\);$")
RE_LRLOAD = re.compile(r"^r(\d+) = vm_read64\(r1 \+ (-?0x[0-9A-Fa-f]+)\);$")
RE_R1 = re.compile(r"\br1\b")
RE_R1_OK = re.compile(r"vm_(?:read|write)(?:8|16|32|64)\(r1 \+ -?0x[0-9A-Fa-f]+[,)]|ctx->gpr\[1\] = r1|r1 = ctx->gpr\[1\]|^r1 = r1 \+ \(int64_t\)\(-?0x[0-9A-Fa-f]+\);$|^r1 = vm_read64\(r1 \+ 0x0\);$|^uint64_t r0 = ")

PARAM_LO, PARAM_HI = 0x30, 0x70


class Stats:
    def __init__(self):
        self.funcs = self.done = self.saves = self.spills = self.accesses = 0
        self.bail = {}

    def bailed(self, why):
        self.bail[why] = self.bail.get(why, 0) + 1


def slot_name(off, width):
    return f"fl{width}_{'m' if off < 0 else ''}{abs(off):X}"


DEBUG = os.environ.get("PFL_DEBUG")


def promote(body, has_inner, is_fragment, stats):
    """body: the function's statement lines (stripped). Returns the new lines."""
    stats.funcs += 1
    n = len(body)
    if has_inner:
        stats.bailed("inner entry"); return body
    if is_fragment:
        stats.bailed("fragment"); return body         # a trampoline lands here: a parent's frame
    if any("g_trampoline_fn" in l for l in body):
        stats.bailed("continuation"); return body
    if any(l.startswith("uint64_t _cs_") and "vm_read64" in l for l in body):
        stats.bailed("tail entry"); return body
    allocs = [i for i, l in enumerate(body) if RE_ALLOC.match(l)]
    if len(allocs) > 1:
        stats.bailed("two prologues"); return body
    if not allocs and any(RE_CALL.search(l) for l in body):
        stats.bailed("no frame, calls"); return body  # a leaf keeps its locals in the red zone; a caller would not
    locals_idx = next((i for i, l in enumerate(body)
                       if re.match(r"^(uint64_t r\d+|double f\d+|ppu_vr v\d+|uint32_t (cr|xer)|uint64_t ctr) = ctx->", l)), None)
    if locals_idx is None or (allocs and locals_idx > allocs[0]):
        stats.bailed("no locals line"); return body
    # r1 must only appear in the forms we understand
    for l in body:
        if RE_R1.search(l) and not RE_R1_OK.search(l) and not RE_ALLOC.match(l) \
                and not RE_ALIAS_ADDI.match(l) and not RE_ALIAS_ADDIC.match(l) and not RE_ALIAS_ADDC.match(l):
            if re.match(r"^r\d+ = r1(?: \| 0x0)?;$", l):
                stats.bailed("r1 copied"); return body
            if DEBUG and stats.bail.get("r1 used otherwise", 0) < 12:
                sys.stderr.write("  r1 used otherwise: " + l[:150] + "\n")
            stats.bailed("r1 used otherwise"); return body

    # control flow and calls
    labels = {}
    for i, l in enumerate(body):
        m = RE_LABEL.match(l)
        if m: labels[m.group(1)] = i
    all_forward = True
    for i, l in enumerate(body):
        for m in RE_GOTO.finditer(l):
            if labels.get(m.group(1), n) <= i: all_forward = False
    calls = [i for i, l in enumerate(body) if RE_CALL.search(l)]
    has_calls = bool(calls)
    last_call = calls[-1] if calls else -1

    # aliases of r1 (addi rN, r1, K and copies): accesses through them, and
    # where they escape (a call while live, or a store of the pointer)
    alias = {}
    escapes_call = {}     # K -> first index at which a pointer to K reached a call
    escapes_store = set() # K -> pointer written to memory: for good
    alias_access = set()  # (K+disp) offsets touched through a pointer (unknown width -> treat as 8 bytes)
    consts = {}           # rN -> K from `li rN, K`, for `addc rM, r1, rN`
    for i, l in enumerate(body):
        m = RE_ALIAS_ADDI.match(l) or None
        if m:
            alias[int(m.group(1))] = int(m.group(2), 0); continue
        m = RE_ALIAS_ADDIC.match(l)
        if m:
            alias[int(m.group(2))] = int(m.group(1), 0); continue
        m = RE_ALIAS_ADDC.match(l)
        if m:
            if int(m.group(1)) not in consts:
                stats.bailed("r1 + register"); return body
            alias[int(m.group(2))] = consts[int(m.group(1))]; continue
        m = RE_CONST.match(l)
        if m:
            consts[int(m.group(1))] = int(m.group(2), 0)
        elif RE_ASSIGN.match(l):
            consts.pop(int(RE_ASSIGN.match(l).group(1)), None)
        m = RE_ALIAS_COPY.match(l)
        if m:
            d, s = int(m.group(1)), int(m.group(2))
            if s in alias: alias[d] = alias[s]
            else: alias.pop(d, None)
            continue
        # a pointer used as a base of a guest access, or stored, or given to a call
        for r, k in list(alias.items()):
            if re.search(rf"vm_(?:read|write)(?:8|16|32|64)\(r{r} \+ (-?0x[0-9A-Fa-f]+)", l):
                for mm in re.finditer(rf"vm_(?:read|write)(?:8|16|32|64)\(r{r} \+ (-?0x[0-9A-Fa-f]+)", l):
                    alias_access.add(k + int(mm.group(1), 0))
            if re.search(rf"vm_write(?:8|16|32|64)\([^;]*, r{r}\)", l):
                escapes_store.add(k)
            if re.search(rf"\br{r}\b", l) and not re.search(rf"vm_(?:read|write)\d+\(r{r} \+", l) \
                    and not RE_CALL.search(l) and not re.match(rf"^r{r} = ", l) \
                    and not re.search(rf"ctx->gpr\[{r}\] = r{r}", l):
                # arithmetic on the pointer, a compare, anything else: give up on it
                escapes_store.add(k)
        if RE_CALL.search(l):
            for r, k in alias.items():
                escapes_call.setdefault(k, i)
        m = RE_ASSIGN.match(l)
        if m and not RE_ALIAS_ADDI.match(l) and not RE_ALIAS_COPY.match(l):
            alias.pop(int(m.group(1)), None)

    # every literal frame access
    acc = {}   # (off, width) -> list of (idx, is_write)
    for i, l in enumerate(body):
        for m in RE_ACCESS.finditer(l):
            k = (int(m.group(3), 0), int(m.group(2)) // 8)
            acc.setdefault(k, []).append((i, m.group(1) == "write"))
    if not acc:
        stats.bailed("no frame access"); return body

    def overlaps(k):
        o, w = k
        for (o2, w2) in acc:
            if (o2, w2) != k and o < o2 + w2 and o2 < o + w: return True
        for a in alias_access:
            if o < a + 8 and a < o + w: return True
        return False

    # callee-save pairs
    gsave = {}  # (off,8) -> reg
    for i, l in enumerate(body):
        m = RE_GSAVE.match(l)
        if m: gsave[(int(m.group(1), 0), 8)] = int(m.group(2))
    grest = {int(m.group(1)) for l in body for m in [RE_GREST.match(l)] if m}
    fsave, fload = {}, {}
    for i, l in enumerate(body):
        m = RE_FSAVE.match(l)
        if m: fsave.setdefault((int(m.group(2), 0), 8), []).append(int(m.group(1)))
        m = RE_FLOAD.match(l)
        if m: fload.setdefault((int(m.group(1), 0), 8), []).append(int(m.group(2)))
    lr_reg = None
    for i, l in enumerate(body):
        if re.match(r"^r(\d+) = ctx->lr;$", l):
            lr_reg = (int(l[1:l.index(" ")]), i)
            break
    lr_slot = None
    if lr_reg is not None:
        for i in range(lr_reg[1] + 1, min(lr_reg[1] + 40, n)):
            m = RE_LRSAVE.match(body[i])
            if m and int(m.group(2)) == lr_reg[0]:
                lr_slot = (int(m.group(1), 0), 8); break

    # Control flow: statement i falls through to i+1 unless it is a plain goto
    # or return; every `goto loc_X` in a line is an edge; `if (c) return;`
    # only falls through. The must-define analysis below then says, for every
    # read of a slot, whether a write of that slot precedes it on EVERY path
    # from entry -- so a promoted local never stands in for a value the
    # function did not write itself (one left by a callee, or by nobody).
    # A call at or after the point a frame pointer reached a call kills the
    # slots at or above that pointer: the callee may have written them.
    slot_ids = {k: j for j, k in enumerate(sorted(acc))}
    writes_at = [0] * n
    reads_at = [0] * n
    for k, uses in acc.items():
        for i, w in uses:
            if w: writes_at[i] |= 1 << slot_ids[k]
            else: reads_at[i] |= 1 << slot_ids[k]
    # The callee-save slots are exempt from the kills: the ABI forbids a callee
    # to touch its caller's save area, pointer or no pointer.
    cs_slots = {k for k, r in gsave.items() if r in grest} | {k for k, regs in fsave.items() if len(regs) == 1 and k in fload}
    if lr_slot: cs_slots.add(lr_slot)
    kill_at = [0] * n
    for i in calls:
        live = [e for e, idx in escapes_call.items() if idx <= i]
        if live:
            kmin = min(live)
            for k, j in slot_ids.items():
                if k[0] >= kmin and k not in cs_slots: kill_at[i] |= 1 << j
    succ = [[] for _ in range(n)]
    for i, l in enumerate(body):
        targets = [labels[m.group(1)] for m in RE_GOTO.finditer(l) if m.group(1) in labels]
        plain_goto = re.match(r"^goto loc_[0-9A-Fa-f]+;$", l) is not None
        plain_return = l == "return;" or re.match(r"^\{ g_trampoline_fn = .*; return; \}$", l) is not None
        succ[i].extend(targets)
        if not plain_goto and not plain_return and i + 1 < n: succ[i].append(i + 1)
    preds = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]: preds[s].append(i)
    ALL = (1 << len(slot_ids)) - 1
    IN = [ALL] * n
    IN[0] = 0
    OUT = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n):
            inn = 0 if i == 0 else ALL
            for p in preds[i]: inn &= OUT[p]
            if i == 0: inn = 0
            out = (inn & ~kill_at[i]) | writes_at[i]
            if inn != IN[i] or out != OUT[i]:
                IN[i], OUT[i] = inn, out; changed = True
    unsafe = 0
    for i in range(n):
        if reads_at[i]: unsafe |= reads_at[i] & ~IN[i]

    rewrite = {}     # (off,width) -> ("save_g", reg) | ("save_f", reg) | ("save_lr",) | ("spill",)
    for k, uses in acc.items():
        if overlaps(k): continue
        writes = [i for i, w in uses if w]
        reads = [i for i, w in uses if not w]
        if not writes or (unsafe >> slot_ids[k]) & 1:
            continue                                   # a read some path reaches unwritten
        if k in gsave and len(writes) == 1 and not reads and gsave[k] in grest:
            rewrite[k] = ("save_g", gsave[k]); continue
        if k in fsave and len(fsave[k]) == 1 and len(writes) == 1 and k in fload \
                and all(r == fsave[k][0] for r in fload[k]) and len(reads) == len(fload[k]):
            rewrite[k] = ("save_f", fsave[k][0]); continue
        if k == lr_slot and len(writes) == 1:
            # every read must be an LR restore: `rX = vm_read64(r1 + off);` followed by `ctx->lr = rX;`
            ok = True
            for r in reads:
                m = RE_LRLOAD.match(body[r])
                if not (m and int(m.group(2), 0) == k[0] and r + 1 < n and body[r + 1] == f"ctx->lr = r{m.group(1)};"):
                    ok = False; break
            if ok:
                rewrite[k] = ("save_lr",); continue
        # a spill or local
        o = k[0]
        if has_calls:
            if o < 0 or PARAM_LO <= o < PARAM_HI: continue
        below_store = [e for e in escapes_store if e <= o]
        if below_store: continue
        rewrite[k] = ("spill",)                        # calls after an escape are kills in the dataflow
    if not rewrite:
        stats.bailed("nothing promotable"); return body

    # --- apply ---------------------------------------------------------------
    out = []
    decls32, decls64, snaps = [], [], []
    for k, how in sorted(rewrite.items()):
        if how[0] == "spill":
            (decls32 if k[1] == 4 else decls64).append(k)
        elif how[0] == "save_f":
            snaps.append(f"const double fs_{how[1]} = f{how[1]};")
        elif how[0] == "save_lr":
            snaps.append("const uint64_t lr_entry = ctx->lr;")
    for k in list(rewrite):
        if k[1] not in (4, 8) and rewrite[k][0] == "spill":
            del rewrite[k]     # 8/16-bit spills: rare, left in memory
    for i, l in enumerate(body):
        if i == locals_idx:
            out.append(l)
            d = []
            if decls32: d.append("uint32_t " + ", ".join(f"{slot_name(o, 4)} = 0" for o, _ in decls32) + ";")
            if decls64: d.append("uint64_t " + ", ".join(f"{slot_name(o, 8)} = 0" for o, _ in decls64) + ";")
            d += snaps
            if d: out.append(" ".join(d) + "   /* frame slots as locals (ppu_frame_locals.py) */")
            continue
        m = RE_GSAVE.match(l)
        if m and (int(m.group(1), 0), 8) in rewrite and rewrite[(int(m.group(1), 0), 8)][0] == "save_g":
            continue
        m = RE_FSAVE.match(l)
        if m and (int(m.group(2), 0), 8) in rewrite and rewrite[(int(m.group(2), 0), 8)][0] == "save_f":
            continue
        m = RE_FLOAD.match(l)
        if m and (int(m.group(1), 0), 8) in rewrite and rewrite[(int(m.group(1), 0), 8)][0] == "save_f":
            out.append(f"f{m.group(2)} = fs_{m.group(2)};"); continue
        if lr_slot in rewrite and rewrite[lr_slot][0] == "save_lr":
            m = RE_LRSAVE.match(l)
            if m and int(m.group(1), 0) == lr_slot[0]: continue
            m = RE_LRLOAD.match(l)
            if m and int(m.group(2), 0) == lr_slot[0]:
                out.append(f"r{m.group(1)} = lr_entry;"); continue
        # spills: textual substitution of the access expressions
        def sub(m):
            k = (int(m.group(3), 0), int(m.group(2)) // 8)
            if k not in rewrite or rewrite[k][0] != "spill": return m.group(0)
            name = slot_name(*k)
            if m.group(1) == "read": return name + (m.group(5) or "")
            return f"{name} = ({'uint32_t' if k[1] == 4 else 'uint64_t'})({m.group(4)});"
        out.append(RE_ACCESS.sub(sub, l))
    stats.done += 1
    stats.saves += sum(1 for h in rewrite.values() if h[0] != "spill")
    stats.spills += sum(1 for h in rewrite.values() if h[0] == "spill")
    stats.accesses += sum(len(acc[k]) for k in rewrite)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--functions", required=True, help="the lifter's functions.json (start/end per function)")
    ap.add_argument("--only", help="rewrite only this function (a test)")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.src, "ppu_recomp_*.cpp")))
    ranges = {int(f["start"], 0): int(f["end"], 0) for f in json.load(open(a.functions))}
    starts = set()
    fragments = set()      # functions a trampoline continues into: they run in a parent's frame
    re_tramp = re.compile(r"g_trampoline_fn = \(void\(\*\)\(void\*\)\)(func_[0-9A-Fa-f]+);")
    for p in files:
        with open(p, encoding="utf-8", errors="replace") as f:
            for l in f:
                m = RE_HDR.match(l)
                if m: starts.add(int(m.group(1)[5:], 16))
                for m in re_tramp.finditer(l): fragments.add(m.group(1))
    starts = sorted(starts)

    def has_inner(start):
        end = ranges.get(start)
        if end is None:
            i = bisect.bisect_right(starts, start)
            end = starts[i] if i < len(starts) else start + 4
        i = bisect.bisect_right(starts, start)
        return i < len(starts) and starts[i] < end

    os.makedirs(a.dst, exist_ok=True)
    stats = Stats()
    for p in files:
        out = []
        with open(p, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        i = 0
        while i < len(lines):
            m = RE_HDR.match(lines[i])
            if not m:
                out.append(lines[i]); i += 1; continue
            j = i + 1
            while j < len(lines) and lines[j] != "}": j += 1
            body = [l.strip() for l in lines[i + 1:j]]
            start = int(m.group(1)[5:], 16)
            if a.only and m.group(1) != a.only:
                out.extend(lines[i:j + 1]); i = j + 1; continue
            new = promote(body, has_inner(start), m.group(1) in fragments, stats)
            out.append(lines[i])
            for l in new:
                out.append(l if l.endswith(":") else "        " + l)
            out.append("}")
            i = j + 1
        with open(os.path.join(a.dst, os.path.basename(p)), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out))
    for extra in ("ppu_recomp.h", "ppu_hle_nids.cpp"):
        src = os.path.join(a.src, extra)
        if os.path.exists(src):
            with open(src, "rb") as f: data = f.read()
            with open(os.path.join(a.dst, extra), "wb") as f: f.write(data)
    print(f"{stats.funcs} functions, {stats.done} rewritten: {stats.saves} callee-save slots, {stats.spills} spill slots, "
          f"{stats.accesses} frame accesses promoted")
    for why, cnt in sorted(stats.bail.items(), key=lambda kv: -kv[1]):
        print(f"  left alone: {cnt:6d}  {why}")


if __name__ == "__main__":
    main()
