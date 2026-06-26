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
| **Parser + semantic** | `tccgen.c`, `svalue.c`, `tccir_operand.c` | QEMU corpus + unit base link | No per-feature unit tests |
| **IR core** | `ir/core.c`, `ir/vreg.c`, `ir/pool.c`, `ir/type.c`, `ir/dump.c`, `ir/stack.c` | `ir_pool/type/vreg` unit suites; rest via QEMU | `core`, `dump`, `stack` need targeted tests |
| **Optimizer** | `ir/opt*.c`, `ir/licm.c`, `ir/opt/ssa_opt*.c` | 11 legacy unit suites, 7 SSA golden cases, ledger | Many registered passes still uncovered |
| **Register allocation** | `tccls.c`, `ir/live.c`, `ir/regalloc.c`, `arch/arm/arm_regalloc.c`, `arch/arm/ssa_opt_arm.c` | QEMU corpus only | No isolated RA/interval tests |
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

Extend the unit harness to modules that are currently only linked as dead-weight
in the optimizer suites:

- `ir/core.c` — add `test_ir_core.c`: instruction append/insert/delete, operand
  packing, `tcc_ir_put` front-end coupling points, nop compaction.
- `ir/dump.c` — add `test_ir_dump.c`: deterministic output, pass-name matching,
  ANSI-color gating. This directly protects the golden-IR harness.
- `ir/stack.c` — add `test_ir_stack.c`: stack slot allocation, VLA frame layout.
- `ir/ssa.c` — add `test_ir_ssa.c`: phi insertion, rename tables, dominator frontiers.
- `svalue.c` / `tccir_operand.c` — add `test_svalue.c` and `test_ir_operand.c`:
  operand constructors, type tagging, constant folding helpers.

### Phase 3 — Register allocation coverage

RA bugs are a major self-host miscompile class. Add unit tests around:

- `ir/live.c` — interval construction for straight-line, loops, and calls.
- `tccls.c` — linear scan allocation/spill decisions, callee-saved save/restore.
- `ir/regalloc.c` — phi resolution, phi-copy scheduling, split/merge live ranges.
- `arch/arm/arm_regalloc.c` / `arch/arm/ssa_opt_arm.c` — target-specific
  constraints, coalescing, hard-float register classes.

Mechanism: hand-built IR with known live ranges → assert assigned physical
registers or spill slots. Keep tests deterministic by pinning the allocator's
heuristics (e.g., fixed instruction order, no coalescing surprises).

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

### Phase 5 — Object, linker, and debug info coverage

These are currently the weakest areas. Add host-side golden tests:

- `tests/linker/relocations/` — compile small C snippets to ELF, assert relocation
  types/symbols via `arm-none-eabi-readelf -r`.
- `tests/linker/sections/` — assert section order, alignment, and merging.
- `tests/linker/yaff/` — if YAFF remains supported, assert YAFF output structure.
- `tests/debug/dwarf/` — compile with `-g`, inspect `.debug_info` / `.debug_line`
  for key DIEs and line-number programs.
- `tests/debug/stab/` — same for STAB if still in use.

Target files:
- `tccelf.c`, `tccld.c`, `tccyaff.c`, `tccdbg.c`, `tccdebug.c`.

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
- `tests/unit/arm/armv8m/test_codegen_{arith,mem,control,call,fp,atomic}.c`.
- `tests/unit/arm/armv8m/test_thop_{alu_imm,dsp}.c`.
- `tests/unit/arm/armv8m/test_libtcc_api.c`.
- `tests/fuzz/` for O-level self-consistency and metamorphic fuzzing (already
  referenced by `docs/plan_bug_hunting.md`).
- `scripts/test_selfhost_fat.sh` or similar.

Modify:
- `tests/unit/ut.h` — add `UT_ASSERT_STREQ` when golden/snapshot asserts land.
- `tests/unit/arm/armv8m/Makefile` — new TUs under test.
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
