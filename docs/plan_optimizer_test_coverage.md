# Plan: Comprehensive optimizer + codegen test coverage

> Companion to `plan_binary_size_reduction.md`. This is the **test layer that gates
> Phase 4** (legacy→SSA optimizer consolidation) of that plan and regression-locks
> the Phase 1–3 codegen wins.

## Context

The path to ~1 MB in `plan_binary_size_reduction.md` depends on **Phase 4 — merging the
legacy linear-IR optimizer (`ir/opt_*.c`) into the SSA optimizer and deleting subsumed
passes** (~200–300 KB, "the 1 MB-maker"). In a fork with a documented history of subtle,
pass-local miscompiles, deleting/rewriting passes is only safe with strong per-pass tests.

Today the optimizer is validated almost entirely **end-to-end**: the `tests/ir_tests/*.expect`
QEMU tests and the `tests/smoke/tcc_suite_test.py` gcc-torture suite. That catches *many*
miscompiles but **corner cases slip through** — the bug history is full of pass-local
defects that only surfaced as device HardFaults: known-bits load-width byte-drop, literal-pool
cross-placement, struct-by-value 9-byte packed operand, wide-string-literal merge, missing
`is_lval` guards in cmp-fold, LICM negative loop target, `ssa_opt_branch` `instr_to_block`
bounds. There is currently **zero isolated coverage of any optimization pass** and **no
coverage of backend instruction selection/emission** (`tests/unit/`'s `thop_*` suites cover
encoders only, not codegen).

**Goal:** a host-side (seconds, no QEMU) test layer covering *every* pipeline-registered pass
and the codegen size levers, with a CI gate so the optimizer merge cannot silently drop
coverage and corner-case regressions are caught per-commit instead of per-device-boot.

Decisions:
- **Hybrid mechanism** (effort split across three harnesses, below).
- **Hard CI gate on all registered passes** as the end-state acceptance criterion: the
  fan-out drives coverage to 100% of pipeline-registered passes, then CI fails on *any*
  uncovered registered pass — every new/merged pass must ship with a test.

## Strategy: three complementary harnesses

| Surface | Primary mechanism | Why |
|---|---|---|
| Legacy linear passes (`ir/opt_*.c`) | **Isolated `ut.h` unit tests** with hand-built IR | Surgical control over adversarial IR the C frontend can't emit; microsecond, deterministic; this is exactly where smoke misses and where merge risk concentrates |
| SSA passes (`ir/opt/ssa_opt_*.c`) | **Golden-IR snapshots** via `-dump-ir-passes` (host, no QEMU) | Hand-building a valid CFG+SSA per test is impractical; snapshotting the real pipeline after a named pass fits |
| Backend codegen / size levers | **objdump mnemonic pattern + count/threshold tests** (host, no QEMU) | cbz/cbnz fusion, `b.w`→`b.n`, R9 spill elim, struct byte handling are emission-level; counting mnemonics ties tests directly to the byte-size goal and locks in Phase 1–3 wins |

A **coverage ledger** (machine-checkable) unifies the three and drives the CI gate.

### Verified feasibility facts
- Every pass is `int tcc_ir_opt_<name>(TCCIRState *ir)` — returns a change count, mutates IR in
  place. Bare `(ir)` entry points are self-contained (they build whatever DU/loop info they
  need internally — that's why the cascades in `ir/opt_pipeline.c:223-302` call them with just
  `ir`). Passes exposed only as `_ex(IROptCtx*)` take an `IROptCtx` (`tcc_ir_opt_ctx_init`,
  `ir/opt_engine.c`) whose require-helpers lazily build DU/blocks/loops.
- IR is hand-buildable: operand constructors `irop_make_vreg/imm32/symref/stackoff`
  (`tccir_operand.h:393-507`), accessors/setters `tcc_ir_op_get_*` / `tcc_ir_op_set_*`
  (`tccir.h:705-854`). **Do not** build via `tcc_ir_put` (`ir/core.c`) — frontend-coupled
  (`SValue*`, auto-coalesce, `file->line_num`). Follow the minimal-`TCCIRState` pattern already
  used by `tests/unit/arm/armv8m/test_ir_vreg.c` / `test_ir_pool.c` (README Pattern B/C).
- The **real IR dumper is `tcc_ir_show`** (`ir/dump.c:1069`); `tcc_ir_dump` (`ir/dump.c:196`)
  is a dead stub. Output is **deterministic / snapshot-safe**: index + op name + `V/T/P<n>`,
  `#imm`, `GlobalSym(<tok>)`, `StackLoc[..]`, `JMP to <idx>` — no pointers/addresses/hashes.
  ANSI spill coloring is gated behind `show_physical_regs`, forced to 0 by `dump_ir_after_pass`
  (`tccgen.c:29030`).
- `-dump-ir` / `-dump-ir-passes=name[,...]` (or `all`) print `=== AFTER <name> ===` blocks via
  `dump_ir_after_pass()` / the `RUN_PASS()` macro — **but all `#ifdef CONFIG_TCC_DEBUG`**
  (`Makefile:51-53`, enabled by `./configure --debug`). The shipped release `bin/armv8m-tcc`
  prints nothing; the golden runner needs a debug-enabled **host** build.
- `arm-none-eabi-objdump` is available for the codegen tests.
- Pass count: ~137 `tcc_ir_opt_*` entry points (~68 unique after stripping `_ex`); the
  authoritative "must cover" set is the pipeline-**registered** passes (the `PASS` / `PASS_GATED`
  string-literal names in `ir/opt_pipeline.c` + the SSA pass tables).

## Phases

### Phase A — Harness foundation
- **`tests/unit/arm/armv8m/ir_build.{h,c}`** (new): test-only IR builder. `ut_ir_new/free`
  (minimal zeroed `TCCIRState`, init only touched fields + live-interval arrays, reuse
  `ut_init_intervals` from `test_ir_vreg.c`); `ut_emit(ir, op, dest, src1, src2)` appends an
  `IRQuadCompact` and pushes present operands (per `irop_config[op].has_*`) into
  `iroperand_pool` matching the `tcc_ir_op_get_*` layout; wrappers `ut_vreg/ut_imm/
  ut_jump_target` + flag setters (`is_lval`, `is_jump_target`); assertion helpers `ut_op/
  ut_dest/ut_is_nop` and `ut_snapshot(ir, buf)` (capture `tcc_ir_show` for sequence asserts).
  ~150 lines, reused by every legacy-pass suite.
- **`tests/unit/ut.h`** (modify): add `UT_ASSERT_STREQ` and a no-op `UT_COVERS("<pass>")`
  annotation macro the ledger script greps for.
- **`tests/unit/arm/armv8m/Makefile`** (modify): add `ir_build.c` to `UT_LOCAL_SRCS`; extend
  `UT_MODULE_SRCS` with opt TUs under test + pure-IR deps (`ir/opt_engine.c`, `ir/cfg.c`,
  `ir/licm.c`, selectively `ir/core.c`). Resolve link gaps via the stub-priority list in
  `tests/unit/README.md` (extend `stubs.c` / `tcc_state_stub.c`).
- **`Makefile`** (modify): add a `tcc-debug-host` target (host `tcc` with `-DCONFIG_TCC_DEBUG`)
  for the golden runner; add a `test-opt` aggregate target (unit suites + golden + codegen
  pytest + ledger check).

### Phase B — Tier-1 isolated-unit suites (legacy hot spots; open each with the known bug)
New `UT_SUITE` files in `tests/unit/arm/armv8m/`, each starting with the exact historical
miscompile as a regression test, then broadening; register each in `test_main.c`:
- `test_opt_knownbits.c` — load-width (1/2/4-byte) preservation, sign-vs-zero extend, masks.
- `test_opt_constfold.c` — IMM fold into CMP/arith, literal-pool placement, 32-bit wrap, LLONG/INT.
- `test_opt_constprop.c` — single-def prop; `addrtaken`-clear prologue (`opt_pipeline.c:318-325`).
- `test_opt_copyprop.c` — `is_lval`/DEREF preservation (`opt_copyprop.c:210-293`).
- `test_opt_cmpfold.c` — `cmp_expr_fold` / `cmp_const_offset_fold` / `cmp_field_fuse` `is_lval` guards.
- `test_opt_licm.c` — invariant only hoisted to a real preheader, never a negative/own-loop index.

### Phase C — Golden-IR snapshot harness (SSA primary + whole-pipeline)
- **`tests/ir_tests/golden/<pass>/<case>.c` + `.expected`** (new) and runner
  **`tests/ir_tests/test_golden_ir.py`**: run `tcc-debug-host -dump-ir-passes=<pass> -c case.c`,
  extract the `=== AFTER <pass> ===` block, diff against `.expected`; `--update` regenerates.
- Primary mechanism for SSA passes: `ssa_opt_branch` (the `instr_to_block` bug), `ssa_opt_fold`,
  `ssa_opt_sccp`, `ssa_opt_cprop`, `ssa_opt_gvn`, `ssa_opt_load_cse`, `ssa_opt_narrow`.

### Phase D — Codegen disassembly tests (size levers)
- **`tests/ir_tests/test_codegen_asm.py`** (new): cross-compile `-c` (no link/boot), `objdump -d`,
  assert (a) mnemonic presence/absence + counts, (b) per-function instruction/byte thresholds.
  Targets mirror `plan_binary_size_reduction.md` Phases 1–2: R9 GOT-base spill elimination,
  `b.w`→`b.n` narrowing, cbz/cbnz fusion, plus struct by-value 9-byte packed operand and
  wide-string-literal merge. Reuse the cross-compile invocation from `tests/ir_tests/qemu_run.py`,
  stopping before QEMU. Counting beats full goldens (robust to scheduling churn); keep full-disasm
  goldens only for the trickiest sequences.

### Phase E — Coverage ledger + CI gate
- **`tests/unit/check_pass_coverage.py`** (new): enumerate registered passes from the
  `PASS`/`PASS_GATED` string-literal names in `ir/opt_pipeline.c` + the SSA pass tables; collect
  tested passes from `UT_COVERS(...)` markers + `tests/ir_tests/golden/<pass>/` dirs; diff and
  report gaps. **`tests/unit/PASS_COVERAGE.md`** (new): checked-in ledger (pass → group →
  test file(s) → kind → risk tier).
- Wire into CI alongside `make ut` (already `0 failed`-gated) via `make test-opt`. Gate policy:
  during fan-out the script reports gaps non-fatally; once Phase F reaches 100% of registered
  passes it flips to **hard fail on any uncovered registered pass** (the agreed end state).

### Phase F — Fan out to all registered passes + merge-equivalence harness
- One suite/golden per remaining registered pass until the ledger is 100% (then flip the gate).
- **Legacy↔SSA equivalence harness** (the direct Phase-4 de-risker): for each legacy pass the
  merge will subsume, snapshot its input→output on a corpus of small `.c` inputs; after the pass
  is subsumed in SSA, diff old-path vs new-path `-dump-ir` for behavioral equivalence. Turns the
  risky merge into a green/red signal.

### Phase G — Source-tree coverage ledger + generator
Phases A–F focus on optimizer passes and codegen size levers, but most of the tinycc source tree
(frontend, IR core, backend drivers, runtime libs) is only exercised indirectly by end-to-end
QEMU/smoke tests. This phase adds a **file-level coverage ledger** so every TU knows which test
layer covers it and which files have no dedicated coverage.
- **`tests/unit/gen_source_coverage.py`** (new): scans tinycc source files, auto-maps unit suites to
  their target TUs, reads `source_coverage_map.json`, and regenerates `SOURCE_COVERAGE.md`.
- **`tests/unit/source_coverage_map.json`** (new): editable ledger mapping each source file to a
  coverage kind (`unit`, `golden_ir`, `codegen_asm`, `ir_test`, `smoke`, `runtime_lib`, `tool`,
  `partial`, `none`) and the covering test artifact(s).
- **`tests/unit/SOURCE_COVERAGE.md`** (new/generated): human-readable report with summary stats and
  per-layer tables.
- **Goal:** every non-runtime source file is either covered by a unit/golden/asm test or explicitly
  annotated as `ir_test`/`smoke`/`partial`/`none`; `gen_source_coverage.py --check` fails the CI job
  when a new TU is missing from the map or when `SOURCE_COVERAGE.md` is stale.

## Risk-prioritized first batch
Order = (historical-miscompile evidence) × (Phase-4 merge centrality) × (ease of isolated test):
1. `opt_knownbits` 2. `opt_constfold` 3. `opt_constprop` 4. `opt_copyprop` 5. cmp-fold family
6. `licm` — all Tier 1, reachable via bare `tcc_ir_opt_<name>(ir)`. Then Tier 2 SSA analogues
(`ssa_opt_fold/sccp/cprop/gvn/load_cse/narrow/branch`) via golden-IR. Then Tier 3 codegen levers.

## Sequencing vs `plan_binary_size_reduction.md`
- **Phases 1–3 (low-risk size levers) proceed in parallel** — each cbz/narrow-branch/R9 win
  lands behind a Phase-D codegen test asserting the instruction-count drop, so the win is
  regression-locked.
- **Phase 4 (the optimizer merge) is gated**: it does not start until Phase B/C coverage of the
  passes it touches + the Phase-F equivalence harness exist. Phase 4's per-pass verification
  ("disable → smoke+torture → delete") gains a fast pre-check: the equivalence harness must be
  green before the slow QEMU gate runs.

## Verification
- `make ut` — all unit suites pass, `0 failed`; each Tier-1 suite **fails first** if its target
  fix is reverted (proves it bites).
- `make test-opt` — golden-IR + codegen pytest modules pass; `--update` regenerates goldens.
- `python tests/unit/check_pass_coverage.py` — prints the pass gap list; after Phase F exits non-zero
  on any uncovered registered pass.
- `python tests/unit/gen_source_coverage.py` — regenerates `SOURCE_COVERAGE.md`; after Phase G exits
  non-zero on any tracked source file missing from `source_coverage_map.json`.
- Determinism spot-check: run a golden case twice, confirm byte-identical `=== AFTER <pass> ===`.
- No regression of the existing slow path: full `tests/smoke/tcc_suite_test.py` /
  `tests/ir_tests/test_qemu.py` still pass.

## Files
- **New:** `tests/unit/arm/armv8m/ir_build.{h,c}`,
  `tests/unit/arm/armv8m/test_opt_{knownbits,constfold,constprop,copyprop,cmpfold,licm}.c`;
  `tests/ir_tests/test_golden_ir.py`, `tests/ir_tests/golden/`; `tests/ir_tests/test_codegen_asm.py`;
  `tests/unit/check_pass_coverage.py`, `tests/unit/PASS_COVERAGE.md`;
  `tests/unit/gen_source_coverage.py`, `tests/unit/source_coverage_map.json`,
  `tests/unit/SOURCE_COVERAGE.md`.
- **Modify:** `tests/unit/ut.h`, `tests/unit/arm/armv8m/Makefile`, `tests/unit/arm/armv8m/test_main.c`,
  `tests/unit/arm/armv8m/{stubs.c,tcc_state_stub.c}` (as link gaps surface), `Makefile`.
- **Reuse (read, don't reinvent):** `tccir_operand.h` (`irop_make_*`), `tccir.h`
  (`tcc_ir_op_get/set_*`), `ir/dump.c` (`tcc_ir_show`), `ir/opt_pipeline.c` (registered-pass
  names), `ir/opt_engine.c` (`tcc_ir_opt_ctx_*`), `tests/ir_tests/qemu_run.py` (cross-compile
  path), `tests/unit/README.md` (suite-authoring + stub priority).
