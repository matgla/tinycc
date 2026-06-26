# Plan: Shrink on-device `tcc` from 2.17 MiB → ~1 MB

## Context

The self-hosted on-device compiler `rootfs/usr/bin/tcc` has grown to **2.17 MiB of
`.text`** (2,270,264 B; `.rodata` ~99 KB). It no longer fits the size budget; the
goal is ~1 MB.

This plan is about the **size of the compiler binary itself**. It is complementary
to `plan_closing_gcc_gap.md` (which targets per-function codegen quality via
whole-program inlining / const-fold) — improvements there also shrink this binary,
since `tcc` is built by compiling its own source with `tcc -O2`.

### Measured root causes

Reproducible by compiling each native TU two ways and comparing `.text`
(`arm-none-eabi-size`): the cross `armv8m-tcc` is byte-identical codegen to the
on-device binary, vs `arm-none-eabi-gcc -O2 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard`.

**1. Codegen quality gap — tcc emits 1.48× more code than gcc-O2, 2.29× more than
gcc-Os** (e.g. `tccgen.c`: tcc 326 KB / gcc-O2 198 KB / gcc-Os 142 KB). Dominated by
mechanical backend issues:

| Issue | Count | Cost | Note |
|---|---|---|---|
| R9 GOT-base save/restore around every call | 39,858 `str` + 39,827 `ldr` | **~311 KB (13.7% of `.text`)** | R9 is function-invariant; pre-call stores are redundant |
| `cmp #0;b{eq,ne}` not fused to `cbz/cbnz` | 21,833 sites | ~42 KB | fusion code exists but is **disabled** (forward-range soundness) |
| Pessimistic wide `b{cond}.w` | 58,611 sites | tens of KB | forward branches never narrowed |

**2. Code volume — the optimizer is ~39% of the binary.** Two parallel frameworks
with overlapping work: legacy linear-IR passes (`ir/opt_*.c`, 37 files, **752 KB**,
a 108-pass pipeline) + SSA passes (`ir/opt/ssa_opt_*.c`, 15 files, **131 KB**, run at
regalloc time).

### Category breakdown of `.text` (tcc vs gcc-O2, per-TU)

| Category | tcc | gcc | ratio |
|---|---:|---:|---:|
| opt — legacy linear-IR (37 files) | 752 KB | 576 KB | 1.34 |
| arm backend (`arm-thumb-gen` + `arch/`) | 327 KB | 195 KB | 1.68 |
| `tccgen` front-end | 326 KB | 198 KB | 1.64 |
| codegen + regalloc | 138 KB | ~95 KB | ~1.5 |
| opt — SSA (`ir/opt/`, 15 files) | 131 KB | 93 KB | 1.44 |
| preprocessor / elf+link / asm / debug-gen | ~190 KB | ~125 KB | ~1.5 |

### Decision & realistic math

Pursue the **full path to 1 MB**, including the legacy→SSA optimizer
consolidation (high-risk, in a fork with a history of subtle opt miscompiles).
Phases 1–3 ≈ 400–450 KB at low risk (→ ~1.6 MB); the `-Os` self-host build is
potentially large; Phase 4's deletable slice ≈ 200–300 KB (the `opt_dead_*` /
`opt_gens_*` families are **lowering/machine-prep that must stay**). Reaching 1 MB
requires all of them landing.

Validation = QEMU smoke + gcc-torture + size measurement, **gated in CI with a
Grafana dashboard** (user-hosted server) tracking size + compile-perf per commit.

**Correctness coverage is a hard prerequisite, not just validation.** Smoke +
torture catch *many* miscompiles but corner cases slip through (the fork's history
of pass-local defects). Before the high-risk optimizer work, build the host-side
per-pass + codegen test layer described in **`plan_optimizer_test_coverage.md`**
(isolated `ut.h` unit tests for legacy passes, golden-IR snapshots for SSA passes,
objdump pattern/count tests for codegen). It is the **gate for Phase 4** and
regression-locks the Phase 1–3 codegen wins.

---

## Phase 0 — Metrics harness + CI size-gate + Grafana (foundation, do first)

**Goal:** make every later change measurable and regression-gated before touching
codegen.

**Reuse, don't rebuild:** `scripts/disasm_common.py` already compiles TCC+GCC,
disassembles with a best-known-result cache (`.disasm_cache.json` /
`.pending.json`), counts instructions, and compares per-function TCC-vs-GCC size.
`scripts/compare_disasm.py` / `scripts/regression_disasm.py` drive it.

**Build:**
- New `scripts/size_metrics.py` on top of `disasm_common`: emit a JSON record per
  build — total `.text`/`.rodata` of `bin/armv8m-tcc.elf` (`arm-none-eabi-size`),
  per-category `.text` (the category map above), per-TU tcc-vs-gcc ratio, the
  R9-spill / cbz-candidate / wide-branch instruction counts (objdump greps), and a
  representative compile-time sample (reuse the on-target per-pass timing where
  available). Key by git SHA + timestamp.
- Time-series + dashboard on the user's server: stand up **InfluxDB** (or
  **Prometheus + Pushgateway**) + **Grafana**; `size_metrics.py --push` writes the
  record. Grafana board: total `.text` over time (target line at 1.0 MB),
  per-category stacked area, tcc/gcc ratio, compile time.
- CI: add a `size_metrics` job to `.github/workflows/yasos_smoke.yml` (after
  `qemu_gate`, parallel to `build_hw`) that builds the cross + self-host tcc, runs
  `size_metrics.py --push`, and **fails the PR if total `.text` regresses beyond a
  small threshold** vs the `main` baseline.

**Verify:** dashboard shows the current 2.17 MiB baseline; a no-op rebuild
reproduces it; an intentional +bloat test trips the gate.

---

## Phase 1 — R9 GOT-base save/restore  (~150 KB → up to ~300 KB)

All logic is in `tcc_gen_machine_func_call_mop()` — `arm-thumb-gen.c:11711-11974`.
R9 is added to the per-call save mask at `:11760-11761`, stored at `:11776-11791`
(to `[SP + ir->call_outgoing_size + slot]`), reloaded at `:11932-11963` (with the
`allow_r9_write` gate). R9 is **not** materialized in the prologue today (the
runtime linker sets it — `arm-link.c:144-223`). Prologue builder:
`tcc_gen_machine_prolog()` `:9396-9700`. Note `caller_saved_registers` (`:210`, set
`:2422-2428`) is **dead** (never read) — clean up.

**Phase 1a (low risk, ~150 KB):** store R9 once to a fixed reserved frame slot in
the prologue; remove R9 from `arg_regs_save_mask` (drop all pre-call stores);
repoint the post-call reload at that fixed slot. Requires reserving one word in the
frame layout (`ir/stack.c`). Net: 1 store + N reloads instead of N + N.

**Phase 1b (medium risk, +~120 KB):** hold the GOT base in a callee-saved register
(e.g. r10 — AAPCS-compliant callees preserve it) reserved out of
`registers_map_for_allocator` (`:2417-2418`). Replace post-call `ldr.w r9,[sp]`
(4 B) with `mov r9, r10` (2 B), eliminating the slot and halving reload cost.
Further: skip the restore entirely after **intra-module** (static, non-PLT) calls,
where R9 is provably unchanged.

**Verify:** the `allow_r9_write` guard (`:2918-2923`) must stay green (no stray R9
writes); QEMU smoke + torture at -O0/-O1/-O2; `size_metrics.py` shows the R9-spill
instruction count drop toward 0.

---

## Phase 2 — Branch peepholes  (~80 KB)

Two-pass dry-run/real-run codegen in `ir/codegen.c:2344-4385`; relaxation infra
`branch_opt_analyze()` at `arm-thumb-gen.c:1019-1251`, called post-dry-run at
`ir/codegen.c:4212`. Conditional branch emit:
`tcc_gen_machine_conditional_jump_mop()` `:12029-12051` (backward already narrowed
via `can_narrow_backward_branch()` `:11981-12004`; forward always wide). CBZ builder
`th_cbz` exists (`arch/arm/thumb/thop_branch.c:146-217`); fusion peephole present but
**disabled** at `ir/codegen.c:2351-2371, 2590-2631, 2806-2850`; patch/abort path
`:3224-3285`. See `codegen_dry_run_opt.md` for the two-pass design.

**Phase 2a (~38 KB):** extend `branch_opt_analyze()` to narrow **forward**
conditional branches too — the dry-run layout map already gives final offsets, so
feed forward targets through the same relaxation decision.

**Phase 2b (~42 KB):** re-enable CBZ/CBNZ fusion by making relaxation
**re-materialize out-of-range CBZ as `cmp #0;b{eq,ne}`** during the fixpoint,
removing the commitment hazard the disabled code documents.

**Verify:** the branch-relaxation fixpoint must converge (assert no size
oscillation); smoke + torture (branch-heavy tests); metrics show wide-branch +
cbz-candidate counts drop.

---

## Phase 3 — Build / feature levers  (~100–200 KB, low effort)

- **Self-host at `-Os`:** the on-device tcc is built at `-O2` (`build_rootfs.sh`
  `NATIVE_TCC_DEBUG_OPT=-O2`, ~line 296). An `-Os` IR pipeline already exists
  (`ir/opt_pipeline.c:539-557` skips the fusion group). Set
  `NATIVE_TCC_OPT_OVERRIDE=-Os`, rebuild, measure — likely the single biggest cheap
  win (gcc -Os was 1.4× smaller than gcc-O2 here). Tune the `-Os` pipeline to also
  drop unroll/reroll.
- **Drop `-DCONFIG_TCC_DEBUG`** from the production self-host build (removes
  `-dump-ir` machinery) once on-device IR dumping is no longer needed.
- **Gate on-device debug-info emission** (`tccdbg`/`tccdebug`, ~30 KB `.text`) and
  the **inline assembler** (`tccasm`, ~20 KB, 2.07× ratio) behind a config if
  unused on device.

**Verify:** the `-Os` self-host must still pass full smoke + torture (codegen-mode
change — watch for latent -Os miscompiles); confirm debug/asm features are truly
unused before gating.

---

## Phase 4 — Optimizer consolidation: legacy → SSA  (~200–300 KB, high risk, the 1 MB-maker)

**Prerequisite (gate):** do not start until the per-pass + merge-equivalence coverage
from `plan_optimizer_test_coverage.md` exists for the passes this phase touches. Each
"disable → delete" step below then runs the fast legacy↔SSA equivalence diff *before*
the slow QEMU smoke/torture gate.

Legacy pipeline: `ir/opt_pipeline.c` (groups at `:522-566`, O-level gate
`:539-557`), engine `ir/opt_engine.{h,c}` (static `PASS()` arrays — remove a pass by
deleting its array entry or gating its `flag_offset`). SSA run:
`ir/regalloc.c:3991-4030` → driver `ir/opt/ssa_opt.c:648-703`. Background:
`plan_ssa.md`, `plan_ssa_regalloc.md`, `plan_opt_modularization.md`, `plan_opt_split.md`.

**Pass classification (from code exploration):**
- **Delete once SSA confirmed equivalent:** `opt_constprop.c` (→ `ssa_opt_sccp`),
  `opt_copyprop.c` (→ `ssa_opt_cprop` + `ssa_opt_dce`), `opt_branch.c` basic folding
  (→ `ssa_opt_sccp` + `ssa_opt_branch`), `opt_loop_dead.c` (→ `ssa_opt_dead_loop`).
- **Needs SSA extended first:** `opt_knownbits.c` (SSA fold lacks known-bits
  tracking — and it is load-bearing for a known miscompile fix; port carefully),
  `opt_loop.c` / `opt_reroll.c` IV strength-reduction (SSA strength is
  single-instruction only).
- **MUST KEEP (lowering / machine-prep, not optimization):** all `opt_dead_*.c`
  (stack-slot / lval alias analysis — SSA only renames promoted vars, not
  address-taken locals / VLAs / param slots) and all `opt_gens_*.c` (ARM
  instruction-selection fusion that codegen depends on).

**Approach (incremental, one pass at a time):**
1. Add a per-pass-family gate flag + a `-disable-legacy-opt` CLI switch so the
   SSA-only pipeline can be A/B tested.
2. For each "delete" candidate: disable it, run full smoke + torture + size metrics;
   confirm zero correctness regressions and a size drop; then delete the source and
   its pipeline entry.
3. Extend SSA (known-bits into `ssa_opt_fold`/`sccp`; loop-IV into
   `ssa_opt_strength`) to unlock the "needs-extension" deletions.
4. Leave the lowering / machine-prep families intact.

**Verify (mandatory per pass removed):** full QEMU smoke **and** gcc-torture at
-O0/-O1/-O2, plus the A/B self-host decisive test (real vs self-host) from
`selfhost_miscompile_debugging.md`; `disasm_common` regression check that no
benchmark function got *worse*; metrics push so the dashboard shows the cumulative
drop.

---

## Overall verification

- Per change: `size_metrics.py` for immediate `.text` delta + the CI size-gate
  (Phase 0).
- Per phase boundary: `make cross && ./build_rootfs.sh` rebuild, then
  `./scripts/run_qemu_smoke.sh` (smoke + torture, O0/O1/O2) and hardware smoke on the
  RP2350 runner.
- Progress tracked live on the Grafana board against the 1.0 MB target line.

## Sequencing

Phase 0 first (foundation). Then 1 → 2 → 3 (independent, low-risk, ~400–600 KB
combined) to reach ~1.6 MB. Then Phase 4 incrementally to close to ~1.0 MB. Phase 1b
and the `-Os` build are the highest size-per-effort items after the gate is in place.
