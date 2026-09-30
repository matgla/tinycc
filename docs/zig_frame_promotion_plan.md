# Promoting Zig's frame objects to registers — plan

Written 2026-09-27 for the tcc size work on the Zig compiler (`zig.c`, the C
the Zig C backend emits). Work happens in the worktree
`.cache/zigmem/tccsize` (detached at `zig-abi` 17c4a873); finished steps are
committed on `zig-abi`.

## Goal and metric

The device compiler is 5.71 MB of code; clang `-Oz` builds the same C into
1.96 MB, the same as Zig's own LLVM build. Going through C costs nothing; the
whole gap is tcc's code generation, and 71% of it is spread evenly over
ordinary functions (1.5–3x clang each), not in outliers.

| build of `zig.c` (filler-stripped), cortex-m33, data via r9 | `.text` |
|---|---|
| tcc -O2 (this worktree's cross) | 5,288,782 |
| gcc -Os | 2,875,448 |
| clang -Os | 2,464,126 |
| clang -Oz | 1,957,572 |

Metric: `.text` of `zig.c` compiled by the worktree cross, measured by
`sizecmp.py` / `patterns.py` (in `.cache/zigmem`). Success for this plan is a
sound, default-on change set worth **≥ 500 KB** (10%); the stretch is 1 MB.
Nothing lands that makes plain C worse, or that is not proven on the Zig
compiler end to end.

## Why memory is the lever

A value in a frame slot is invisible to every value-level pass tcc has. Once
stored, it is not constant-propagated, not folded, not CSE'd, not
dead-code-eliminated, and its known-zero upper bits are lost so it is
re-extended on every reload. The register allocator never gets to decide about
it either: a slot is a spill the allocator did not choose.

`Type_toUnsigned` (404 B vs clang 184 B) shows all of it in one function: a
parameter copied to a local and read back through a pointer local, an 8-byte
error union built in four slots and copied three times, a switch selector
reloaded before every compare, a 72-byte frame against clang's 8.

The "small levers" measured earlier (extends 92 KB, store→reload 111 KB,
0xAA stores 83 KB, selector reloads, argument spills) are symptoms of this one
cause. Promotion gets them by construction, plus the cases a peephole cannot
reach.

## The pipeline today

From `-dump-ir-passes=all`: flat passes (`ptr_local_fwd`, `param_copy_alias`,
`sl_forward`, `dse`, …) → SSA loop passes → **`sra` → `ssa_promote` →
`ssa_rename` → `ssa_phi` → SSA value passes** (sccp, cprop, gvn, narrow,
dce, …) → `frame_relayout` (colouring) → `ra_linear_scan`.

So: SRA (`source/opt/flat/memory/sra.c`) turns whole frame objects into
per-field VARs; SSA promotion (`source/ir/ssa.c`, `ssa_var_promotable`) turns
VARs into renamed SSA values with phis; the allocator sees what is left. The
phi machinery exists and works (multi-block defs are promoted with phis). The
gap is in what the two front stages accept.

## Where the bytes are stuck (measured on `zig.c`)

SRA rejections, by object bytes (`TCC_SRA_DBG=1`, with the escaping-op detail
added in this worktree):

| reason | bytes | what it is |
|---|---|---|
| addr-escape via FUNCPARAMVAL | 664 KB | address passed to a call: sret buffers, out-pointers |
| address via FUNCPARAMVAL | 509 KB | same, direct slot address as the argument |
| address via LEA into a VAR | 524 KB | Zig's pointer locals: `t1 = &t0; … *t1` |
| struct-param | 395 KB | a struct passed **by value** from a frame object |
| narrow-store | 186 KB | a byte/halfword store whose value has unknown upper bits |
| BLOCK_COPY operand | 178 KB | objects moved by inline block copies |
| too-big | 95 KB | > 32 bytes |

Promotion refusals: SRA's field VARs are defined by slot STOREs, and
`ssa_var_promotable` keeps an upward-exposed STORE-only VAR memory-resident
(a cycles rule from gcc-torture `loop-15`). In `main_cmdAstCheck`, 308 SRA-made
VARs survive to the final IR unpromoted. Not yet counted TU-wide.

Rejected already, do not retry: **A1** (SRA across calls, materializing before
the call: +62 KB at best, because a Zig temp is used at exactly one call so
building it in registers then storing costs more than building it in place);
**A2** (coalescing word-pair copies in frame colouring: −12 KB, blocked by the
source still being used after the copy). Both are in this worktree behind env
knobs, uncommitted, for reference.

## Phases

Each phase: instrument → census → change behind a knob → measure → make
default → validate (below) → commit. Stop a phase when its census says the
prize is small.

### Phase 1 — Let SSA promotion take what SRA produced — DONE

Result (on zig-abi 3ab7386e, which already had Phase 2): zig.c `.text`
5,139,194 -> 5,047,910 (**-91 KB, -1.8%**); Zig compiler jobs astcheck
1545M -> 1510M, sema 1212M -> 1176M. Two widenings in `ssa_var_promotable`,
both default-on, off with `TCC_DISABLE_PASS=ssa_prom_sra,ssa_prom_single`:

- SRA's field VARs skip the STORE-only rule (census: 22,979 refused for it).
  Capped at 2,000 SRA VARs per function (`TCC_SSA_PROM_SRA_MAX`):
  `main_buildOutputType` has more, and promoting all of them grew it
  141 -> 206 KB. Cap 1000 -30 KB, 2000 -33 KB, 4000 +32 KB (before rebase).
- A VAR whose every definition is in the entry block (entry has no
  predecessors) is renamed even when no phi is needed (census: 20,614
  single-def refusals). The broader "empty dominance frontier" version is
  -117 KB but UNSOUND: the frontier under-reports on the un-rotated loop
  shape (gcc-torture pr78675).
- Both are gated on a VAR never being named at an offset other than its home,
  because renaming clears the offset (`_Complex double`, ieee/cdivchk).
  The SRA range travels in `ir->sra_var_lo/hi`, reset before the SRA call site.

What the census showed first (kept for the record):

1. Census: a `TCC_SSA_DBG` line per VAR the promoter refuses, with the reason
   and whether SRA created it (`ir->next_local_variable` before `sra_objects`
   is the boundary). Count TU-wide.
2. Experiment A: promote SRA-created VARs regardless of the STORE-only rule.
   The `loop-15` cycles concern does not apply to them (they were never
   upward-exposed loop variables in the source).
3. Experiment B: have SRA emit 32-bit field stores of a value as ASSIGN
   definitions, as `sra_store_as_def` already does for narrow fields. Then
   they are not STORE-only and promote under the existing rule.
4. Pick whichever is smaller code and sound; measure `zig.c`, then the IR
   suite's benchmark cycles (the scorecard is cycles, not instructions) to
   confirm plain C is unchanged.

### Phase 2 — Address roots in pointer VARs (524 KB of objects) — DONE by yasos-zig-35

Landed as zig-abi 3ab7386e ("sra: follow object addresses held in pointer
VARs"): "address" rejections 47.8K -> 17.5K objects, .text -153 KB on that
session's build. Coordinated split: that session owns flat pre-SSA SRA
(sra.c, ptr_local_fwd.c), this plan owns ssa.c promotion and frame.c.

`sra_addr_def` follows an object's address only through TEMPs defined once.
Zig writes the address into a VAR (`V0 <-- Addr[StackLoc]`), then copies it to
TEMPs and derefs those. Extend the address-root tracking to a VAR whose every
definition is the same `LEA` of the same object (the `ptr_local_fwd`
condition), with the same escape rules as for TEMPs: every use is an access
base at a constant displacement or feeds another such pointer. Fields reached
through it become SRA fields. `ptr_local_fwd` stays for scalars; check the two
do not fight (order: `ptr_local_fwd` is flat and early, SRA late).

Census first: how many of the 524 KB have a single-LEA pointer VAR and no other
escape.

### Phase 3 — Small structs by value through calls as words (395 KB + callee side)

AAPCS passes a ≤16-byte struct in r0–r3; tcc builds it in the frame and
passes the address of the frame object to `FUNCPARAMVAL`, which is why the
object must stay in memory. Two halves, measured separately:

- **Caller.** `FUNCPARAMVAL` of a word-only struct ≤ 16 B from a frame object
  → N word parameters sourced from the field VARs (`sra_struct_is_word` does
  this for 4-byte structs today; generalize to 8/12/16 with the AAPCS
  even-register rule for 8-byte-aligned members — see the `_Alignas` fix
  2da73cc1).
- **Callee.** An incoming struct parameter is copied to a `FRAME_OBJ_ARG_COPY`
  slot; Zig then does `t2 = a1; t3 = &t2`. Make the arg copy an SRA unit fed
  from the parameter registers, so `t2` gets field VARs and Phase 2 handles
  `t3`.

This is an ABI-adjacent change in the caller's lowering, not in the ABI: the
bytes in r0–r3 are the same. Still, rebuild every tcc object (libc, apps)
before the device suite, as after any call-lowering change.

### Phase 4 — sret and out-pointer buffers (664 + 509 KB): reassess, probably leave

A callee writing through a pointer needs the buffer in memory; clang pays this
too (`sub sp, #8` in `Type_toUnsigned` is exactly one such buffer). The cost
tcc adds is around the buffer: copying it into the caller's own return object
(the helper-copy merge in `frame.c` already removes the memmove form) and
reloading fields that are then stored on. After Phases 1–3, re-measure the
`ldr [slot]; str [sret]` pattern; if it is still large, the fix is in copy
coalescing (A2's residue) or in the return lowering, not in promotion.

### Phase 5 — Real mem2reg for the residue (too-big 95 KB, block-copied 178 KB)

Objects over 32 bytes, or moved by inline block copies, are outside SRA. A
per-slot SSA with phis (the escape analysis already exists in `frame.c` as the
taint/lifetime machinery) is the general answer. Only worth designing if
Phases 1–3 leave more than ~300 KB of the gap; measure first.

### Phase 6 — Allocation quality after promotion

With values in registers the residue is the allocator's: spill order,
rematerializing `add rX, sp, #k` (268 KB today) instead of keeping addresses
live, callee-saved pressure (28,193 saved registers vs clang's 23,220).
Measure after Phase 3; not before.

## Validation, every phase

- Build: `make -j24 armv8m-tcc` in the worktree (configure was
  `--enable-cross --enable-O2`; the four untracked test deps are copied in).
- Size: compile `zig.c` with `. ../agg/inc.sh` flags (2.5 min); `sizecmp.py`
  against the previous object and against `clang_Oz.o`; `patterns.py` for the
  class breakdown.
- Zig end to end: `agg/build_zig_linux.sh O2` (`OUT=` names the binary; the
  zigcmp script overwrites a shared file, do not use it) then
  `zigcmp/check_zig.sh <bin>` at -O1 and -O2. **check_zig is the oracle;** a
  size win that fails it is not a win.
- Plain C: `make test` in the worktree (green baseline 14,213 / 273 skipped at
  the branch point; the two YAFF v4 tests are red at HEAD), then the abcorpus
  object diff for anything the change is not meant to touch.
- Fuzz last, never chained after the suite.
- Device: the QEMU tcc suite (`run_qemu_smoke.sh --no-build tcc_suite_test.py`)
  before the superproject bump; the native tcc is itself compiled by the cross,
  so a cross bug can surface only there.
- When check_zig fails: `mix/globalize.sh` + `fbisectj.sh`; on "neither half",
  `agg/ddmin.sh` for the minimal function set; then a per-function env knob
  (`TCC_…_FN=<name>`) and `TCC_DUMP_FUNC=<name> -dump-ir-passes=all`. A1's
  bug (argument-order: an object passed as PARAMn is written after PARAM0) was
  found this way in about 15 minutes.

## Risks

- **Soundness of relaxed promotion.** The `ssa_var_promotable` rules exist for
  reasons (pr78675, pr125291 in the comments); relax only for VARs SRA created,
  whose every access SRA has already proven.
- **Performance.** Promotion raises register pressure; the allocator may spill
  what it should keep. Measure cycles on the benchmark scorecard after each
  phase, not just size.
- **Aliasing rules.** Everything here must stay C-correct: an object whose
  address reaches a call, a store, or a return stays in memory. A1 tested the
  alternative and it did not pay anyway.
- **Exclusions stay.** `sra_function_eligible` already refuses inline asm,
  setjmp, VLAs, nested functions; keep them refused.
- **Two compilers in one tree.** Changes to call lowering (Phase 3) require
  rebuilding libc and every app before the device suite means anything.

## Non-goals

A1 and A2 (measured, rejected/marginal). Pruning the Zig compiler itself
(separate ~0.5 MB, in the Zig fork). Moving data to the SD card (done,
patch pending). Switch lowering, extends and the other symptom-level levers
(B–F): revisit after Phase 3 with fresh `patterns.py` numbers, since promotion
should have taken most of them.
