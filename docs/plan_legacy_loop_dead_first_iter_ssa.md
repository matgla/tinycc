# Plan: Replace `loop_dead_first_iter` with `ssa:first_iter_exit`

**Status:** proposed pass plan · **Created:** 2026-07-06

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedent: [`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md)
(v1 placement and coexistence rules are reused here).

## Intent

Replace the legacy pre-SSA first-iteration-exit peeling pass
(`tcc_ir_opt_loop_dead_first_iter`) with a CFG/dominator-driven pass that:

- proves a top-tested loop's header exit test (TEST_ZERO/CMP + JUMPIF to a
  target outside the loop) is statically **true on first entry**, using
  first-iteration constant values of TEMPs and VAR stack slots, including
  values reached through `&VAR` pointers (`p = &s; ... *p`);
- rewrites the exit JUMPIF into an unconditional JUMP to the exit target and
  removes the never-executed loop body;
- keeps the conservatism of the legacy analysis: any call, unknown store,
  non-straight-line entry path, or unresolvable operand declines the loop.

Behavior intentionally **not** preserved / explicit non-goals for v1:

- No generalization of the entry-path requirement. The legacy pass requires a
  fully straight-line path from function entry to the exit JUMPIF (it bails on
  *any* JUMP/JUMPIF during the walk). v1 keeps exactly this requirement;
  extending the walk across a single-pred/single-succ CFG chain is a possible
  v2, not part of the migration.
- No attempt to fold exit tests of **rotated** (bottom-tested) loops. After
  rotation the entry guard is straight-line code outside the loop; folding it
  belongs to constant/branch folding, not to this pass (see "Interaction with
  other passes").
- No extension of `ssa:sccp`'s memory lattice with points-to facts for
  deref-loads. That is the long-term home for this reasoning but is explicitly
  deferred: SCCP's stack-store forwarding and entry-block exemption are a
  documented fuzz-bug cluster (see `docs/debugging_fuzz_divergences.md`
  history), and growing that lattice for one loop pattern is disproportionate
  risk. Revisit only after the legacy pass is retired.

## Current Legacy Shape

- Call site: single site in `tccgen.c` ("Phase 4c.5", `tccgen.c:29986` at time
  of writing), immediately after legacy loop rotation (`ZZ_loop_rotation`) and
  before `loop_ptr_iv_exit_subst`, `loop_const_sim`, `loop_unroll`, LICM, and
  IV strength reduction. The header comment states the ordering intent: run
  pre-SSA and before unroll/LCS "so subsequent passes see the simplified IR".
- Gate: `tcc_state->opt_const_prop` only (set at `-O1+`, cleared by
  `-fno-const-prop`). There is **no dedicated flag** for this pass.
- Observability: **none**. The call site has no `dump_ir_after_pass` name and
  the pass is not registered with `tcc_ir_opt_pass_disabled`, so it is
  invisible to `-dump-ir-passes` and cannot be isolated by
  `TCC_DISABLE_PASS` — bisection currently has to lean on `-fno-const-prop`,
  which disables far more. The replacement fixes this for free.
- Source: `ir/opt_loop_dead.c` (self-contained; helpers from
  `ir/opt_loop_utils.h` for `loop_size_cmp`, `evaluate_compare_condition`).
  Structure:
  - `LdState` value walk (`ld_step`/`ld_resolve`): tracks `LD_CONST` for VARs
    (`var_state[256]`) and TEMPs (`tmp_state[512]`) plus `LD_LEA_VAR`
    (`&VAR`), with an address-taken bitmap. Calls / VLA / inline asm /
    setjmp-class ops and unknown stores invalidate all address-taken VARs;
    stores through a known `LEA(&V)` update `V` precisely. The walk starts at
    instruction 0 and **bails on any JUMP/JUMPIF/IJUMP/SWITCH/RETURN** before
    the exit branch — this is what makes "first-iteration values" sound.
    `LdState` is ~18 KB and heap-allocated (32 KB target-stack overflow
    otherwise; keep this in the replacement).
  - `ld_find_exit_branch`: finds TEST_ZERO/CMP + JUMPIF within 6 instructions
    of `loop->start_idx` whose target lies outside `[start_idx, end_idx]`.
  - `ld_eval_branch`: TEST_ZERO folds only EQ/NE (hardcoded tokens
    0x94/0x95); CMP folds via `evaluate_compare_condition`.
  - Mutation: rewrite the JUMPIF into JUMP (dest already holds the exit
    target), then **NOP every other instruction in the flat range
    `[start_idx, end_idx]`** — the header's pre-test defs are dead and leaving
    them creates stale `is_jump_target` state that pessimizes
    `stack_addr_nonnull_fold`. Finally `ld_nop_fallthrough_jumps` NOPs any
    JUMP targeting its own next live instruction, function-wide.
  - Loop candidates come from legacy `tcc_ir_detect_loops` flat ranges
    (smallest-first), requiring `preheader_idx >= 0`. Range-based body
    handling is the parent tracker's designated high-risk area.
- Call-site cleanup cascade (fires only when the pass eliminated a loop):
  `branch_folding` → `dce` → `compact_nops` → `const_prop` →
  `stack_addr_nonnull_fold` → `branch_folding` → `dce` → `compact_nops`, then
  up to 4 rounds of `dead_trailing_addrvar_store_elim` +
  `dead_alloca_vreg_elim` (+ dce/compact each). This cascade is what actually
  collapses the post-loop `if (!s) abort()` shape and the now-dead alloca in
  the torture tests; the replacement must reproduce its *effect*, not its
  mechanism.
- Existing unit tests: 15 tests in
  `tests/unit/arm/armv8m/test_opt_loop_dead.c` (integrated per
  `tests/unit/PASS_COVERAGE.md`): TEST_ZERO EQ/NE fire, EQ-not-taken decline,
  CMP LT fire / false-decline / unsigned-semantics decline, LEA+deref fire,
  store-through-pointer fire, call-invalidation decline, unknown-value
  decline, no-loop, JUMPIF-target-inside-loop decline, unknown-token decline,
  intervening-jump bail, idempotency.
- Existing IR regressions: **none dedicated**. The motivating case is GCC
  torture `execute/20070824-1.c` (`for (p = &s; *p; p = &(*p)->a);` with
  `s == 0`), covered only when the torture suite runs. Closing this gap is
  part of the migration (see tests below).
- Known fuzz regressions tied to this pass: none recorded in `docs/bugs.md`,
  the fuzz triage notes, or the divergence memory index. The pass has been
  quiet — which also means the fuzzer exercises it rarely; do not read the
  silence as strong evidence.

## Step 0: Coverage-Gap Experiment (decision gate)

Before writing any code, measure what the existing SSA pipeline already
covers. `ssa:sccp` is optimistic (edges start unexecutable), so a loop whose
header test folds true on the preheader-only phi values is proven dead by
construction — **for register values**. SCCP also forwards stack stores to
loads with alias guards. What it deliberately does not do is resolve a
deref-load through a pointer holding `&VAR` (`ssa_opt_sccp.c` states VAR-DEREF
is not a slot read), which is exactly the motivating pattern.

Experiment (local, uncommitted):

- Comment out the `tcc_ir_opt_loop_dead_first_iter` call-site block in
  `tccgen.c`, `make cross`.
- For each of the 15 unit-test shapes (recreated as small C inputs) plus
  `20070824-1.c`, compile at `-O1` and `-O2`; check via
  `tests/ir_tests/run.py` execution and `-dump-ir` whether the loop still
  collapses at SSA time, and whether the post-loop abort branch and dead
  alloca disappear from the final IR/disassembly.
- Run `python3 scripts/diff_olevels.py --seeds 0-1000 --require-qemu` with the
  call disabled to detect any fuzz-visible dependence.

Outcomes:

- **(a) SSA already collapses every shape including the deref pattern** →
  skip the new pass entirely; the migration reduces to regression tests +
  cleanup-parity check + gated call-site removal.
- **(b) Gaps exist** (expected: the `LD_LEA_VAR` deref shapes; possibly the
  plain VAR-const shapes too at `-O1`, since `ssa:dead_loop` is `-O2`-only
  and SCCP's memory forwarding has entry-block alias restrictions) → implement
  `ssa:first_iter_exit` per the design below, scoped to the shapes SSA does
  not cover, but matching legacy acceptance on all 15 unit shapes anyway
  (redundant coverage is cheap; behavioral parity is the gate).

Record the resulting shape-by-shape matrix in this document before
implementation starts.

### Step 0 Results (run 2026-07-06) — outcome (b)

Method: the 15 unit shapes distill to 11 C programs (one per behavior class;
structural declines like "no loop" / "exit target inside loop" have no C-level
equivalent worth compiling).  Each was compiled with the tree's `armv8m-tcc`
(ASan dev build) at `-O1`/`-O2` twice — legacy pass enabled vs. call site
compiled out — and the disassembly of the loop function inspected for a
surviving backward branch.

| shape (C distillation) | legacy O1/O2 | no-legacy O1 | no-legacy O2 |
|---|---|---|---|
| TEST_ZERO EQ: `x=0; while(x)` | eliminated | **loop survives** | **loop survives** |
| TEST_ZERO NE: `x=5; while(!x)` | eliminated | **loop survives** | **loop survives** |
| control (runs): `x=5; while(x--)` | kept (correct) | kept | kept |
| CMP false on entry: `i=10; while(i<3)` | eliminated | **loop survives** | **loop survives** |
| control (runs): `i=3; while(i<10)` | kept (correct) | kept | kept |
| unsigned: `unsigned x=-1u; while(x<1u)` | eliminated | **loop survives** | **loop survives** |
| LEA+deref: `s=0; p=&s; while(*p)` | eliminated | **loop survives** | **loop survives** |
| store-thru-ptr: `*p=0; while(*p)` | eliminated | **loop survives** | **loop survives** |
| call invalidation: `ext(); while(*p)` | kept (decline) | kept | kept |
| unknown: `while (g)` extern volatile | kept (decline) | kept | kept |
| `20070824-1.c` pointer chase | eliminated, abort gone | **loop + abort survive** | **loop + abort survive** |

Findings:

- **The SSA pipeline covers none of the eliminable shapes** — not even the
  plain VAR-const `while(x)` with `x==0` at `-O2`.  `ssa:sccp`/`ssa:branch`/
  `ssa:dead_loop` never fold the header test of these top-tested loops, so
  the coverage gap is the entire legacy pass, not just the deref shapes.
- `ssa:loop_rotate` **declines** every shape above (verified via
  `-dump-ir-passes=ssa:loop_rotate` on the no-legacy build: the loops are
  still top-tested `TEST_ZERO/CMP + JUMPIF` with intact VAR/TEMP/LEA operands
  at regalloc time).  The planned rotate → first-iter order is therefore
  compatible with the replacement seeing legacy-identical IR shapes.
- The regalloc-time IR for the deref shapes is walk-compatible: `V0=0;
  V1=&V0; T1=V1; TEST_ZERO T1***DEREF***; JUMPIF exit` — same VAR/TEMP/LEA
  encodings the `LdState` walk models.
- The `diff_olevels --seeds 0-1000` no-legacy sweep was **not** run:
  per maintainer instruction fuzz validation is run by the maintainer after
  the implementation lands (see Acceptance).

Decision: **outcome (b)** — implement `ssa:first_iter_exit` per the design
below, matching legacy acceptance on all shapes.

## SSA Replacement Design

### Placement: standalone flat-IR pass before SSA construction (v1)

Same placement and rationale as `ssa:loop_rotate` (see the rotation plan's
"Placement" section for the full argument): the SSA fixed-point loop cannot
rebuild CFG/dominators/phis after a structural rewrite, and everything this
pass needs (straight-line entry walk, flat JUMPIF rewrite, block NOPing) works
on flat IR. v1 therefore runs in `ir/regalloc.c` immediately **after**
`ssa:loop_rotate` and before `tcc_ir_cfg_build`/SSA construction.

Ordering notes:

- After `ssa:loop_rotate` mirrors the legacy phase order (rotation 4c →
  first-iter 4c.5). The two passes decline each other's shapes: rotation
  declines the deref-heavy loops this pass targets, and this pass only
  matches top-tested loops (a rotated loop's guard lives outside the loop, so
  `find_exit_branch` fails). Running first-iter-exit *before* rotation (never
  rotate a provably dead loop) is a possible later swap; do not change two
  variables at once in v1.
- An in-fixed-point variant (fold the JUMPIF via the `ssa:branch` machinery +
  `ssa_drop_phi_edge`, let `ssa:dce` clean up) is the natural v2 once a
  one-call CFG/SSA reconstruction entry point exists; deferred exactly as in
  the rotation plan.

### Naming, files, observability

- Entry point: `int ssa_opt_first_iter_exit(struct TCCIRState *ir)`, declared
  in `ir/opt/ssa_opt.h`, implemented in `ir/opt/ssa_opt_loop.c` alongside
  `ssa_opt_loop_rotate` (parent-tracker file-placement decision: loop-shape
  structural passes share `ssa_opt_loop.c`).
- Invoke from `ir/regalloc.c` guarded by
  `tcc_ir_opt_pass_disabled(ir, "ssa:first_iter_exit")` and followed by
  `tcc_ir_dump_after_pass(ir, "ssa:first_iter_exit")` — observable via
  `-dump-ir-passes=ssa:first_iter_exit`, isolable via
  `TCC_DISABLE_PASS=ssa:first_iter_exit`. Both are new capabilities relative
  to the legacy pass.
- Gate: `tcc_state->optimize >= 1 && tcc_state->opt_const_prop`, matching the
  legacy gate exactly so `-fno-const-prop` keeps its current meaning for
  bisection sweeps (`scripts/bisect_opt.py` knob attribution depends on it).
- Shared analysis code: refactor `ir/opt_loop_dead.c` to expose the value walk
  and branch evaluation behind non-static entry points (e.g.
  `ld_first_iter_prove(ir, test_idx, jumpif_idx)` — final naming per
  implementer) consumed by both the legacy driver and the SSA pass during
  coexistence. Do **not** copy-paste the ~300-line walk. When the legacy pass
  is deleted, the analysis moves wholesale into `ir/opt/ssa_opt_loop.c` (or a
  shared `ssa_opt_loop_utils` home if rotation's deferred cleanup lands
  first).

### Candidate detection (the actual improvement)

Replace legacy `tcc_ir_detect_loops` flat ranges with dominance-verified
facts, following the `ssa_opt_loop_rotate` pattern:

- Build a throwaway `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` CFG
  per fixed-point pass; free it before mutating.
- Candidates are back-edges `latch → header` where the header dominates the
  latch.
- **Explicit single-entry guard (new, stronger than legacy):** the header
  block's predecessors must be exactly {one entry-path predecessor, latch},
  and no block of the natural loop body other than the header may have a
  predecessor outside the loop. Legacy only implied this through
  `preheader_idx >= 0` plus the entry-walk bail; make it a checked CFG fact.
- Natural-loop membership (backward reachability from latch to header) is
  computed once per candidate and reused by the legality checks and the NOP
  step below.
- Process candidates smallest-first (inner loops first), mirroring the legacy
  `loop_size_cmp` ordering. Iterate to a small fixed point (an eliminated
  inner loop can make an outer loop's entry path straight-line); cap passes
  like `SSA_LOOP_ROTATE_MAX_PASSES` does.

### First-iteration evaluation (unchanged semantics)

- Locate the exit branch exactly as legacy does: TEST_ZERO/CMP + JUMPIF within
  a small lookahead of the header block's start whose target block is outside
  the loop membership set (replacing the flat `[start_idx, end_idx]` range
  test with the membership test).
- Run the shared `LdState` walk from instruction 0 to the JUMPIF with the
  legacy bail rules unchanged (any branch before the JUMPIF aborts). Keep the
  heap allocation of `LdState`.
- Fold TEST_ZERO (EQ/NE) and CMP (via `evaluate_compare_condition`) exactly as
  legacy; require the proven-taken result. Consider replacing the local
  `LD_TOK_EQ/NE` duplicates with the shared token definitions while moving the
  code, but do not change accepted token coverage in v1.

### Mutation strategy

- Rewrite the JUMPIF op to JUMP in place (dest operand already holds the exit
  target). Indices never shift; no instruction is inserted.
- NOP the loop's **member blocks** (from the natural-loop membership set),
  not the flat `[start_idx, end_idx]` range. This is the second real
  improvement: flat-range NOPing is unsound if non-loop code is interleaved
  inside the range, which is precisely the failure class (`body_instrs` /
  range mismatches) documented for other legacy loop passes. The header block
  is NOPed except the rewritten JUMP; the pre-test header defs die with it,
  preserving the legacy `is_jump_target`-hygiene rationale.
- Keep the `ld_nop_fallthrough_jumps` sweep (JUMP-to-next-live → NOP) after
  any elimination, as a shared helper.
- The pass must be **idempotent** and inert while the legacy pass is enabled:
  with legacy on, qualifying loops are already gone by regalloc time, so the
  SSA pass finds no candidates and returns 0 — same coexistence contract as
  `ssa:loop_rotate`.
- Leave all downstream cleanup to the existing SSA pipeline (`ssa:sccp`,
  `ssa:branch`, `ssa:dce`, `ssa:dead_loop`); the pass itself performs no
  cascade. Whether that pipeline reproduces the legacy call-site cascade's
  *result* is a gated question (see "Retirement hazard").

### Interaction with other passes

- `ssa:dead_loop` (`-O2` only) is complementary, not overlapping: it handles
  pure loops that provably **run** and produce constants; this pass handles
  loops that provably **never run**. No shared shapes.
- `ssa:sccp` may independently prove some register-only candidates dead inside
  the fixed point; the standalone pass firing first merely presents SCCP with
  simpler IR. If Step 0 shows SCCP covers a shape at both `-O1` and `-O2`,
  the standalone pass still accepting it is harmless duplication.
- While the legacy `tccgen.c` call site remains enabled, the legacy
  `loop_const_sim` / `loop_unroll` / IV-SR passes continue to see
  post-elimination IR, so their behavior is unchanged during coexistence.

## Required Unit Tests

v1 runs on flat IR, so tests live beside the pass consumers in the existing
harness. Extend `tests/unit/arm/armv8m/test_opt_loop_dead.c` (which already
builds every relevant IR shape) with `ssa_opt_first_iter_exit` variants,
mirroring how `test_opt_loop.c` gained `test_ssa_loop_rotate_*`.

Minimum scenarios (assert change count **and** resulting IR: rewritten JUMP
target, NOPed body, untouched non-loop code):

- port of each firing legacy shape: TEST_ZERO EQ/NE, CMP LT, LEA+deref,
  store-through-pointer;
- port of each declining legacy shape: not-taken, CMP-false, unsigned
  semantics, call invalidation, unknown value, no loop, exit target inside
  loop, unknown token, intervening entry jump;
- **new:** header with an extra external predecessor (jump into the loop from
  below) declines — the explicit single-entry guard;
- **new:** non-loop instructions interleaved inside the flat
  `[header, latch]` index range survive elimination (block-membership NOPing,
  the case legacy flat-range NOPing would corrupt);
- **new:** bottom-tested (already-rotated) loop declines;
- idempotency: second run on the pass's own output returns 0;
- inert coexistence: running the pass on IR the legacy pass already processed
  returns 0.

IR regression test (closes the existing coverage hole): add
`tests/ir_tests/NN_first_iter_exit.c` (next free number) with the
`20070824-1.c` pointer-chase shape plus a runtime-value control loop that must
**not** be eliminated; register it in `TEST_FILES` in
`tests/ir_tests/test_qemu.py`.

## Migration Steps

- [x] Run the Step 0 coverage-gap experiment; record the shape matrix here and
  pick outcome (a) retire-directly or (b) implement the pass. Steps below
  assume (b); under (a), skip to the regression-test and retirement steps.
  → **Outcome (b)** (see "Step 0 Results" above).
- [x] Add the IR regression test `tests/ir_tests/344_first_iter_exit.c`
  (verified green at `-O0/-O1/-O2` via `run.py` both before and after the
  migration; registered in `TEST_FILES`).
- [x] Refactor `ir/opt_loop_dead.c` to expose the shared walk/eval entry
  points (`ld_first_iter_prove`).  Superseded the same day by the full move
  (below).
- [x] Add `ssa_opt_first_iter_exit` unit tests (list above) to
  `test_opt_loop_dead.c` — 15 legacy shape ports + extra-header-pred decline,
  interleaved-non-loop-code survival, bottom-tested decline (18 total, green).
- [x] Implement `ssa_opt_first_iter_exit` in `ir/opt/ssa_opt_loop.c`; declare
  in `ir/opt/ssa_opt.h`.
- [x] Invoke from `ir/regalloc.c` after `ssa:loop_rotate`, gated
  `optimize >= 1 && opt_const_prop`, wrapped in `tcc_ir_opt_pass_disabled` +
  `tcc_ir_dump_after_pass`.
- [x] Confirm observability (`-dump-ir-passes=ssa:first_iter_exit`) and
  disable knob (`TCC_DISABLE_PASS=ssa:first_iter_exit`).
- [x] Confirm idempotency (unit test) and inert coexistence — verified while
  both passes were briefly wired: shape matrix with legacy enabled was
  byte-identical to baseline.
- [x] Remove the legacy call site **including its cleanup cascade**
  (tccgen.c Phase 4c.5) — see "Retirement decision" below.
- [x] Delete `ir/opt_loop_dead.c` (legacy driver removed; the LdState
  analysis moved wholesale into `ir/opt/ssa_opt_loop.c` as static code);
  removed from `Makefile` + unit-test Makefile + `ir/opt.h`; updated
  `tests/unit/PASS_COVERAGE.md`.

### Retirement decision (2026-07-06)

The maintainer compared codegen against the baseline with the legacy pass
disabled and accepted the small regression ("all tests are passing,
regression in codegen is small, let's remove — we will optimize later if this
will be needed").  The legacy call site and driver were removed the same day
the SSA pass landed, skipping the planned coexistence window.

Replacement-only shape matrix (identical to the final retired state): every
eliminable shape collapses to the same 2-instruction function as under
legacy, at both `-O1` and `-O2`; all declines preserved.  The one known
codegen regression is the `20070824-1.c` cleanup-cascade parity gap: the loop
is eliminated, but the post-loop `if (!s) abort()` fold and dead-alloca
removal that the legacy call-site cascade performed do not yet happen at SSA
time (main() 24 insns vs. 2, unreachable abort path survives; runtime
behavior verified correct at `-O0/-O1/-O2`).  Documented in the tccgen.c
Phase 4c.5 tombstone comment; revisit at SSA level if it matters for code
size.

Fuzz validation (sweeps + diff_olevels) is run by the maintainer after this
lands; fixes will be triggered from there.

## Retirement hazard (separate gate for removing the legacy call site)

Removing the `tccgen.c` call site changes more than the pass itself:

- **Downstream legacy consumers.** `loop_ptr_iv_exit_subst`,
  `loop_const_sim`, `loop_unroll`, LICM, and IV-SR will start seeing
  never-executing loops that used to be gone. That must be correctness-neutral
  (they treat it as an ordinary loop) but can cost compile time and code size
  (e.g. unrolling a dead loop that only dies at SSA time). Gate on the fuzz
  diff plus a code-size comparison (`metrics/` tooling) over the IR test
  corpus and torture suite.
- **Cleanup-cascade parity.** The call-site cascade (const_prop +
  `stack_addr_nonnull_fold` + `dead_trailing_addrvar_store_elim` +
  `dead_alloca_vreg_elim` rounds) currently collapses the post-loop
  null-check/abort and the dead alloca. At SSA time this work must fall out of
  `ssa:sccp`/`ssa:branch`/`ssa:dce`. Verify on `20070824-1.c` (and every
  Step-0 shape) that the final disassembly at `-O1`/`-O2` contains no
  resurrected abort path, alloca, or dead stores. If parity fails, either the
  relevant SSA cleanup is extended (own mini-plan) or the regression is
  documented and accepted explicitly — silence is not acceptance.
- The `-O1` question: legacy fires at `-O1`; `ssa:dead_loop` is `-O2`-only by
  the O1/O2 split policy. First-iteration exit is constant/branch folding of a
  never-entered loop — `-O1`-class work — so the replacement stays at `-O1+`.
  Confirm GCC-O1-parity metrics don't shift.

## Acceptance

For landing `ssa:first_iter_exit` (legacy still enabled):

- [ ] New unit tests pass; existing 15 legacy unit tests pass unchanged.
- [ ] New IR regression `NN_first_iter_exit.c` passes.
- [ ] `make cross -j$(nproc)`
- [ ] `make test -j16`
- [ ] GCC torture suite (`pytest tests/gcctestsuite/ -v`, at minimum
  `20070824-1.c` at `-O0/-O1/-O2`).
- [ ] **Maintainer-run** (per 2026-07-06 instruction, after the implementation
  is complete): `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0
  divergent profiles, and `python3 scripts/diff_olevels.py --seeds 0-5000
  --require-qemu` → 0 divergences.
- [ ] **Maintainer-run**: same sweep with
  `TCC_DISABLE_PASS=ssa:first_iter_exit` (pass off must equal today's
  behavior) and, once wired, with the legacy call site disabled locally
  (replacement-only run) before proposing retirement.
- [ ] **Maintainer-run**: zero new divergences; fixes triggered from there.

For the separate retirement step: all of the above re-run after the call-site
removal, plus the code-size comparison and cleanup-parity disassembly checks
from "Retirement hazard", plus the maintainer's wider fuzz sweep before the
legacy code is deleted.

## Assumptions

- `ssa:loop_rotate` remains at its current `ir/regalloc.c` position; if the
  rotation plan's own retirement steps move it, this pass moves with it,
  keeping the rotate → first-iter order.
- The legacy pass stays enabled and untouched (beyond the shared-helper
  refactor) until the retirement gate passes.
- `IRLoop`/`tcc_ir_detect_loops` is not used by the new pass; candidates come
  from the throwaway CFG. Any helper still needed from
  `ir/opt_loop_utils.h` (`evaluate_compare_condition`, `loop_size_cmp`) is
  treated as shared, not legacy-owned.
