# Plan: Whole tinycc implementation coverage

> Extends `docs/plan_optimizer_test_coverage.md` (optimizer + codegen size levers)
> to a **source-tree-wide coverage strategy** for the entire ARMv8-M TinyCC fork.
> Goal: every subsystem — frontend, IR, backend, linker, runtime, self-host — has
> a defined, machine-checkable test layer and a CI gate that catches regressions
> per-commit instead of per-device-boot.

## Context

The optimizer plan (`docs/plan_optimizer_test_coverage.md`) is now mostly landed:
`PASS_COVERAGE.md` tracks registered passes, `SOURCE_COVERAGE.md` tracks 118 source
files, unit suites cover 42 files, and golden-IR/codegen-asm harnesses exist. That
work is **pass-centric**. Most of the rest of the compiler (preprocessor, parser,
type system, IR construction, backend instruction selection, register allocation,
ELF/linker, debug info, runtime libraries, the libtcc API, and the self-host
bootstrap) is still only exercised indirectly by the QEMU `ir_tests` corpus and
smoke tests.

This plan names the missing layers, reuses the existing harnesses where possible,
and adds new ones where the subsystem's contract is not naturally tested by the
optimizer work.

## Subsystem map and current state

| Subsystem | Key files | Current coverage | Gap |
|---|---|---|---|
| **Preprocessor** | `tccpp.c` | QEMU `ir_tests` corpus only | No isolated lexer/macro/tests2 test |
| **Parser + semantic** | `tccgen.c`, `svalue.c`, `tccir_operand.c` | `test_svalue.c`, `test_ir_operand.c` | Frontend parser still needs isolated tests |
| **IR core** | `ir/core.c`, `ir/vreg.c`, `ir/pool.c`, `ir/type.c`, `ir/dump.c`, `ir/stack.c` | `ir_pool/type/vreg/core/dump/stack/ssa` unit suites | RA/backend still need targeted tests |
| **Optimizer** | `ir/opt*.c`, `ir/licm.c`, `ir/opt/ssa_opt*.c` | 11 legacy unit suites, 7 SSA golden cases, ledger | Many registered passes still uncovered |
| **Register allocation** | `tccls.c`, `ir/regalloc.c`, `arch/arm/arm_regalloc.c` (`ir/live.c` removed; logic now in `ir/regalloc.c`), `arch/arm/ssa_opt_arm.c` | QEMU corpus only | No isolated RA/interval tests |
| **Backend codegen** | `arm-thumb-gen.c`, `ir/codegen.c`, `ir/machine_op.c`, `tccmachine.c` | QEMU corpus + 5 codegen-asm characterizations | No per-IR-op backend unit tests |
| **Thumb encoder** | `arch/arm/thumb/thop_*.c`, `arch/arm/thumb/thumb.c` | 27 `thop_*` unit suites | `thop_alu_imm`, `thop_dsp` not unit-tested |
| **Inline asm** | `tccasm.c`, `arm-thumb-asm.c`, `thumb-tok.h` | QEMU corpus + `tests/thumb/armv8m/*.S` | Assembler parser has no host unit tests |
| **AAPCS / calls** | `arch/arm/arm_aapcs.c`, `arm-thumb-callsite.c`, `arm-link.c` | QEMU corpus only | ABI edge cases not isolated |
| **Object / linker** | `tccelf.c`, `tccld.c`, `tccyaff.c` | QEMU corpus only | No ELF/linker unit tests |
| **Debug info** | `tccdbg.c`, `tccdebug.c` | QEMU corpus only | No DWARF/STAB unit tests |
| **libtcc API** | `libtcc.c` | QEMU corpus + `tests/libtcc_test*.c` | API not run in CI |
| **Runtime libs** | `lib/*.c`, `lib/*.S`, `lib/fp/*` | exercised by compiled programs | No per-helper unit tests |
| **Self-host bootstrap** | cross/native build, `tests/tests2/*` | smoke tests, manual debugging guide | No automated self-host validation |
| **Tools / scripts** | `scripts/*`, `tcctools.c` | ad-hoc | No regression tests for tooling |

## Strategy: layered coverage

Reuse the three mechanisms already proven:

1. **Isolated `ut.h` unit tests** (`tests/unit/arm/armv8m/`) for pure functions,
   data structures, and IR-algorithm modules.
2. **Golden-IR snapshots** (`tests/ir_tests/golden/`) for passes whose output is
   deterministic and whose input is easier to write in C than in IR.
3. **Codegen disassembly tests** (`tests/ir_tests/asm/`) for backend emission
   contracts tied to size/correctness.

Add three new layers for the rest of the compiler:

4. **Frontend feature tests** (`tests/frontend/<feature>/`) — tiny `.c` inputs
   that exercise a single parser/type/semantic construct, compiled host-side with
   the debug cross and asserted against IR snapshots, diagnostics, or object
   symbols. No QEMU.
5. **Object/linker golden tests** (`tests/linker/`) — small TUs and linker
   scripts, compiled to object/ELF/YAFF, then inspected with `readelf`/`objdump`
   and compared to golden relocations/section layouts.
6. **Self-host smoke gate** — an automated FAT-drive round-trip that rebuilds a
   test with the device native `tcc` and asserts the result matches the host cross.

A single **coverage ledger** (`source_coverage_map.json` / `SOURCE_COVERAGE.md`)
already covers the source tree; this plan extends the *kind* annotations and adds
the new layers to the generator.

## Phases

### Phase 0 — Finish the optimizer plan first

Do not expand into new subsystems until the existing optimizer work is closed:

- [ ] `check_pass_coverage.py` enumerates `PASS`/`PASS_GATED` names in
  `ir/opt_pipeline.c`, diffs against `UT_COVERS(...)` markers + golden dirs, and
  flips to a hard CI fail on any uncovered registered pass.
- [ ] All registered passes in `propagation_passes`, `memory_passes`,
  `late_cleanup_passes`, `entry_store_passes`, and SSA tables have at least one
  test.
- [ ] The legacy↔SSA equivalence harness (Phase F) exists and runs on a small
  corpus.

> This phase is a prerequisite because the optimizer plan already owns most of the
> current active work; expanding scope before it is complete diffuses effort.

### Phase 1 — Frontend coverage layer ✅ Implemented

Target: parser/type/semantic features that are hard to unit-test in isolation but
have clear input/output contracts.

Implemented in `libs/tinycc/tests/frontend/` with a shared pytest harness
(`conftest.py` + `test_frontend.py`). Registered in `make test` via a new
`test-frontend` target and in `run_tests.py --frontend`.

Run:
```bash
make -C libs/tinycc test-frontend
# or
cd libs/tinycc/tests && python run_tests.py --frontend -q
```

#### 1a. Preprocessor/lexer ✅

`tests/frontend/pp/` (14 cases):
- macro expansion order, variadic macros, stringification, token pasting.
- `#if`/`#ifdef` integer constant evaluation edges.
- include-path resolution, pragma handling, `#undef`, predefined macros.

Mechanism: run `armv8m-tcc -E -P` and diff stdout against `.expect`. Builtin
preamble is stripped and `__DATE__`/`__TIME__` are normalized for stable goldens.

#### 1b. Type system / semantic analysis ✅

`tests/frontend/types/` (31 cases):
- arithmetic conversions, qualifiers (`const`, `volatile`, `restrict`),
  `_Alignas`, bit-fields, VLA.
- function types, variadics, `_Noreturn`, storage classes, `inline`, `typedef`,
  `enum`.
- initializer folding, designated initializers, compound literals.
- `struct` / `union`, array decay, pointer arithmetic, casts, `sizeof`, `_Bool`,
  `long long`, `float`, `double`.

Mechanism: run `armv8m-tcc -dump-ir -c` and diff stdout against `.expect`.
The harness auto-falls back to `armv8m-tcc.debug` when the release cross does
not expose `-dump-ir`.

#### 1c. Parser diagnostics ✅

`tests/frontend/diagnostics/` (16 cases):
- Expected-error tests for undeclared identifier, type mismatch, redefinition,
  invalid lvalue, incompatible call, missing semicolon/brace, break/continue
  outside loop, duplicate label, void variable.
- Mechanism: compile with `-Werror`, expect non-zero exit, assert every
  non-empty line of `.stderr` appears as a substring of the captured stderr.

### Phase 2 — IR core + data-structure coverage

Status: **complete**. The following unit suites are wired into `tests/unit` and
pass with `make ut`:

- `ir/core.c` — `test_ir_core.c`: instruction append, operand packing,
  `tcc_ir_put` dest/src validation, call/non-leaf marking, backpatching.
- `ir/dump.c` — `test_ir_dump.c`: deterministic output, pass-name matching,
  ANSI-color gating. This directly protects the golden-IR harness.
- `ir/stack.c` — `test_ir_stack.c`: stack slot lookup, frame size, alignment,
  and legacy physical-register assignment paths.
- `ir/ssa.c` — `test_ir_ssa.c`: construction null/trivial/unsupported-op cases,
  phi insertion on a diamond CFG, and rename-driven use rewriting.
- `svalue.c` / `tccir_operand.c` — `test_svalue.c` and `test_ir_operand.c`:
  operand constructors, negative-vreg encoding, SValue round-trips, and
  `irop_compare_svalue` debug comparisons.

#### Bugs found and fixed during Phase 2

Writing the new suites exposed three real bugs in the IR operand layer; all are
now fixed:

1. `irop_has_no_vreg` was wrong for `IROP_NONE`. It returned false for the
   canonical none operand (`vr == -1`), while `irop_get_vreg` correctly reported
   `-1`. Fixed by making it derive the answer from `irop_get_vreg`.

2. `irop_op_is_{lval,local,llocal,const}` treated every negative `vr` as “no
   operand” and returned `0`. That discarded the `is_lval` / `is_local` flags on
   negative-vreg stack operands (e.g. spilled locals encoded with the sentinel).
   Fixed to check `IROP_TAG_NONE` instead of `vr < 0`.

3. `irop_compare_svalue` did a full `memcmp` over `CValue`. Because `CValue` is
   a union larger than the active member, semantically-equal scalars failed the
   comparison due to uninitialized padding bytes. Fixed to compare `c.i` only.

### Phase 3 — Register allocation coverage ✅ Implemented

Status: **complete**. The following unit suites are wired into
`tests/unit/arm/armv8m/`, run with `make ut`, and registered in
`source_coverage_map.json`:

- `tests/unit/arm/armv8m/test_ra_live.c` — interval construction for
  straight-line code, loop back-edges, and call crossing (covers the interval
  builder now living in `ir/regalloc.c`; the old `ir/live.c` was removed in
  earlier refactoring).
- `tests/unit/arm/armv8m/test_ra_linearscan.c` — linear-scan allocation,
  spill-under-pressure, and callee-saved register use across calls (covers
  `tccls.c` and `ir/regalloc.c`).
- `tests/unit/arm/armv8m/test_ra_phi.c` — phi resolution, explicit copy
  insertion at predecessor block ends, and phi-destination liveness (covers
  `ir/regalloc.c`).
- `tests/unit/arm/armv8m/test_ra_arm.c` — ARM target descriptor
  (`arch/arm/arm_regalloc.c`) and FP/64-bit interval metadata.

Mechanism: hand-built IR → `tcc_ir_ssa_regalloc` → assert assigned physical
registers, spill slots, or interval properties. Tests set a deterministic
allocator environment (`registers_for_allocator`, `float_abi`, etc.) and pin
`optimize = 0` for stable, isolated behaviour.

Because linking `ir/regalloc.c` pulls in the SSA optimizer engine and several
legacy optimization passes that have their own dedicated unit suites, a small
`tests/unit/arm/armv8m/ra_link_stubs.c` file provides no-op definitions for
those symbols. This keeps the RA suites focused on register allocation without
dragging the entire optimizer/backend graph into the unit-test binary.

#### Findings during Phase 3

- `ir/live.c` no longer exists; live-interval construction is now part of
  `ir/regalloc.c` (`ra_build_intervals`). The suite name `test_ra_live.c` is
  kept for plan continuity but tests the equivalent surface inside
  `ir/regalloc.c`.
- The unit-test harness does not initialise the ARM `architecture_config.fpu`
  table, so IR opcodes that consult it (e.g. `TCCIR_OP_CVT_ITOF`) segfault.
  Hard-float coverage is exercised by directly checking the FP interval
  metadata set by `tcc_ir_vreg_type_set_fp` instead of by lowering float IR.
- Pre-RA phi resolution can elide explicit copies when a phi operand is
  coalesced into the phi destination, so the phi tests assert on observable
  outcomes (instruction-count growth and valid allocation) rather than a fixed
  number of inserted `ASSIGN` copies.

### Phase 4 — Backend + codegen coverage

Extend the codegen-asm harness from five size-lever characterizations to
per-instruction-family correctness tests:

- Arithmetic: `ADD/SUB/MUL/DIV/IMOD` with all operand shapes (imm/reg/variadic).
- Memory: `LOAD/STORE/LEA/LOAD_INDEXED/STORE_INDEXED` with all addressing modes.
- Control: `JUMP/JUMPIF/IJUMP/SWITCH_TABLE` and branch narrowing.
- Calls: parameter marshalling per AAPCS, return-value handling, tail calls.
- Floating point: soft-float vs hard-float lowering, VFP instruction selection.
- Atomics / exclusive ops: map IR ops to LDREX/STREX loops or V8-M atomics.

Also add backend unit tests for:
- `ir/codegen.c` — dry-run vs real-run dispatch, two-pass loop invariants.
- `ir/machine_op.c` — machine-op creation and lowering.
- `tccmachine.c` — machine-level store/assign helpers.

#### Status — partially implemented

New unit suites wired into `tests/unit/arm/armv8m/Makefile` and `test_main.c`:
- `test_thop_alu_imm.c` — covers `th_add_imm`, `th_sub_imm`, `th_addw`/`th_subw`,
  and T32-only `rsb`/`adc`/`sbc`/`and`/`bic`/`orr`/`orn`/`eor` modified-immediate
  encodings, plus constraint/fallback paths.
- `test_thop_dsp.c` — covers `uadd8`, `usub8`, `sel`, and `pkhbt` (LSL/ASR shifts),
  plus DSP-feature gating.

New codegen-asm cases in `tests/ir_tests/asm/` with assertions in
`test_codegen_asm.py`:
- `arith_imm_reg.c` — immediate vs register operand shapes, MUL-by-constant strength reduction.
- `arith_div_mod.c` — `sdiv`/`udiv` selection and DIV→MUL→SUB modulo lowering.
- `mem_load_store.c` — PC-relative globals, scaled indexed addressing, immediate-offset struct access, SP-based LEA.
- `control_switch.c` — jump-table switch emission.
- `control_branch.c` — conditional branches and loop back-edges.
- `call_args.c` — AAPCS register arguments, 64-bit register pairs, stack-passed fifth argument.
- `fp_select.c` — soft-float vs hard-float instruction selection.

#### Findings during Phase 4

Tests were written first and adjusted where initial expected encodings were
hand-computed incorrectly; no production changes were made. Two genuine codegen
gaps were exposed and are recorded here (not fixed per instruction):

1. **Hard-float VFP instruction selection is missing.** Even with
   `-mfloat-abi=hard -mfpu=fpv5-sp-d16`, `fp_select.c` lowers `float`/`double`
   operations to `__aeabi_fadd`/`__aeabi_dadd`/`__aeabi_fmul` and passes FP
   values in integer registers. `test_fp_hard_float_uses_vfp` in
   `test_codegen_asm.py` fails because no `vadd.f32`/`vadd.f64`/`vmul.f32`/
   `vmul.f64` instructions are emitted.

2. **Atomic / exclusive-op lowering is not exercised by the codegen-asm layer.**
   The cross compiler does not expose `__atomic_*` builtins under the harness's
   `-nostdlib` compile, and including `<stdatomic.h>` fails because newlib
   headers hit host-include paths and unsupported type constructs. A dedicated
   atomic codegen-asm case is therefore deferred until either the builtins are
   wired or the harness is taught to use the QEMU newlib sysroot includes.

### Phase 5 — Object, linker, and debug info coverage ✅ Implemented

Status: **implemented**. Host-side pytest harnesses live in `tests/linker/` and
`tests/debug/`, are wired into `run_tests.py` (`--linker`, `--debug`) and
`make test-linker` / `make test-debug`, and update the source-coverage ledger.

Implemented cases:

- `tests/linker/relocations/` — external globals produce `R_ARM_ABS32`, external
  function calls produce `R_ARM_THM_JUMP24`, and static locals produce no
  relocations.
- `tests/linker/sections/` — standard section presence/order, custom sections,
  alignment, and current `-ffunction-sections` behaviour (functions stay in a
  single `.text` section rather than per-function subsections).
- `tests/linker/yaff/` — YAFF header structure test.  The cross compiler in this
  tree does not define `TCC_TARGET_YASOS`, so it produces ELF even for `.yaff`
  output; the test skips with a documented note rather than failing.
- `tests/debug/dwarf/` — compile-unit, function/parameter/variable DIEs, and
  line-number program presence.
- `tests/debug/stab/` — documents that STAB emission is disabled in this fork
  (`put_stabs*` in `tccdbg.c` are no-ops); only DWARF sections are emitted.

Target files:
- `tccelf.c`, `tccld.c`, `tccyaff.c`, `tccdbg.c`, `tccdebug.c`.

#### Findings during Phase 5

- `-ffunction-sections` does **not** currently split functions into
  `.text.<name>` subsections; the section test asserts the observed single-`.text`
  layout so a future change will be visible as a failure to flip.
- YAFF output is gated by `TCC_TARGET_YASOS` in `tcc.c`; the host cross compiler
  falls back to ELF.  The YAFF case is therefore a structural check guarded by a
  skip on this build, not a hard failure.
- STAB output is intentionally disabled; the STAB case verifies that no `.stab`
  sections are emitted and then skips.

### Phase 6 — Runtime library coverage

The runtime libs (`lib/*.c`, `lib/*.S`, `lib/fp/*`) are exercised by compiled
programs, but helpers are rarely tested in isolation. Add:

- Host-native unit tests for pure software-FP helpers (`lib/fp/soft/*.c`) where
  the algorithm is architecture-independent.
- Cross-compiled mini-tests for `__aeabi_*`, `__muldi3`, `__divsi3`, `memcpy`,
  `memset`, `longjmp`, and VLA helpers.
- Coverage for `lib/armv8m_eabi.c`, `lib/armeabi.c`, `lib/builtin.c`.

### Phase 7 — libtcc API + tooling coverage

- Run `tests/libtcc_test.c` and `tests/libtcc_test_mt.c` in CI against the built
  `libtcc.a`.
- Add a minimal API test for the ARMv8-M cross target: compile a string in memory,
  extract the code, and check the first instruction bytes.
- Add regression tests for helper scripts (`scripts/qemu_fatdisk_run.py`,
  `scripts/create_disk.py`, etc.) using a tiny synthetic FAT image.

### Phase 8 — Self-host bootstrap gate

The self-host miscompile guide (`docs/selfhost_miscompile_debugging.md`) is
manual. Automate the critical path:

- A nightly or slow CI job that:
  1. Builds the cross and native `tcc`.
  2. Runs a curated subset of `tests/tests2/` through the FAT-drive
     round-trip (put source, compile on device, run, compare output to host cross).
  3. Fails on any behavioral divergence.
- A lighter PR gate: compile the tinycc source with the cross and run a small
  subset of `tests/tests2/` through the resulting native binary on QEMU without
  rebuilding the kernel (reuse a prebuilt kernel + FAT swap of a smaller test
  harness if possible).

### Phase 9 — Coverage ledger + CI gate

Extend the existing generator:

- Update `source_coverage_map.json` kinds to include the new layers:
  `frontend`, `ra`, `backend_unit`, `linker`, `debug`, `runtime_unit`,
  `libtcc_api`, `selfhost`.
- `gen_source_coverage.py --check` fails on any source file missing from the map
  or any stale `SOURCE_COVERAGE.md`.
- Add a CI job that runs the new frontend/linker/debug/runtime suites and the
  self-host gate.

## Risk-prioritized first batch

Order = (self-host miscompile frequency) × (isolation difficulty) × (blast radius
of a bug):

1. **RA / live intervals** (Phase 3) — matches the current biggest failure class.
2. **IR core / dump** (Phase 2) — cheap, protects every golden-IR test.
3. **Backend per-op codegen** (Phase 4) — high value, can reuse codegen-asm harness.
4. **Frontend type/parser** (Phase 1) — large surface but many tiny tests.
5. **Object/linker** (Phase 5) — needed before any ELF format changes.
6. **Self-host gate** (Phase 8) — the only end-to-end proof for the cross.

## Verification

- `make ut` stays green and grows by the new unit suites.
- `pytest tests/frontend/` passes host-side.
- `pytest tests/linker/` passes host-side.
- `pytest tests/ir_tests/test_codegen_asm.py` covers backend correctness, not
  just size levers.
- `make test-selfhost` or the nightly job shows zero divergence on the curated
  `tests2` subset.
- `python3 tests/unit/gen_source_coverage.py --check` passes in CI.

## Files / deliverables

New:
- `docs/plan_whole_tinycc_coverage.md` (this file).
- `tests/frontend/` tree with runner.
- `tests/linker/` tree with runner.
- `tests/debug/` tree with runner.
- `tests/unit/arm/armv8m/test_ir_{core,dump,stack,ssa,operand,svalue}.c`.
- `tests/unit/arm/armv8m/test_ra_{live,linearscan,phi,arm}.c`.
- `tests/unit/arm/armv8m/ra_link_stubs.c` — isolates RA suites from the SSA
  optimizer/backend dependency graph.
- `tests/ir_tests/asm/{arith_imm_reg,arith_div_mod,mem_load_store,control_switch,control_branch,call_args,fp_select}.c` +
  assertions in `tests/ir_tests/test_codegen_asm.py`.
- `tests/unit/arm/armv8m/test_thop_{alu_imm,dsp}.c`.
- `tests/unit/arm/armv8m/test_codegen_{arith,mem,control,call,fp,atomic}.c` (pending — backend unit tests for `ir/codegen.c`, `ir/machine_op.c`, `tccmachine.c` not yet written).
- `tests/unit/arm/armv8m/test_libtcc_api.c`.
- `tests/fuzz/` for O-level self-consistency and metamorphic fuzzing (already
  referenced by `docs/plan_bug_hunting.md`).
- `scripts/test_selfhost_fat.sh` or similar.

Modify:
- `tests/unit/ut.h` — add `UT_ASSERT_STREQ` when golden/snapshot asserts land.
- `tests/unit/arm/armv8m/Makefile` — new TUs under test.
- `tests/unit/arm/armv8m/stubs.c` — remove `tcc_ls_*` stubs now that the real
  `tccls.c` is linked.
- `tests/unit/arm/armv8m/test_main.c` — register new suites.
- `tests/unit/source_coverage_map.json` — annotate new files and layers.
- `.github/workflows/ci.yml` — new jobs for frontend, linker, self-host.

## Relationship to other plans

- `docs/plan_optimizer_test_coverage.md` — this plan's **Phase 0**.
- `docs/plan_binary_size_reduction.md` — codegen-asm tests from this plan become
  the regression lock for size levers.
- `docs/plan_bug_hunting.md` — fuzz/metamorphic tracks feed bugs into the
  appropriate subsystem suite.
- `docs/selfhost_miscompile_debugging.md` — the self-host gate (Phase 8)
  operationalizes the manual workflow.

## Stop criterion

Every non-test source file in `libs/tinycc/` is listed in
`source_coverage_map.json` with a kind other than `ir_test` or `none`, and every
CI job that corresponds to a kind is green. Until then, `gen_source_coverage.py
--check` fails the build.
