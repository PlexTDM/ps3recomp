# PPU Static Recompilation — loader, ISA, HLE, boot

The PPU half of the recompiler. Where the RSX/SPU work is the **runtime** that a
recompiled game links against, this is the path that turns a game's PowerPC code
into a native host executable: disassemble → lift to C → load → link with the
HLE runtime → run.

`ps3recomp` is a **static recompiler** (à la the N64Recomp family), *not* an
emulator: the game's PPC machine code is translated to C ahead of time and
compiled into a native binary; there is no runtime CPU interpretation. The work
here is game-agnostic — Uncharted: Drake's Fortune (BCUS98103) is only the test
target (we have its decrypted `EBOOT.elf` + an RSX shader corpus).

---

## Pipeline

```
EBOOT.elf (PPC64 ELF, decrypted)
   │
   ├─ ppu_loader.py ──► image manifest + OPD function table + TOC + imports
   │                      (.functions.json / .image.json / .loader.json / .imports.json)
   │
   ├─ ppu_disasm.py ──► PPC64 disassembly
   ├─ ppu_lifter.py ──► ppu_recomp.{c,h}   (one C function per game function)
   │     --functions  <OPD table>          (authoritative function boundaries)
   │     --imports     <imports.json>       (firmware stubs → ps3_hle_call)
   │     --names       <ghidra names>       (optional, for readable comments)
   │
   └─ build: ppu_recomp.c + runtime (ppu_loader.cpp, ppu_hle.cpp + generated
             NID table) + HLE libs (libs/*) ──► native executable
```

---

## Loader

PS3 retail EBOOTs are `ET_EXEC` (fixed load address → **no relocations**;
relocations only matter for `ET_DYN` PRX/.sprx, a later stage). The loader has
two parts:

**Offline (`tools/ppu_loader.py`)** — parses the ELF and emits:
- **Image manifest**: the `PT_LOAD` segments (vaddr / filesz / memsz / flags).
  BSS is `memsz > filesz`.
- **OPD function table**: PS3 function descriptors are 8-byte `{u32 func, u32 toc}`
  pairs (32-bit ABI). A function *pointer* in the binary is an OPD address, so
  indirect calls must resolve OPD → code entry. The `.opd` section (located as
  the section containing `e_entry` — section names are usually stripped) gives
  the **authoritative list of function entry points** (far better than heuristic
  scanning) and the module **TOC** (`r2`). Uncharted: **14,104 functions**,
  entry code `0x10230`, TOC `0x7d3190`.
- **Firmware imports**: via the `proc_prx_param` segment (PT type `0x60000002`,
  magic `0x1b434cec`) → `libstub` table → `(library, NID, stub_addr)` per import.
  Uncharted: **172 imports across 12 libraries** (cellGcmSys, cellSpurs,
  cellSysutil, sysPrxForUser, sys_net, sys_io, sys_fs, …).

**Runtime (`runtime/ppu/ppu_loader.cpp`)** — provides what the lifted code needs:
- big-endian guest memory accessors (`vm_read8/16/32/64`, `vm_write*`),
- `ppu_load_elf(path)` — copies `PT_LOAD` into `vm_base + vaddr`, zeroes BSS,
- the address → function registry (populated by `ppu_recomp_register()`),
- `ps3_indirect_call(ctx)` — dispatches `ctx->ctr` (an already-OPD-resolved code
  address) through the registry,
- `g_trampoline_fn` (TLS) + `ppu_unlifted_stub` (subset/incremental builds),
- `lv2_syscall(ctx)` stub,
- `ppu_run(entry_opd, stack_top)` — resolves the entry OPD, sets `r1`/`r2`,
  dispatches the entry function.

---

## Lifter ABI

Each game function becomes `void func_XXXXXXXX(ppu_context* ctx)` (`ppu_context`
holds `gpr[32]`, `fpr[32]`, `vr[32]` VMX, `cr/lr/ctr/xer/...`). Output is C++
(`extern "C"`, `__declspec(thread)`).

- Direct call (`bl`) → `func_TARGET(ctx); DRAIN_TRAMPOLINE(ctx);`
- Indirect (`bctrl`/`bctr`) → `ps3_indirect_call(ctx)` (target in `ctx->ctr`).
- `bi $r0`-style returns, cross-fragment tail calls via the `g_trampoline_fn`
  TLS mechanism (iterative, avoids deep host recursion).
- `sc` → `lv2_syscall(ctx)`.
- Import stubs (with `--imports`) → `func_STUB(ctx) { ps3_hle_call(NID, ctx); }`.
- Call targets not in this lift → `ppu_unlifted_stub` (a full-image lift has none).
- `ppu_recomp_register()` populates the runtime address→function map.

**ISA coverage**: ~99.8% on real Uncharted code (measured by counting
`/* TODO */` no-ops). Remaining gaps are a few VMX ops the *disassembler*
doesn't decode (`op31_x983`, `vmx_x1098`), not the lifter.

---

## Registers in locals (`--nonvolatile-locals`)

Without the option every guest register a function touches is a load from or
a store to `ppu_context`. With it a function's registers are C locals for
the length of the function, and the context is only what a call goes
through. `tools/ppu_nv_dirty.py` rewrites each lifted body for that; its
module comment has the reasoning, this is the outline:

- **Write back what changed.** A local differs from the context only if the
  function has assigned it since the two last agreed. A forward dataflow
  over the body knows that set at every call, return and tail jump, and
  only it is stored there.
- **... and what is read.** Of the scratch registers (r0, r3-r12, f0-f13,
  v0-v19, CTR, the volatile CR fields, XER's carry) a changed one is stored
  only if something reads it before writing it. That is liveness across
  calls: every function is summarised (what it reads before writing, from
  its entry and from after each of its calls) and the summaries are solved
  over the whole call graph. Code outside the lift is held to the ABI: an
  import, a hook and the target of an indirect call may read the argument
  registers, a native caller the result registers.
- **CR and XER apart.** Each CR field a function uses is a local of its own,
  and so is XER's carry: a compare assigns one small integer and the branch
  after it tests that, where the whole CR word was masked, merged and
  picked apart again. `mfcr`, `mtcrf` and the CR save slot are followed, so
  a function that saves and restores CR still gets the split form.
- **One page test for a stretch of memory.** The inline accessors
  (`runtime/ppu/ppu_vm_fast.h`) test the page of every load and store. The
  saves of a prologue all fall in one stretch of stack, and so do the loads
  and stores that follow one another at constant offsets from one unchanged
  register with no label or call between them: `PPU_FRAME_INIT` and
  `PPU_RUN` test the two ends of such a stretch once, and its accesses
  (`PPU_FR*`/`PPU_FW*`, `PPU_RR*`/`PPU_RW*`) go straight to memory when that
  test passed and take their ordinary path, test and all, when it did not.

The lift prints what it did. Drakengard 3's 1.01 executable, 53,425
functions:

| | |
|---|---|
| stores to the context at calls and returns | 10,118,258 before, 3,731,204 after (at 450,604 transfers) |
| of the scratch registers' write-backs | 1,474,964 of 2,573,187 are live |
| prologues under one page test | 30,161 |
| loads and stores in runs | 686,658, in 87,593 runs |

Also part of the same form: the base of guest memory is a local of each
function (`vm_base`, loaded once: the global may be aliased by any guest
store as far as the compiler can tell), and the trampoline drain after a
call is left out where the callee cannot have set the trampoline.

### Switches

`ppu_lifter.py` reads these from its environment; each turns one part off,
to find which part a fault is in. Lift into a folder of its own and build
that.

| Switch | |
|---|---|
| `PPU_NV_DIRTY=0` | every register stored at every transfer (the form before any of this) |
| `PPU_NV_LIVE=0` | every changed register stored, read or not |
| `PPU_NV_FLAGS=0` | CR and XER as whole words |
| `PPU_NV_VRALL=0` | v0-v19 stay in the context |
| `PPU_FRAME_ENTRY=0` | no shared page test for a prologue's saves |
| `PPU_VM_RUNS=0` | none for runs off one register (`PPU_VM_RUN_MIN=<n>`: the shortest run that gets one, 4) |
| `PPU_DRAIN_ELIDE=0` | the trampoline drained after every call |
| `PPU_VM_BASE_LOCAL=0` | the base of guest memory read from the global |
| `PPU_NV_SUMM=<file>` | the functions' summaries, pickled, to work on the solver without a lift |

### The self-checking form

`PPU_NV_VERIFY=1` lifts a form that checks the passes while the title runs:

- a register left out of a write-back because it had not changed is
  compared with the context there, and a difference is reported as
  `[nv-verify] func_XXXXXXXX ...` (the first sixty);
- a register left out because nothing reads it gets a marker value in the
  context instead (`0xBAD0BAD0BAD000nn`, a signalling NaN with the same
  digits, `0xBD` bytes in a vector register), which a reader the analysis
  missed would not survive quietly, and which shows in any crash dump;
- every access of a run compares the address it was handed with the one it
  would have worked out itself (`[nv-verify] run: ...`).

The checks cost the lifted code about a third more CPU time (Drakengard
3's hub at 193 frames a second instead of 205). Play through what a change
touches and search the log for `nv-verify`; a register dump that shows
marker values in scratch registers is expected, a crash on one is a
finding.

Two of the tools' tests cover this: `tools/test_ppu_nv_dirty.py` assembles a
small program, lifts it plain, with locals and in the self-checking form,
and compares what the three compute on random inputs;
`tools/test_ppu_vmx_helpers.py` checks the vector helpers of the source
preamble (the SSE forms and the portable ones) against the instructions'
definitions.

---

## NID → HLE bridge

PS3 links firmware imports by **NID** (a 32-bit hash of the function name), not
by symbol. The bridge connects a game's import call to our HLE C libraries.

**NID algorithm** (`tools/nid_database.py`, `include/ps3emu/nid.h`): NID =
first 4 bytes of `SHA-1(name + suffix)` read **little-endian**, where the suffix
is the 16-byte secret
`67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A`
(verified against RPCS3 `ppu_generate_id` and real EBOOT imports). *The previous
Python suffix/byte-order was wrong; `nid.h` was already correct.*

**Runtime (`runtime/ppu/ppu_hle.cpp`)**:
- a flat NID → handler table (`ps3_hle_register`, `ps3_nid_table` from nid.h),
- `ps3_hle_call(nid, ctx)` — resolves the NID and marshals the **PPC integer/
  pointer ABI**: args `r3..r10` → C args, return → `r3`. Covers the majority of
  `cellXxx` APIs. *Caveat:* functions that take/return **pointers** need
  host↔guest address translation (per-function wrappers); the generic path
  passes the raw value through.

**Table generation (`tools/gen_hle_nids.py`)**: scans an HLE lib's non-static
function definitions, **computes** each NID, and emits the `ps3_hle_register`
calls (no manual `/* NID */` annotations needed — and some hand-annotations were
found to be wrong).

**Coverage** against Uncharted's 172 imports: **69%** (118/172). cellGcmSys is
100% (26/26). Remaining gaps are concentrated in **networking (sys_net), input
(sys_io), and SPU tasks (cellSpurs)** — none render-critical — plus the
sysPrxForUser CRT.

---

## Boot

`runtime/ppu/tests/boot_main.cpp` links the whole PPU half (lifted code + loader
+ HLE bridge + HLE libs) and dispatches the entry. The integrated build works;
running it executes **real Uncharted startup code** until it hits a function
outside the lifted subset, an unimplemented import, or a syscall — each logged,
so the boot is a live to-do list. First boot stops at **`sys_initialize_tls`**
(NID `0x744680A2`), the textbook first CRT call.

**Full-image build** needs the lifter to **split output into multiple TUs**: a
14,104-function lift is ~88 MB of C in one file, impractical for a single g++
invocation. That output-splitting is the main scaling task.

---

## File map

| File | Role |
|---|---|
| `tools/ppu_disasm.py` | PPC64 disassembler |
| `tools/ppu_lifter.py` | PPC → C lifter (`--functions/--imports/--names`) |
| `tools/ppu_nv_dirty.py` | the lifter's `--nonvolatile-locals` pass: registers in locals, written back where changed and read |
| `tools/ppu_loader.py` | offline loader: image + OPD + TOC + imports |
| `tools/elf_parser.py` | ELF/SELF/PRX parsing primitives |
| `tools/nid_database.py` | NID computation (fixed) + DB tooling |
| `tools/gen_hle_nids.py` | generate NID→handler table from HLE sources |
| `runtime/ppu/ppu_loader.cpp` | runtime loader: image load, mem, registry, dispatch |
| `runtime/ppu/ppu_hle.cpp` | NID→HLE dispatch + PPC ABI adapter |
| `runtime/ppu/ppu_context.h` | `ppu_context` (also emitted into `ppu_recomp.h`) |
| `runtime/ppu/tests/test_loader.cpp` | loader smoke test (real EBOOT) |
| `runtime/ppu/tests/test_hle*.cpp` | HLE adapter + real-library integration tests |
| `runtime/ppu/tests/boot_main.cpp` | integrated first-boot harness |

## Status

| Piece | State |
|---|---|
| Offline loader (image/OPD/TOC/imports) | done, validated on real EBOOT |
| Runtime loader + dispatch | done, executes real lifted code |
| Lifter ISA coverage | ~99.8% on real code |
| NID→HLE bridge + correct NID algorithm | done, 69% import coverage |
| Integrated build + first boot | done; stops at `sys_initialize_tls` |
| Full-image lift (output splitting) | TODO (scaling) |
| HLE completeness (net/input/spurs/CRT) | incremental, mapped by NID |
| Pointer host↔guest wrappers | TODO |
