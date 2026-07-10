# Plan: Replace `loop_const_sim` with `ssa:loop_const_sim`

**Status:** proposed pass plan · **Created:** 2026-07-06

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents: [`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md)
(v1 placement, shared-engine retention, ordering-hazard gating) and
[`plan_legacy_loop_dead_first_iter_ssa.md`](plan_legacy_loop_dead_first_iter_ssa.md)
(Step 0 coverage experiment, analysis-move-on-retirement, cleanup-parity gate).

## Intent

Replace the legacy pre-SSA loop constant simulation pass
(`tcc_ir_opt_loop_const_sim`) with a CFG/dominator-driven pass that preserves
its current behavior:

- collapse loops whose body is **register-only** (no memory ops), built from
  simulator-safe integer ops plus recognised `__aeabi_*` soft-float helper
  calls and internal within-loop branches, by executing the body on the host
  and replacing the loop with residual final-value ASSIGN/STOREs;
- two acceptance modes, both preserved:
  - **IV-trip mode**: a primary induction variable with a computable trip
    count in `(0, LCS_MAX_TRIP_COUNT=16]`
    (`find_induction_vars_ex` + `find_loop_exit_condition` +
    `compute_trip_count`);
  - **generic bounded mode**: no usable IV; single exit target, stack-local
    state only, back-edge count bounded by `LCS_MAX_TRIP_COUNT`, and a
    modified-memory-used-after-loop bail;
- residual fidelity: 32-bit wrap via `lcs_truncate`, narrow-VAR
  `btype`/`is_unsigned` preservation (seed-4791 class), suppression of
  residual stores that rewrite an unchanged initial value;
- all current conservative bails: unknown calls, address-taken or complex
  VARs, PARAM destinations, `is_llocal`/`is_sym` operands, div-by-zero,
  trip/step/slot-table overflow, unsupported ops.

Behavior intentionally **not** preserved / explicit non-goals for v1:

- **No resurrection of body-memory simulation.** The current driver already
  declines any loop whose body contains
  LOAD/STORE/LOAD_INDEXED/STORE_INDEXED/POSTINC/BLOCK_COPY or an `is_lval`
  source operand ("LCS is only sound for register-only arithmetic" — the
  driver's `has_memory` scan, added after the tests-241/245/275 fuzz
  cluster). v1 keeps exactly this narrowing. The `LcsMemSlot` machinery stays
  only for what still uses it: the **pre-loop** initial-state scan and
  residual bookkeeping.
- No widening of `LCS_MAX_TRIP_COUNT`, step bounds, or the supported-op set.
- No SSA-native (phi-aware) simulation inside the `tcc_ir_ssa_opt_run` fixed
  point, and no merging with `ssa:dead_loop`. Both are possible v2s once a
  one-call CFG/SSA reconstruction entry point exists; explicitly deferred,
  same as in the rotation plan.
- No replacement of the shared `opt_loop_utils` IV helpers
  (`find_induction_vars_ex`, `find_loop_exit_condition`,
  `compute_trip_count`); they are treated as shared analysis, not
  legacy-owned.

## Current Legacy Shape

- Call site: single site in `tccgen.c` "Phase 4e" (`tccgen.c:30096` at time
  of writing, comment at 30091), **after** diamond store forwarding (4d½) and
  **before** loop unrolling (Phase 5a) — the stated ordering intent is "runs
  before unrolling so unrolling sees fewer candidates to expand". The call
  site iterates the pass up to **4 rounds**; after each fruitful round it
  runs a cleanup cascade: `compact_nops` → `dce` → `const_prop` →
  `branch_folding` → `compact_nops`. Rationale in the comment: folding one
  loop can expose the next loop as constant-foldable.
- Gate: `tcc_state->opt_loop_unroll` — set at `-O2` only (`libtcc.c`),
  toggled by `-floop-unroll`/`-fno-loop-unroll`. There is **no dedicated
  flag**; `-fno-loop-unroll` disables LCS and the unroller together, and
  `scripts/bisect_opt.py` lists `loop-unroll` among its preferred knobs, so
  LCS bugs currently attribute to that shared knob.
- Observability: `dump_ir_after_pass(..., "ZZ_loop_const_sim")` under
  `CONFIG_TCC_DEBUG` only, emitted once **after** the whole 4-round loop; not
  registered with `tcc_ir_opt_pass_disabled`, so `TCC_DISABLE_PASS` cannot
  isolate it. The replacement fixes both for free.
- Source: `ir/opt_loop_const_sim.c` (2305 lines) + `ir/opt_loop_const_sim.h`.
  Entry points: `tcc_ir_opt_loop_const_sim(ir)` and an `_ex(IROptCtx*)`
  wrapper that nothing in the pipeline tables references (delete at
  retirement). Declared also in `ir/opt.h:721-722`. Built in `Makefile
  IR_FILES`, linked into the unit-test binary
  (`tests/unit/arm/armv8m/Makefile:154`), and listed in the hardcoded
  `SELFHOST_COMPILE_SOURCES` (`tests/selfhost/test_selfhost_compile.py:72`).
- Driver structure (`tcc_ir_opt_loop_const_sim`, line 2160):
  - candidates from legacy `tcc_ir_detect_loops` flat ranges;
  - **overlapping-loop merge** (a C for-loop often yields two detected loops);
  - outermost only (`depth > 1` skipped), range cap 256 instructions;
  - function-wide **external-entry scan** (seed 589: a switch's
    case-before-dispatch layout satisfies the detector's backward-jump test
    without being a loop);
  - the `has_memory` decline described under Intent;
  - `lcs_try_fold` per surviving loop.
- `lcs_try_fold` structure (line 1750):
  - IV selection and trip count, else generic mode
    (`lcs_find_single_exit_target` + `lcs_generic_loop_is_stack_local`);
  - **rotated-range extension**: when `exit_target > end_idx + 1` the
    effective range grows to `[start_idx, exit_target-1]` (cap 512), with a
    re-scan that no outside jump lands in the absorbed tail (seed 2426: a
    guard's else arm sat in that tail and was NOPed);
  - branch containment check over the effective range;
  - `lcs_scan_body` legality walk (op whitelist `lcs_op_supported`:
    NOP/ASSIGN/LEA/LOAD-non-lval/ADD/SUB/MUL/AND/OR/XOR/SHL/SHR/SAR/ROR/
    DIV/UDIV/IMOD/UMOD/CMP/TEST_ZERO/JUMP/JUMPIF/FUNCPARAM*/FUNCCALL*/STORE;
    consults `tcc_ir_get_live_interval` for `addrtaken`/`is_complex` VARs);
  - pre-loop scan `lcs_init_var_state` seeding initial VAR values **and** a
    stack-memory map from pre-loop direct stores, with subword overlap
    clobbering (`lcs_mem_clobber_overlaps`, bitfield seed 11840) and
    modeling of STORE_INDEXED (test 241) and `Addr[StackLoc]+imm` (test 245);
  - host simulation `lcs_exec` bounded by
    `trip_count × body_size + 32` (≤ `LCS_MAX_ITER_STEPS=512`), soft-float
    helper evaluation (`lcs_classify_softcall`/`lcs_eval_softcall`,
    `__aeabi_{d,f}{add,sub,mul,div}`, conversions, `c{d,f}cmp{eq,le}`
    flag-setters with NaN semantics);
  - generic-mode `lcs_any_mem_used_after` bail;
  - residual **pre-flight fit check**, then in-place mutation: NOP the
    effective range and write residual ASSIGN/STOREs into the freed slots —
    **no instruction insertion, indices never shift**.
- Limits: trip ≤16, 512 steps/iteration, 256 VAR / 256 TEMP slots, 64 memory
  slots, 32 call ids × 4 params.
- Existing unit tests: 18 tests in
  `tests/unit/arm/armv8m/test_opt_loop_const_sim.c` (isolated harness,
  links the real module): accumulator fold, is_unsigned residual
  preserve/zero, int32 wrap, and a decline battery (store-in-body ×3,
  LOAD_INDEXED, runtime param, unknown call, trip>max both paths,
  div-by-zero, addr-taken VAR, internal branch to third target, no-loop,
  memory-loop idempotent no-op, LEA indirect store, zero-trip store loop).
  **`tests/unit/PASS_COVERAGE.md` has no row for this suite — add one during
  the migration.**
- Existing IR regressions (all in `TEST_FILES`, `tests/ir_tests/test_qemu.py`):
  `181_loop_const_sim_extern_store.c`,
  `220_fuzz_const_sim_branch_redef_liveness.c`,
  `223_fuzz_loop_const_sim_fp_compare.c`,
  `231_fuzz_loop_const_sim_bf_rmw_addrof_alias.c`,
  `238_fuzz_loop_const_sim_unsigned_char_residual.c`,
  `241_fuzz_loop_const_sim_indexed_store.c`,
  `245_fuzz_loop_const_sim_addr_plus_imm.c`,
  `249_fuzz_loop_const_sim_else_arm_absorbed.c`,
  `275_fuzz_loop_const_sim_subword_overlap.c` (plus
  `258_derived_iv_strength_reduction.c` adjacency).
- Known fuzz history tied to this pass (richest of any legacy loop pass —
  the motivation for the migration): seed 589 (false loop from the
  detector's dominance-free backward-jump test), longlong 2426 (rotated-range
  extension absorbed an else arm), agg_deep 47 (pre-loop scan ignored
  STORE_INDEXED), combo_num 872 (pre-loop scan missed `Addr[StackLoc]+imm`),
  seed 4791 (residual dropped `is_unsigned`), bitfield 11840/11743/15654
  (subword overlap), plus the test-220/231 branch-redef and bitfield-RMW
  aliasing cases. The cluster pattern is exactly the parent tracker's
  designated high-risk area: **flat-range candidates + pre-loop memory
  modeling**. CFG-fact candidates remove the first class structurally; the
  `has_memory` narrowing already removed most of the second.

## Step 0: Coverage-Gap Experiment (decision gate)

Unlike `first_iter_exit`, three mechanisms plausibly overlap LCS's narrowed
(register-only) domain:

1. **Legacy `loop_unroll` (Phase 5a, same gate)** — a constant-trip pure loop
   LCS folds is usually also fully unrollable, and the post-unroll cascade
   (`const_prop`/`dce`/…, `tccgen.c:30135+`) folds the expanded arithmetic to
   the same constants. LCS runs first today only as a cheaper path.
2. **`ssa:dead_loop`** (`-O2`, in the SSA fixed point) — pure loops whose
   body value is loop-invariant constant, incl. a guarded (SELECT) variant.
   It does **not** simulate loop-carried arithmetic (accumulators), so
   overlap is partial by design.
3. **`ssa:sccp` + `ssa:branch`** — may fold what the residuals would have
   produced anyway once the loop is straight-line.

Experiment (local, uncommitted): compile the C distillations of the 18 unit
shapes and the 9 LCS IR regressions at `-O2` (the pass is O2-only) with the
Phase 4e block compiled out, and compare against baseline:

- runtime correctness via `tests/ir_tests/run.py`;
- final disassembly: does the loop collapse at all, and to what (LCS-style
  residual constants vs. unrolled straight-line vs. surviving loop);
- attribute the collapsing pass via `-dump-ir-passes` diffs;
- code size over the IR-test corpus + torture suite (the `metrics/` tooling),
  since "unroll expands what LCS pre-collapsed" is a size hazard even when
  correctness holds;
- specifically probe the shapes unroll cannot take: soft-float helper bodies
  (`223`), internal-branch bodies (`220`), generic no-IV bounded loops, and
  a two-loop cascade where loop B's inputs are loop A's residuals (`181`).

Outcomes:

- **(a) unroll + SSA cover every shape with acceptable code size** → skip the
  new pass; the migration reduces to regression tests + code-size/cleanup
  parity checks + gated call-site removal.
- **(b) gaps exist** (expected: soft-float bodies, trip counts above the
  unroller's willingness, generic-mode loops, cascade shapes) → implement
  `ssa:loop_const_sim` per the design below.

Record the shape-by-shape matrix here before implementation starts.

### Step 0 Results

Outcome **(b) implement**, as anticipated. Confirmed via the temporary
regalloc-side experiment (running the legacy unroll mutator at the SSA point,
since reverted): register-only bounded loops — soft-float helper bodies,
internal-branch bodies, generic no-IV loops — reach regalloc unfolded and are
not covered by unroll + `ssa:dead_loop` + `ssa:sccp` alone, so a dedicated
`ssa:loop_const_sim` is warranted. The full shape-by-shape matrix was not
transcribed here; the acceptance corpus (the 9 IR regressions + the 18 unit
shapes, now mirrored by 14 `ssa_` unit tests) plus the new `346` regression
pin the behavior directly.

## SSA Replacement Design

### Placement: standalone flat-IR pass before SSA construction (v1)

Same placement and rationale as the two siblings: the SSA fixed point cannot
rebuild CFG/dominators/phis after a structural rewrite, and everything this
pass does (flat-range simulation, in-place NOP + residual emission with no
index shifts) works on flat IR. v1 runs in `ir/regalloc.c` as the third pass
of the pre-SSA trio:

```
ssa:loop_rotate → ssa:first_iter_exit → ssa:loop_const_sim → cfg/ssa build
```

Ordering notes:

- After `ssa:first_iter_exit` mirrors the legacy phase order (4c.5 before 4e)
  and is also cheaper: provably-never-running loops are gone before we spend
  simulation budget on them.
- After `ssa:loop_rotate` means the pass sees **bottom-tested** loops again.
  Note the current interim state is the opposite: since legacy rotation was
  retired, the legacy LCS in `tccgen.c` sees only un-rotated for-loops plus
  source-level do-while shapes. The engine handles both (the rotated-range
  logic exists precisely for bottom-tested layouts), but unit tests must
  cover both orientations (see tests below).
- The big ordering change vs. legacy: the pass now runs **after** legacy
  `loop_unroll`/LICM/IV-SR instead of before them. See "Interaction with
  other passes" — this is the main retirement hazard.

### Naming, files, observability

- Entry point: `int ssa_opt_loop_const_sim(struct TCCIRState *ir)`, declared
  in `ir/opt/ssa_opt.h`. The **driver** (CFG candidate detection + legality
  + iteration) lives in `ir/opt/ssa_opt_loop.c` alongside its two siblings.
- The **simulator engine** (~2k lines: `LcsState`, `lcs_exec`, pre-loop scan,
  residual emission) is not copy-pasted and does not move in v1: refactor
  `ir/opt_loop_const_sim.c` to expose one shared entry point consumed by both
  drivers during coexistence, e.g.
  `int lcs_fold_region(TCCIRState *ir, int eff_start, int eff_end, int header_idx, int preheader_idx)`
  (final shape per implementer — it must subsume today's `lcs_try_fold`
  including IV selection, so the legacy driver shrinks to detection + the
  call). This mirrors rotation's retained `try_rotate_loop`. At retirement
  the file is **kept as the shared engine** (see retirement step for the
  rename decision).
- Invoke from `ir/regalloc.c` guarded by
  `tcc_ir_opt_pass_disabled("ssa:loop_const_sim")` and followed by
  `tcc_ir_dump_after_pass(ir, "ssa:loop_const_sim")` — observable via
  `-dump-ir-passes=ssa:loop_const_sim`, isolable via
  `TCC_DISABLE_PASS=ssa:loop_const_sim`. Both are new capabilities; today's
  only isolation knob (`-fno-loop-unroll`) also kills the unroller.
- Gate: `tcc_state->opt_loop_unroll`, matching the legacy gate exactly
  (`-O2` default; manual `-floop-unroll` keeps working at lower levels;
  `-fno-loop-unroll` keeps its bisection meaning of "no unroll and no LCS").

### Comment policy (applies to every code change in this migration)

- New code (SSA driver, engine refactor, unit tests) carries **no comment
  blocks**: at most a single-line comment, and only where the code cannot
  express the constraint itself.
- The legacy engine is comment-heavy (fuzz-seed narratives, multi-paragraph
  design essays). Any code **moved or touched** during the refactor sheds
  those comments: delete them, or compress to one line if a real constraint
  remains. Spotting an existing multi-line comment in touched code means
  removing it, not preserving it.
- Fuzz-seed history and design rationale live in this plan (and the tests
  that pin them), not in source comments; do not re-home deleted narratives
  elsewhere in the code.

### Candidate detection (the actual improvement)

Replace `tcc_ir_detect_loops` flat ranges + overlap-merge + function-wide
external-entry scan + rotated-range extension with dominance-verified facts,
following the `ssa_opt_first_iter_exit` pattern (`fie_*` helpers):

- Build a throwaway `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` per
  driver round; free it before mutating.
- Candidates are back-edges `latch → header` where the header dominates the
  latch; collect natural-loop membership by backward reachability
  (reuse/extend `fie_collect_members`).
- Process **outermost** loops only, matching the legacy `depth > 1` skip:
  merge candidates whose membership sets intersect (shared header or nesting)
  into their outermost enclosing candidate before evaluation. Inner-loop
  back-edges inside an accepted candidate are simply simulated as internal
  control flow, exactly as today, and remain bounded by the step caps.
- **Contiguity + single-entry as checked facts (new, stronger than legacy):**
  let `[eff_start, eff_end]` be the min/max instruction index over member
  blocks. Require (1) every non-NOP instruction in that span to belong to a
  member block, and (2) the only edges from outside into the span to target
  the header. This one structural check replaces the legacy overlap-merge,
  the seed-589 external-entry scan, **and** the seed-2426 absorbed-tail
  re-scan; the shapes those defended against are declined by construction.
  Non-contiguous loop layouts decline in v1 (legacy's branch-containment
  check effectively required contiguity anyway).
- Exit discipline: IV mode takes the exit target from
  `find_loop_exit_condition` and requires every other in-span branch to stay
  in-span or land exactly on it (as today). Generic mode requires all
  span-leaving branches to share a single target
  (`lcs_find_single_exit_target` semantics, expressed over member blocks).
- Size caps unchanged: decline spans over 256 instructions (the legacy
  pre-extension cap; the 512 extension cap loses its reason to exist once
  membership is exact — keep 256 as the single cap and note it).
- Keep the driver-level `has_memory` decline, evaluated over member
  instructions instead of `body_instrs`.

### Simulation and mutation (unchanged semantics)

- The engine is reused verbatim through the shared entry point: same op
  whitelist, soft-float table, trip/step/slot caps, pre-loop scan (including
  its memory map — it models **pre-loop** stores, which is unaffected by the
  body-memory decline), residual `btype`/`is_unsigned` rules, generic-mode
  `lcs_any_mem_used_after` bail, residual pre-flight, in-place NOP +
  residual emission. Indices never shift.
- Driver iteration: mirror the legacy call site's cascade **inside the pass**
  as up to 4 rounds of "detect → fold all candidates", stopping when a round
  changes nothing. Between rounds run **no cleanup passes**: the pre-loop
  scan reads actual IR, so round N's residual ASSIGN/STOREs are visible to
  round N+1 directly, and NOPs are skipped by the walk. Whether this
  cascades as well as the legacy `const_prop`+`branch_folding` interleave is
  a Step 0 question (probe the `181`/two-loop cascade shape); if a real gap
  shows, the fallback is calling the flat-IR `tcc_ir_opt_const_prop` +
  `tcc_ir_opt_branch_folding` between rounds (legal at this pre-SSA point),
  with the choice recorded here. Do not silently accept a cascade
  narrowing.
  - **Decision (implemented): no interleaved cleanup.** The driver folds every
    outermost candidate per CFG build (as the legacy driver folded every
    detected loop per pass): outermost natural loops have provably disjoint
    spans (contiguity means a candidate's span holds only its own members) and
    each fold touches only its own span plus its IV init in its own preheader
    gap, so folds do not interfere. Processing in ascending block (≈ flat)
    order lets a forward cascade resolve in one round: `test_lcs_ssa_cascade_
    two_loops` pins loop B reading loop A's residual — A folds (residual
    `acc=40` written into A's freed slots), then B's pre-loop scan reads that
    residual directly and folds to `acc=340`, both in round 1. The residuals
    are plain IR the scan sees; the stale exit-target `is_jump_target` is
    ignored by the scan's `real_pre_target` filter (no live JUMP targets it
    after the fold). `test_lcs_ssa_three_independent_loops_fold` pins that
    N disjoint loops collapse in a single call (no artificial cap). The bounded
    fixed point still covers backward cascades and newly exposed loops. The
    const_prop/branch_folding fallback was not needed.
- Verify at implementation time that `tcc_ir_get_live_interval`
  `addrtaken`/`is_complex` facts consulted by `lcs_scan_body` are still valid
  at the `ir/regalloc.c` call point (they are vreg metadata, and the sibling
  passes already run equivalent queries there — confirm, don't assume).
- The pass must be **idempotent** (its output contains no back-edge, so a
  second run finds no candidates) and **inert during coexistence** (with the
  legacy pass enabled, qualifying loops are already residualized by regalloc
  time; the driver finds no candidates and returns 0) — same contract as
  both siblings.

### Interaction with other passes

- **Legacy `loop_unroll` (the retirement hazard).** Today LCS runs first and
  starves the unroller of constant-trip candidates. Once the Phase 4e call
  site is removed, the unroller sees them all: correctness must be neutral
  (unroll + post-unroll cascade produce the same constants), but compile
  time and **code size** can regress where the cascade cannot fully fold the
  expansion, and shapes the unroller declines (soft-float calls, internal
  branches) now survive to regalloc time where `ssa:loop_const_sim` folds
  them — later, but with identical results. Gate the call-site removal on
  the fuzz diff **plus** a code-size comparison over the IR corpus and
  torture suite, exactly like the rotation plan's ordering-hazard step. If
  size regresses, the options are (in order): teach the unroller to decline
  LCS-foldable loops, keep the SSA pass but move the unroll gate, or accept
  and document the delta — silence is not acceptance.
- **`ssa:dead_loop`** (`-O2`, in the fixed point): complementary, not
  subsumed. It handles loop-invariant constant results with possibly
  **unknown** trip counts (guarded SELECT variant); LCS handles loop-carried
  arithmetic with **known-bounded** trips. After `ssa:loop_const_sim` fires,
  dead_loop simply sees fewer candidates. Keep both; harmless duplication on
  the overlap.
- **`ssa:first_iter_exit`**: disjoint by trip count — first-iter handles
  provably-zero-trip loops, LCS requires trip > 0 (IV mode) or simulates the
  first-iteration exit fine (generic mode). Running first-iter first is a
  cost optimization only.
- **`ssa:loop_rotate`**: produces bottom-tested candidates for this pass;
  no legality interaction beyond the orientation coverage already noted.
- **LICM / IV-SR / reroll (still legacy, still upstream)**: they continue to
  see post-LCS IR today and pre-LCS IR after retirement; their own
  migrations are tracked separately in the parent plan. Nothing here may
  assume their output shape beyond what `lcs_scan_body` already checks.

## Required Unit Tests

v1 runs on flat IR; tests live in the existing isolated harness. Extend
`tests/unit/arm/armv8m/test_opt_loop_const_sim.c` with `ssa_` variants
driven through `ssa_opt_loop_const_sim` (the legacy 18 stay untouched until
retirement, then their drivers are re-pointed or deleted with the legacy
entry point — decide at retirement, mirroring `test_opt_loop_dead.c`).

Minimum scenarios (assert change count **and** resulting IR: residual
values/btypes, NOPed span, untouched non-member code):

- port of every firing legacy shape: accumulator fold, is_unsigned
  preserve/zero pair, int32 wrap;
- port of every declining legacy shape: store-in-body ×3, LOAD_INDEXED,
  runtime param, unknown call, trip-over-max both paths, div-by-zero,
  addr-taken VAR, internal-branch-to-third-target, no-loop, zero-trip store
  loop, LEA indirect store, memory-loop idempotent no-op;
- **new:** bottom-tested (rotated) candidate folds — the orientation the
  legacy driver currently never sees but the SSA pass will;
- **new:** top-tested candidate folds (both orientations pinned);
- **new:** seed-2426 shape — guard's else arm laid out between back-edge and
  exit target — declines structurally (non-member code in span);
- **new:** seed-589 shape — switch dispatch backward jump — produces no
  dominance-verified candidate;
- **new:** jump from outside into a non-header member block declines
  (single-entry fact);
- **new:** nested loop — outer candidate simulates through the inner
  back-edge within step bounds; inner loop alone is not separately
  processed;
- **new:** two-loop cascade — loop B consumes loop A's residuals; folds
  within the 4-round driver without interleaved cleanup (or pins the chosen
  fallback);
- idempotency: second run on the pass's own output returns 0;
- inert coexistence: running the pass on IR the legacy pass already
  processed returns 0.

IR regression tests: the 9 existing LCS tests are the acceptance corpus and
must stay green throughout. Add one new test,
`tests/ir_tests/345_loop_const_sim_ssa.c` (next free number; register in
`TEST_FILES`), pinning a shape only the replacement covers post-migration:
a soft-float accumulator loop with a trip count the unroller declines,
feeding a second loop through its residual, plus a runtime-bounded control
loop that must **not** fold. Verified green at `-O0/-O1/-O2` before the
migration (legacy covers it) and after (SSA covers it).

Also add the missing `opt_loop_const_sim` row to
`tests/unit/PASS_COVERAGE.md` when the suite grows.

## Migration Steps

- [x] Run the Step 0 coverage-gap experiment; decided outcome (b) implement
  (see *Step 0 Results*).
- [x] Add IR regression `tests/ir_tests/346_loop_const_sim_ssa.c` (345 was
  taken by the ptr_iv sibling; green under legacy at `-O0/-O1/-O2/-Os`).
- [x] Refactor `ir/opt_loop_const_sim.c` to expose the shared engine entry
  point `lcs_fold_region(ir, start, end, header, preheader, allow_extension)`;
  legacy driver becomes detection + call with `allow_extension=1`; existing 18
  unit tests + 9 IR tests green. (SSA callers pass exact membership with
  `allow_extension=0`, so the rotated-range extension is off for them.)
- [x] Add the `ssa_` unit tests to `test_opt_loop_const_sim.c` (14 new,
  driven through `ssa_opt_loop_const_sim`; suite 32 tests, full UT binary
  green).
- [x] Implement `ssa_opt_loop_const_sim` (dominance-verified candidates +
  member union + contiguity + header-dominates-members single-entry +
  outermost-only + one-fold-per-CFG-build 4-round driver) in
  `ir/opt/ssa_opt_loop.c`; declared in `ir/opt/ssa_opt.h`.
- [x] Invoke from `ir/regalloc.c` after `ssa:ptr_iv_exit_subst`, gated
  `tcc_state->opt_loop_unroll`, wrapped in `tcc_ir_opt_pass_disabled` +
  `tcc_ir_dump_after_pass`.
- [x] Confirm observability (`-dump-ir-passes=ssa:loop_const_sim` emits the
  AFTER block) and disable knob (`TCC_DISABLE_PASS=ssa:loop_const_sim`).
- [x] Confirm idempotency (`test_lcs_ssa_idempotent`) and inert coexistence
  (`test_lcs_ssa_inert_after_legacy_fold`: legacy fold then SSA returns 0).
- [x] Run the full validation gate with both passes enabled (unit suite +
  full `make test` IR suite green). Maintainer fuzz sweeps per *Acceptance*
  remain.
- [x] Removed the legacy `tccgen.c` Phase 4e call site (including its 4-round
  compact_nops/dce/const_prop/branch_folding cleanup cascade); tombstone left
  in place mirroring Phase 4c.5. Full IR suite (`make test`) green post-removal.
  Maintainer fuzz sweeps + code-size comparison remain per *Acceptance*.
- [x] Deleted the legacy driver (`tcc_ir_opt_loop_const_sim`, the `_ex`
  wrapper, the `ir/opt.h` + `ir/opt_loop_const_sim.h` declarations). The engine
  file `ir/opt_loop_const_sim.c` is **retained in place as the shared engine**
  (no `Makefile`/`SELFHOST_COMPILE_SOURCES` list changes; it still compiles and
  the SSA driver links it). The 18 legacy unit tests were re-pointed to
  `ssa_opt_loop_const_sim` (all pass through the SSA driver — same engine, same
  fold/decline outcomes); the coexistence test was dropped. `allow_extension`
  stays on `lcs_fold_region` for the engine's own rotated-range handling but is
  now always called with 0 (SSA membership is exact). Updated
  `tests/unit/PASS_COVERAGE.md`.

## Retirement hazard (separate gate for removing the legacy call site)

- **Unroll sees LCS's candidates** — the code-size / compile-time gate
  described above; this is the LCS analogue of rotation's ordering hazard
  and the main reason call-site removal is its own step.
- **Cleanup-cascade parity** — the per-round `const_prop`/`branch_folding`
  at the call site also serviced *downstream* phases (post-LCS constants
  feeding Phase 5a+). After removal this folding happens partly in the
  remaining tccgen tail and partly at SSA time. Verify on the cascade
  shapes (181, 249, the new 345) that final disassembly at `-O2` contains
  no surviving loop, residual dead stores, or unfolded guard branches
  relative to baseline. Document and explicitly accept any parity gap, as
  the first-iter plan did for 20070824-1.
- **`-O1` question**: none — legacy is O2-only via `opt_loop_unroll`, and
  the replacement keeps the same gate. Confirm `-floop-unroll` at `-O0/-O1`
  still reaches the new pass (manual-flag parity).
- **Compile time**: the pass adds a CFG build + simulation budget per
  function at `-O2` in regalloc; bounds are unchanged from legacy
  (≤4 rounds, ≤512 steps/iter). Spot-check selfhost compile time.

## Acceptance

For landing `ssa:loop_const_sim` (legacy still enabled):

- [ ] New unit tests pass; existing 18 legacy unit tests pass unchanged.
- [ ] All 9 LCS IR regressions + new `345_loop_const_sim_ssa.c` pass.
- [ ] `make cross -j$(nproc)`
- [ ] `make test -j16`
- [ ] GCC torture suite (`pytest tests/gcctestsuite/ -v`).
- [ ] **Maintainer-run** (per the 2026-07-06 instruction, after
  implementation is complete): `tests/fuzz/sweep_all_chunks.py 0 1000
  --mode triage` → 0 divergent profiles, and `python3
  scripts/diff_olevels.py --seeds 0-5000 --require-qemu` → 0 divergences.
- [ ] **Maintainer-run**: same sweep with
  `TCC_DISABLE_PASS=ssa:loop_const_sim` (pass off must equal today's
  behavior), and a replacement-only run (legacy call site disabled locally)
  before proposing retirement.
- [ ] **Maintainer-run**: zero new divergences; fixes triggered from there.

For the separate retirement step: all of the above re-run after the
call-site removal, plus the code-size comparison and cleanup-parity
disassembly checks from "Retirement hazard", plus the maintainer's wider
fuzz sweep before the legacy driver is deleted.

## Assumptions

- `ssa:loop_rotate` and `ssa:first_iter_exit` keep their current
  `ir/regalloc.c` positions; this pass slots in directly after them and
  moves with them if they move.
- The legacy pass stays enabled and untouched (beyond the shared-engine
  refactor) until the retirement gate passes.
- `IRLoop`/`tcc_ir_detect_loops` is not used by the new driver; candidates
  come from the throwaway CFG. `find_induction_vars_ex` /
  `find_loop_exit_condition` / `compute_trip_count`
  (`ir/opt_loop_utils.c`) are shared analysis, not legacy-owned — they stay
  regardless of what else retires.
- The `has_memory` narrowing is considered permanent for this pass; any
  future body-memory simulation is a new plan with its own fuzz gate, not a
  revival of the deleted modeling.
