# Plan: Replace `loop_unroll` with `ssa:loop_unroll`

**Status:** SSA replacement landed **and legacy retired** (2026-07-06) ·
**Created:** 2026-07-06

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents:
[`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md)
(v1 flat-IR placement, shared-mutator retention, ordering-hazard gate),
[`plan_legacy_loop_dead_first_iter_ssa.md`](plan_legacy_loop_dead_first_iter_ssa.md)
(Step-0 coverage experiment, CFG-membership candidate detection,
cleanup-parity gate),
[`plan_legacy_loop_ptr_iv_exit_subst_ssa.md`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md)
(member-span synthetic `IRLoop`, single-entry/step-legality facts,
process-one-then-rebuild control-flow-change rule), and
[`plan_legacy_loop_const_sim_ssa.md`](plan_legacy_loop_const_sim_ssa.md)
(the sibling *upstream* pass — LCS runs immediately before the unroller today
and its migration decides this pass's ordering; read its "Interaction with
other passes" section, which already names loop_unroll as its own retirement
hazard).

## Intent

Replace the legacy pre-SSA loop unrolling / constant-trip elimination pass
(`tcc_ir_opt_loop_unroll`) with a CFG/dominator-driven pass that preserves its
current behavior. The legacy driver dispatches, per detected loop, to three
mutators in priority order — all three are "loop_unroll" behavior and the
replacement must subsume all three:

1. **`try_eliminate_loop`** (closed-form counter/accumulator): a register-only
   counted loop with a computable trip count `> 0` collapses to per-IV
   final-value ASSIGNs (`i = init + trip*step`), NOPing the body, the IV inits,
   and the rotated pre-loop guard; restores the exit edge with an explicit
   `JUMP exit_target` when fall-through no longer reaches it (`need_exit_jump`,
   seed 198468 / test 317).
2. **`try_eliminate_loop_symbolic`** (SELECT closed-form for a *symbolic* limit):
   `for (i=0;i<n;i++) acc += c;` with `init=0, step=1` and a pre-loop guard
   becomes `CMP n,#0` + `MUL V_acc = n*c` + `SELECT V_acc = (n>0)?V_acc:0`
   (codegen lowers SELECT to an ITE block). The unconditional-fallback closed
   form is deliberately **not** taken (it can't guard the zero-trip case for a
   symbolic limit — test 185).
3. **`try_unroll_loop_ex`** (full body replication): a register-only counted
   loop with trip `∈ (0, UNROLL_MAX_TRIP_COUNT=16]`, body `≤ 32` insns, total
   `≤ 128` insns, replicates the body `trip` times with the IV substituted by
   its per-iteration constant, per-iteration TEMP renaming (≤16 renamed TEMPs,
   escape-checked), IV-final assignment, and the same `need_exit_jump`
   restoration. Grows the IR (via `insert_instr_at`) only for single-loop
   functions when the region is too small.

All current conservative bails are preserved:

- register-only bodies only — LOAD/STORE/*_INDEXED/*_POSTINC/BLOCK_COPY, any
  memory-reading operand, calls, `FUNCPARAM*`, internal `JUMPIF`, inline asm,
  and 4-operand ops (LOAD/STORE_INDEXED, *_POSTINC, MLA, SELECT — whose 4th
  pool slot `write_instr_at_nop` cannot clone) all decline
  (`collect_body_instructions`; `try_eliminate_loop`'s "only IV/NOP/JUMP/CMP"
  body check);
- back-edge marked `no_unroll` by the re-roller declines (`try_unroll_loop_ex`
  reads `compact_instructions[end_idx].no_unroll`);
- `try_unroll_loop_ex` over-cap body rejected outright, never truncated
  (`collect_body_instructions` scans the full range — random-C seed 18);
- external entries into the loop body (not the header) decline;
- the zero-trip / `trip > max` / body-too-big / IR-growth-in-multi-loop cases
  all decline.

Behavior intentionally **not** preserved / explicit non-goals for v1:

- **No SSA-native (phi-aware) unrolling** inside the `tcc_ir_ssa_opt_run` fixed
  point, and no partial/runtime unrolling. v1 is the same flat-IR mutation on
  the same constant-trip shapes, only with CFG-verified candidates. SSA-native
  unrolling is a possible v2 once a one-call CFG/SSA reconstruction entry point
  exists (same deferral as rotation/const_sim).
- **No widening** of `UNROLL_MAX_TRIP_COUNT` (16), `UNROLL_MAX_BODY_INSNS` (32),
  `UNROLL_MAX_TOTAL_INSNS` (128), the `≤16` TEMP-rename cap, or the supported-op
  set. No new mutator shapes.
- **No merge with `loop_const_sim` or `dead_loop_elim`.** `try_eliminate_loop`
  overlaps conceptually with LCS's IV-mode and with `ssa:dead_loop`, but the
  three keep distinct guards and fuzz histories; deduplicating them is a
  separate future plan (see "Interaction with other passes"). This migration
  only moves the three unroll mutators.
- **No new consumer fold.** Unlike `ptr_iv_exit_subst`, the unroller does not
  own a downstream compare fold; the constant-folding of the expanded
  straight-line body is delegated (Step-0 decides to SSA vs. an inline cascade;
  see "Cleanup cascade").

## Current Legacy Shape

- **Call site:** single site in `tccgen.c` "Phase 5a"
  (`tccgen.c:30119-30161` at time of writing), gated `tcc_state->opt_loop_unroll`
  — set at `-O2` only (`libtcc.c:2325`, inside the `optimize >= 2` block),
  toggled by `-floop-unroll`/`-fno-loop-unroll`. On success (`unrolled > 0`) it
  runs `compact_nops` then a **≤10-round** post-unroll cleanup cascade:
  `dce` → `dse` → `const_prop` → `const_prop_tmp` → `branch_folding` →
  `stack_addr_nonnull_fold` → `setif_branch_fuse` → `stack_bool_diamond` →
  `or_bool_diamond` → `var_tmp_fwd` → `value_tracking` (each `opt_*`-gated).
  This cascade is where most of the *value* lives: it collapses the expanded
  `0+5+5+5+5+5` into `25`. **Retiring the call site relocates that folding
  work** (Step-0 probe 1 / Retirement hazard).
- **Ordering:** legacy runs `loop_const_sim` (Phase 4e) immediately before
  (LCS starves the unroller of constant-trip candidates), then unroll (5a),
  then LICM (Phase 5), then IV-SR (Phase 6). Rotation and first-iter-exit,
  which historically preceded this region, have already migrated to regalloc
  time; the tccgen unroller therefore now sees **un-rotated** for-loops plus
  source-level do-while shapes (the engine handles both; the rotated-guard NOP
  branch fires on bottom-tested layouts only).
- **Observability:** `dump_ir_after_pass(..., "ZZ_loop_unroll")` under
  `CONFIG_TCC_DEBUG` only, emitted once after the whole 5a block; **not**
  registered with `tcc_ir_opt_pass_disabled`, so `TCC_DISABLE_PASS` cannot
  isolate it. Today's only isolation knob is `-fno-loop-unroll`, which also
  kills LCS (shared gate). `scripts/bisect_opt.py` lists `loop-unroll` among
  its knobs, so unroll *and* LCS bugs both attribute to it. The replacement
  fixes all of this (`ssa:loop_unroll` dump name + disable knob).
- **Driver:** `tcc_ir_opt_loop_unroll` (`ir/opt_loop.c:577`, prototype
  `ir/opt.h:808`). Structure:
  1. candidates from legacy `tcc_ir_detect_loops` flat ranges;
  2. **overlapping-loop merge** (a C for-loop yields two back-edges → two
     detected loops → merged into one; absorbed loops marked `start_idx = -1`);
  3. **external-entry skip** (function-wide scan: a JUMP/JUMPIF from outside
     `[start,end]` targeting *inside the body* but not the header → decline,
     protects outer-loop back-edges after jump threading);
  4. per surviving loop, dispatch `try_eliminate_loop` → else
     `try_eliminate_loop_symbolic` → else `try_unroll_loop_ex`.
- **Mutators (the actual cores, all in `ir/opt_loop_utils.c`, all take
  `IRLoop *`):**
  - `try_eliminate_loop` (`:2795`) — IV selection via `find_induction_vars_ex`
    (`allow_copy_through=1`) + `find_loop_exit_condition` + `compute_trip_count`;
    body-shape check; NOP body + write final-value ASSIGNs for used-after IVs +
    NOP inits + NOP rotated guard + `need_exit_jump`.
  - `try_eliminate_loop_symbolic` (`:2526`) — symbolic-limit counter IV
    (`find_loop_exit_condition_op`, non-immediate limit, `init=0,step=1`);
    body-shape check; slot-fit check; SELECT path only (unconditional fallback
    is dead code — bails for the symbolic zero-trip reason); rewrites the
    pre-loop guard region in place (`write_select_at_nop`, `tcc_ir_pool_add`).
  - `try_unroll_loop_ex` (`:3008`) — `no_unroll` check; IV + trip;
    `collect_body_instructions` (`:2295`, the full legality/memory/4-operand
    scan); slot-fit with optional `insert_instr_at` growth (single-loop only,
    with **sibling `IRLoop` record patching** — parent tracker's designated
    high-risk side-table remapping); NOP region; per-iteration clone with IV
    constant substitution + TEMP renaming; IV-final; `need_exit_jump`.
- **Shared helpers consumed (all remain shared, not legacy-owned):**
  `find_induction_vars_ex`, `find_loop_exit_condition`,
  `find_loop_exit_condition_op`, `compute_trip_count`,
  `collect_body_instructions`, `write_instr_at_nop`, `write_select_at_nop`,
  `insert_instr_at`, `loop_size_cmp` (`ir/opt_loop_utils.c`), and
  `tcc_ir_detect_loops` / `tcc_ir_free_loops` (`ir/licm.c`). The three `try_*`
  mutators are **already** the shared cores (they operate on an `IRLoop *`),
  declared in `ir/opt_loop_utils.h:92-94` and exercised directly by the unit
  tests — so unlike the const_sim engine there is **no core to extract**; the
  refactor is detection-only.
- **Known-fragile spots** (the parent tracker's high-risk areas, present here):
  flat `[start_idx,end_idx]` ranges from `tcc_ir_detect_loops` + the
  overlap-merge heuristic + the function-wide external-entry scan (the seed-278
  class: a switch dispatch's case-before-jump layout can satisfy the detector's
  backward-jump test without being a natural loop); `insert_instr_at` IR growth
  with sibling-record patching (`try_unroll_loop_ex`); the `is_jump_target`
  side-table write in `need_exit_jump`; the rotated pre-loop-guard NOP that
  assumes the guard sits in `[init_idx+1, start_idx)`.
- **Existing unit tests:**
  - `tests/unit/arm/armv8m/test_opt_loop.c` — 3 top-level driver tests through
    `tcc_ir_opt_loop_unroll`: store-body-blocks-fold (`:398`, memory guard),
    no-loop-returns-zero (`:430`), pure-counters-eliminated-not-unrolled
    (`:442`, the eliminate-first ordering). `UT_COVERS("loop_unroll")` at
    `:1415`.
  - `tests/unit/arm/armv8m/test_opt_loop_utils.c` — rich core coverage:
    `emit_unrollable_loop` (`:415`); unroll register-body-three-iters (`:455`),
    store-body-blocks (`:491`), store-body-used-after (`:509`),
    single-iter-blocks (`:540`), trip-over-max (`:555`), trip-zero (`:571`),
    no_unroll-flag (`:585`), body-with-call (`:600`), no-iv (`:624`),
    internal-jumpif (`:641`); eliminate pure-counter-and-accumulator (`:935`),
    unused-iv-no-final-assign (`:988`), side-effect-body-blocked (`:1010`),
    no-iv-gives-up (`:1029`), zero-trip-gives-up (`:1041`); plus the shared IV /
    trip-count / find-exit battery these depend on.
- **Existing IR regressions** (in `TEST_FILES`, `tests/ir_tests/test_qemu.py`):
  `108_loop_unroll_basic.c`, `109_loop_unroll_no_unroll.c`,
  `110_loop_unroll_with_array.c`, `185_loop_elim_zero_trip.c` (symbolic
  zero-trip guard — `try_eliminate_loop_symbolic`),
  `218_fuzz_loop_unroll_branch_fallthrough.c`,
  `278_fuzz_unroll_switch_dispatch_loop.c` (false loop from the detector's
  dominance-free backward-jump test), `308_fuzz_unroll_indexed_store_alias.c`
  (memory guard), `317_fuzz_loop_elim_missing_exit_jump.c` (`try_eliminate_loop`
  `need_exit_jump`, seed 198468). **Note:** `318_fuzz_dead_loop_elim_missing_exit_jump.c`
  is the *same seed* but a **different pass** (`tcc_ir_opt_dead_loop_elim`,
  `ir/opt_dce.c`) — it belongs to the `dead_loop_elim` tracker entry, not this
  one; keep them distinct.
- **Known fuzz history tied to this pass:** seed 278 (switch-dispatch false
  loop → detector needs dominance), the test-218 branch-fallthrough case, the
  test-308 indexed-store-alias case (closed by the memory guard), and seed
  198468 / test 317 (`need_exit_jump` for the then-arm-of-`if` layout — memory
  [[loop-elim-missing-exit-jump-class]]). The cluster is exactly the parent
  tracker's designated high-risk area: **flat-range candidates + side-table
  mutation**. CFG-fact candidates remove the false-loop and external-entry
  classes structurally.

## Step 0: Coverage / Cascade-Relocation Experiment (decision gate)

Run before writing code; record the matrix here. Unlike the three landed
precedents, loop_unroll's payoff is **downstream** of the mutation (the
post-unroll cleanup cascade collapses the expansion), so the decisive question
is not "does the pattern survive to regalloc time" but "does the fold still
happen once unrolling moves after the tccgen cleanup cascade". Probes, all on
the motivating shapes (`for(i=0;i<5;i++) sum+=i;` accumulator; a plain
constant-trip register loop; a symbolic-limit `while(i<n) acc+=c;`; a
trip-count-eliminable empty then-arm loop) compiled at `-O2`:

1. **Where does the constant fold happen after relocation?** Compile with the
   Phase 5a block compiled out and `ssa:loop_unroll` wired at regalloc time.
   Does the SSA pipeline (`ssa:sccp`/`ssa:gvn`/`ssa:fold`) collapse the unrolled
   straight-line `0+5+5+5+5+5` to `25`? Attribute the collapsing pass via
   `-dump-ir-passes` diffs. **This decides the Cleanup-cascade section**:
   - **(fold-in-SSA)** SSA folds it → no inline cascade needed; delegate to
     `ssa:sccp`/`ssa:fold`/`ssa:dce` (best outcome — SSA const-prop strictly
     dominates the flat cascade).
   - **(fold-inline)** SSA leaves residue → run a bounded flat cascade
     (`const_prop`/`const_prop_tmp`/`branch_folding`) inside the pass after a
     firing, mirroring the legacy tail but before CFG build. Record exactly
     which passes are needed; do not silently accept a narrowing.
2. **Code-size delta.** For each shape and over the IR-test corpus + torture
   suite (`metrics/` tooling), compare final code size baseline vs. relocated.
   Unrolling *expands*; if probe 1 lands on (fold-inline) with residue, size
   can regress. This is the mirror image of the const_sim size hazard and gates
   retirement, not landing.
3. **Do the SSA pointer/rotation passes already dissolve any candidate?** The
   pass now runs after `ssa:loop_rotate` / `ssa:first_iter_exit` /
   `ssa:ptr_iv_exit_subst`. Confirm none of them removes a loop the unroller
   would take (they shouldn't — rotate reshapes, first-iter handles trip==0,
   ptr_iv only rewrites post-loop reads). Dump at each pass to confirm the
   counted loop with trip `∈ (0,16]` still arrives intact.
4. **Symbolic-limit SELECT survives.** Confirm the
   `try_eliminate_loop_symbolic` SELECT shape (test 185) still lowers to an ITE
   block at regalloc time — i.e. the guard-region rewrite
   (`write_select_at_nop` + `tcc_ir_pool_add`) is valid at the new call point
   (it manipulates the flat pool, which exists; confirm, don't assume).
5. **LCS interaction (only if `loop_const_sim` is already migrated).** If
   `ssa:loop_const_sim` has landed and runs immediately before this pass, verify
   it still starves the unroller of the shapes it folds (register-only
   constant-trip) so the two do not both fire on the same loop. If LCS is *not*
   yet migrated, this pass runs after the still-legacy LCS in program order for
   now (see Placement) and the interaction is unchanged from today.

Outcomes:

- **(a)** probe 1 shows SSA already folds every relocated shape with acceptable
  size (probe 2) → implement `ssa:loop_unroll` with an SSA-delegated cleanup
  (no inline cascade); this is the target design.
- **(b)** probe 1 needs an inline cascade → implement with the minimal recorded
  flat cascade between rounds; note the passes and the reason.
- **(c)** probe 2 shows an unacceptable size regression on relocation that no
  cleanup placement fixes → the unroller's *value* depends on the tccgen
  pipeline position; fall back to the in-place migration variant (detector swap
  + observability/naming, call site stays in `tccgen.c` order) and revise
  Placement before implementing. This is the real risk for this pass and the
  reason Step 0 leads with the cascade question.

Record the shape-by-shape matrix here before implementation starts.

### Step 0 Results (2026-07-06, `48823c03` + in-flight const_sim wiring)

Method: measured final `-O2` disassembly of the four motivating shapes
(`scratchpad/s0_shapes.c`) in two configs — **baseline** (legacy Phase 5a on)
and **experiment** (a temporary call to the *existing* legacy
`tcc_ir_opt_loop_unroll` wired at the regalloc site, right after
`ssa:ptr_iv_exit_subst`, where loops are already rotated; the temporary wiring
carries no post-unroll cleanup cascade, so only the SSA/regalloc pipeline folds
its output). Temp wiring reverted after measuring.

| shape | mutator | baseline (tccgen site) | experiment (regalloc site) | reading |
|---|---|---|---|---|
| `s2_counter` `for(i<7)c++` | `try_eliminate_loop` | folded → `movs r0,#7` | folded → `movs r0,#7` | pure-counter closed form fires at **both** sites |
| `s4_then_arm` empty loop in `if` | `try_eliminate_loop` + `need_exit_jump` | loop removed, then-arm correct | same | fires at both; `need_exit_jump` intact |
| `s3_symbolic` `while(i<n)a+=3` | `try_eliminate_loop_symbolic` | **loop survives** (SELECT not emitted) | **`cmp #0; add.w r1,r0,r0,lsl#1; ite gt; movgt/movle`** = `(n>0)?n*3:0` | **INERT at tccgen, FIRES at regalloc → migration restores it** |
| `s1_accum` `for(i<5)s+=i` | `try_unroll_loop_ex` | loop survives | loop survives (still not unrolled) | full body-unroll did **not** fire at either site (see open probe) |

**Key finding — the legacy unroll is partially inert at its tccgen site, same
root cause as `ptr_iv_exit_subst`.** Since rotation migrated to regalloc time,
the tccgen unroller sees un-rotated split-body for-loops; the paths that need a
rotated/contiguous body do not fire there:

- `try_eliminate_loop` (IV-only closed form): fires at both sites — it needs
  only the IV init/step/exit, not the body layout.
- `try_eliminate_loop_symbolic` (SELECT/ITE): **inert at tccgen, restored at
  regalloc** (probe 4 = YES: `write_select_at_nop` + `tcc_ir_pool_add` are valid
  at the new site; the SSA/regalloc pipeline folds the `n*3` MUL and lowers the
  SELECT to a clean ITE with no residue → probe 1 = YES for the closed-form
  path).
- `try_unroll_loop_ex` (body replication): did not fire on `s1_accum` at either
  site.

**Outcome: (a)/(b) — implement `ssa:loop_unroll` at the regalloc site after
`ssa:loop_const_sim`.** The symbolic-path restoration alone (dead today,
correct+smaller after) justifies the pass, and the closed-form fold happens in
the SSA/regalloc pipeline with no inline cascade needed for that path — so the
cleanup is **SSA-delegated (outcome a)** for the eliminate/symbolic paths. The
code-size hazard is smaller than the plan feared because the only *expanding*
path (`try_unroll_loop_ex`) appears rarely reached.

**Open probe — partially resolved.** `try_unroll_loop_ex` (body replication)
does **not** fire under the flat detector, at either site: `prod16`
(`for(i<4) p*=2` — multiplicative, so only full-unroll can fold it; `scratchpad/s0_unroll.c`)
and `s1_accum` both stay loops at `-O2`, baseline **and** at the experimental
regalloc site. But this is the **flat-detector** inertness (the same
split-layout range bug that made ptr_iv inert), **not** proof body-unroll is
dead: `tcc_ir_detect_loops` hands `collect_body_instructions` a range that
excludes/mismatches the body for these layouts. The CFG-based
`ssa:loop_unroll` will hand the mutator a **correct** synthetic `IRLoop`, so it
may **revive** body-unroll exactly as `ssa:ptr_iv_exit_subst` revived pr49644.
Consequences for the implementation:

- Keep the full `try_unroll_loop_ex` machinery (growth path, TEMP renaming,
  1-element `IRLoops` wrapper) — it is *not* dead code once CFG detection is
  correct.
- The code-size hazard and the probe-1 fold question (does SSA collapse
  `p<<1<<1<<1<<1 → 16` / `0+1+2+3+4 → 10`) **remain live** and are resolved
  definitively by testing `prod16` and `s1_accum` through the real pass: if
  they unroll, confirm the SSA/regalloc pipeline folds the expansion (outcome a)
  before relying on SSA-delegated cleanup; if the expansion survives unfolded,
  add the minimal inline cascade (outcome b) or decline the expanding path.
- The two `try_eliminate_*` paths are already validated (s2/s4 fire everywhere;
  s3 symbolic restored at regalloc with clean SSA fold), so the pass lands
  regardless; this probe only sizes the body-unroll path's risk.

**Probe resolved after implementation:** with the real CFG-driven pass built,
`prod16` **still does not unroll** (`try_unroll_loop_ex` declines even with a
correct synthetic `IRLoop`), while `s3` symbolic folds and `s2`/`s4` eliminate.
So **body-unroll is effectively inert** in this tree — the migration's live
value is the two `try_eliminate_*` paths plus the restored symbolic SELECT,
whose closed forms the SSA/regalloc pipeline folds with no residue. Consequences:
**outcome (a)** confirmed (SSA-delegated cleanup, no inline cascade), and the
**code-size hazard is moot** (nothing expands). The `try_unroll_loop_ex` path +
1-element `IRLoops` growth wrapper are retained for legacy parity but currently
exercise no shape; keep them (a future shape may hit them) but the retirement
code-size gate can be a spot-check, not a blocker.

## Implementation bug found + fixed: nested loop-carried accumulator

The first build hung `gcc.c-torture/execute/991216-4` at `-O2` (infinite loop).
Root cause, and the reason the driver is **outermost-only**:

- `991216-4` inlines `bug(5,10)` into a nest: outer `while(num<5)` containing
  inner `for(i=1;i<10;i++) num++`, with `num` **loop-carried** by the outer loop.
- An early **innermost-first** driver processed the inner loop first.
  `find_induction_vars_ex` walks back from the inner preheader and found `num`'s
  "init" `V<--#0` — but that assignment is the **outer** loop's preheader, not a
  per-entry init; `num` is carried in with a different value each outer pass.
- `try_eliminate_loop` then treated `num` as a closed-form IV, NOPed its "init"
  **and** the "pre-loop guard" it scans for — which was actually the outer loop's
  exit test `CMP num,#5` — leaving the outer back-edge `JMP` with no exit →
  infinite loop.
- The legacy pass never hit this: its overlap-merge collapsed the nest into one
  loop that then declined on internal branches, so it effectively processed
  **outermost-only and never unrolled nested inner loops**. The CFG-correct
  detector exposed the latent unsoundness in the shared mutators
  (`find_induction_vars_ex` reads inits from before the preheader, which is only
  sound for a non-nested single-entry loop).
- **Fix:** the driver processes **outermost loops only** (a header whose loop is
  contained in another header's loop is skipped; the enclosing loop declines on
  its own internal branches). This matches legacy behavior *and* restores IV-init
  soundness. Mirrors `ssa:loop_const_sim`'s outermost-only policy.
- Verified: `991216-4` exits 0 at `-O2`; `347` green at `-O0/-O1/-O2/-Os`; `s3`
  symbolic fold and `s2`/`s4` elimination unaffected.

## SSA Replacement Design

(Assumes Step 0 outcome (a)/(b); under (c) the Placement subsection is replaced
by the in-place variant and the rest stands.)

### Placement: standalone flat-IR pass before SSA construction (v1)

Same placement and rationale as all three siblings: full unrolling grows and
mutates flat IR (in-place NOP + `write_instr_at_nop`, and `insert_instr_at`
growth), which the SSA fixed point cannot rebuild CFG/dominators/phis across.
v1 runs in `ir/regalloc.c` as the last pass of the pre-SSA flat region, after
the landed trio:

```
ssa:loop_rotate → ssa:first_iter_exit → ssa:ptr_iv_exit_subst
   → [ssa:loop_const_sim] → ssa:loop_unroll → cfg/ssa build
```

Ordering notes:

- **After `ssa:loop_const_sim`** (when it lands) reproduces the legacy
  4e-before-5a order: LCS folds register-only constant-trip loops to residuals
  first, so the unroller sees fewer candidates to expand — the exact starvation
  the legacy order relies on. If `loop_const_sim` has **not** yet migrated when
  this pass lands, `ssa:loop_unroll` slots directly after `ssa:ptr_iv_exit_subst`
  and the legacy LCS still runs upstream in `tccgen.c` — functionally the same
  starvation, just split across the pipeline; the ordering tightens when LCS
  migrates. State which world the implementation lands in.
- **After `ssa:loop_rotate`** means the pass sees **bottom-tested** loops (the
  rotated-guard NOP branch fires); the current interim tccgen state is the
  opposite (un-rotated). Both orientations must be unit-tested (see below).
- **After `ssa:ptr_iv_exit_subst`**: the ptr_iv plan already notes unrolling a
  small counted loop *removes* it before its own pass would see post-loop reads.
  With unroll now *downstream* of ptr_iv, ptr_iv runs first and substitutes
  post-loop pointer reads; unroll then removes the loop body. Different vregs,
  no conflict — but confirm on the ptr_iv IR test (`345_ptr_iv_exit_subst.c`,
  which uses a trip count `>16` precisely to survive unrolling).

### Naming, files, observability

- Entry point: `int ssa_opt_loop_unroll(struct TCCIRState *ir)`, declared in
  `ir/opt/ssa_opt.h`, implemented in `ir/opt/ssa_opt_loop.c` (parent-tracker
  decision: loop-shape structural passes share this file, alongside
  `ssa_opt_loop_rotate` / `ssa_opt_first_iter_exit` /
  `ssa_opt_ptr_iv_exit_subst`).
- The **three mutators stay shared** in `ir/opt_loop_utils.c` (no extraction —
  they already take `IRLoop *`). Both the thin legacy driver and the new SSA
  driver call them during coexistence; the unit tests keep calling them
  directly. This is rotation's retained-`try_rotate_loop` model, one level
  richer (three cores instead of one).
- Invoke from `ir/regalloc.c` guarded by
  `tcc_ir_opt_pass_disabled("ssa:loop_unroll")` and followed by
  `tcc_ir_dump_after_pass(ir, "ssa:loop_unroll")` — observable via
  `-dump-ir-passes=ssa:loop_unroll`, isolable via
  `TCC_DISABLE_PASS=ssa:loop_unroll`. Both new capabilities.
- Gate: `tcc_state->opt_loop_unroll`, matching the legacy gate exactly
  (`-O2` default; manual `-floop-unroll` keeps working at lower levels;
  `-fno-loop-unroll` keeps its bisection meaning). Note the shared gate with LCS
  is unchanged; once *both* passes are migrated each gets its own
  `TCC_DISABLE_PASS` name even though they share the `-f` flag, which already
  improves bisection resolution.

### Comment policy (applies to every code change in this migration)

Per [[comments-max-one-liner]] and the const_sim plan's policy: new code (SSA
driver, unit tests) carries **no comment blocks** — at most a single-line
comment where the code cannot express the constraint. Any legacy comment in
*touched* code is deleted or compressed to one line. Fuzz-seed narratives and
design rationale live in this plan and the pinning tests, not in source. The
three `try_*` mutators are heavily commented; leave them untouched by the
detection-only refactor so their comments are neither moved nor preserved into
new code — if a mutator must be touched, its touched region sheds its comments.

### Candidate detection (the actual improvement)

Replace `tcc_ir_detect_loops` flat ranges + overlap-merge + external-entry scan
with dominance-verified facts, following the `ssa_opt_ptr_iv_exit_subst` /
`ssa_opt_first_iter_exit` pattern:

- Throwaway `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` per
  fixed-point round; **freed before any mutation** (insertion/NOP invalidate
  it). Candidates are back-edges `latch → header` where the header dominates
  the latch; natural-loop membership via the shared backward walk
  (`fie_collect_members`, already generalized for ptr_iv — reuse, don't copy).
- Process **outermost / smallest-first**: the legacy driver had no nesting
  policy beyond the overlap-merge; here, merge candidates whose membership sets
  intersect into their outermost enclosing candidate, and skip a candidate that
  is strictly nested inside another accepted this round. A body containing an
  inner back-edge is simply an internal `JUMPIF`/`JUMP` — `collect_body_instructions`
  already rejects internal `JUMPIF`, so nested loops decline exactly as today.
- **Single-entry guard (new, stronger than legacy's external-entry scan):** the
  header's predecessors must be exactly {one out-of-loop preheader predecessor,
  latch}, and no other member block may have an out-of-loop predecessor. This
  one structural fact replaces the function-wide external-entry scan (step 3)
  **and** kills the seed-278 false-loop class (a switch-dispatch backward jump
  produces no dominance-verified natural loop, and a side entry into the body
  declines here).
- **Synthetic `IRLoop`** for the three shared mutators:
  - `header_idx` = `start_idx` = header block's first instruction index;
  - `end_idx` = the **latch back-edge instruction** index (NOT `max member
    end_idx`). This is load-bearing: `try_unroll_loop_ex` reads
    `compact_instructions[end_idx].no_unroll` (the re-roller's marker) and NOPs
    exactly `[start_idx, end_idx]`. Verify with a member-span check that no
    non-member instruction lies in `[start_idx, end_idx]` — decline if one does
    (the legacy mutators assume a contiguous body; the ptr_iv split-body layout
    is not unrollable and must decline, not misfire).
  - `preheader_idx` = first non-JUMP/JUMPIF instruction walking back from
    `header_idx - 1` (`ir/licm.c:199-212`), so `find_induction_vars_ex` and the
    rotated-guard scan behave identically.
  - `body_instrs` / `num_body_instrs`: the mutators read `body_instrs` **only**
    via the legacy overlap-merge (which we drop); `try_unroll_loop_ex` and
    friends scan `[start_idx, end_idx]` directly. Set `body_instrs = NULL`;
    confirm at implementation time that no reached code path dereferences it
    (the ptr_iv migration verified the same for its core).
- **`insert_instr_at` growth preservation:** `try_unroll_loop_ex`'s growth path
  is gated `(!loops || loops->num_loops != 1)` → **declines when `loops ==
  NULL`**. To preserve the single-loop growth win, the SSA driver wraps the
  synthetic `IRLoop` in a **1-element `IRLoops`** and passes it (`loop_idx = 0`);
  `num_loops == 1` re-enables growth and the sibling-patch loop is a no-op (no
  siblings). Because we rebuild the CFG after every firing (below), processing
  is genuinely one-loop-at-a-time, so this is sound.
- Size cap: decline member spans over the legacy body/total caps as before
  (the caps live inside the mutators; the driver adds only a coarse span cap to
  bound the CFG work, matching the const_sim 256 note).

### Mutation strategy and CFG staleness

- **Process one candidate, then rebuild.** Every successful firing is a
  control-flow change (a loop is removed): `try_eliminate_loop` /
  `try_eliminate_loop_symbolic` NOP in place (indices stable but the CFG shape
  is now stale), and `try_unroll_loop_ex` may `insert_instr_at` (indices
  shift). Either way the CFG and all not-yet-processed candidates' indices are
  invalid. Rule: per CFG build, evaluate candidates smallest-first and **stop
  after the first firing**; the outer fixed point (cap 4, mirroring
  `SSA_FIRST_ITER_EXIT_MAX_PASSES`) rebuilds and continues. A round that fires
  nothing ends the pass. This is the same "process-one-then-rebuild" rule
  ptr_iv uses for its guard NOP, applied to *every* firing here.
- Change count = number of loops eliminated/unrolled, so a firing is visible to
  the fixed point and to dumps.
- Keep the mutators' `need_exit_jump` / rotated-guard-NOP / `is_jump_target`
  semantics verbatim — they are inside the shared cores and unchanged.
- **Idempotency:** the pass's own output contains no back-edge for a removed
  loop, so a second run finds no candidate for it → 0 further changes.
- **Coexistence:** with the legacy Phase 5a site enabled, qualifying loops are
  already eliminated/unrolled by regalloc time; the SSA driver finds no
  back-edge for them and returns 0. Unlike ptr_iv (which could still NOP a
  rotation guard the legacy site never saw), unroll's coexistence is expected
  **silent** — the legacy site and the SSA site fire on the identical shapes.
  The validation gate runs with both enabled regardless.

### Cleanup cascade

Decided by Step-0 probe 1. Two admissible outcomes, no silent narrowing:

- **(fold-in-SSA)** — preferred: after unrolling, the expanded straight-line
  arithmetic is folded by `ssa:sccp`/`ssa:gvn`/`ssa:fold` and dead-stripped by
  `ssa:dce` downstream. No inline cascade; the legacy ≤10-round tail is not
  replicated. This is strictly more powerful than the flat cascade and is the
  target.
- **(fold-inline)** — fallback if probe 1 shows SSA leaves residue: run a
  bounded flat cascade (the minimal subset of
  `const_prop`/`const_prop_tmp`/`branch_folding` recorded in Step 0) inside the
  pass after each firing, legal at this pre-SSA point, exactly as the const_sim
  plan provides for its own between-rounds fold. Record the chosen subset here.
- Either way, `dse`/`stack_bool_diamond`/`or_bool_diamond`/`setif_branch_fuse`/
  `value_tracking` from the legacy tail are **not** replicated inline — they are
  general cleanups the SSA pipeline and the remaining tccgen tail still run.
  Verify on the cascade shapes (Retirement hazard) that no residual dead store
  or unfolded branch survives relative to baseline; document any parity gap
  (first-iter's 20070824-1 precedent).

### Interaction with other passes

- **`ssa:loop_const_sim` (the ordering sibling).** LCS and `try_eliminate_loop`
  overlap on register-only constant-trip loops: LCS residualizes them (final
  ASSIGN/STOREs) and `try_eliminate_loop` collapses them to final-value ASSIGNs.
  Whichever runs first wins; the other finds no loop. Keep LCS upstream so it
  starves the unroller (legacy order). A future plan may fold
  `try_eliminate_loop` into LCS entirely (they compute the same closed form),
  but that is out of scope — this migration keeps all three unroll mutators.
- **`ssa:dead_loop`** (`-O2`, in the SSA fixed point): complementary. It handles
  loop-invariant-*result* loops (possibly unknown trip, guarded SELECT variant);
  the unroller handles loop-carried arithmetic with **known-bounded** trips.
  After `ssa:loop_unroll` fires, `ssa:dead_loop` sees fewer candidates. Note the
  separate `dead_loop_elim` tracker entry owns test 318 (same seed as unroll's
  test 317, different eliminator) — do not conflate them.
- **`ssa:first_iter_exit` / `ssa:loop_rotate` / `ssa:ptr_iv_exit_subst`:**
  upstream, disjoint (trip==0 peel / reshape / post-loop read rewrite). Confirm
  none dissolves an unrollable candidate (Step-0 probe 3).
- **Re-roll (`-freroll-blocks`, still legacy in `tccgen.c`):** sets `no_unroll`
  on back-edges it created; `try_unroll_loop_ex` honors it via
  `compact_instructions[end_idx].no_unroll`. The synthetic `IRLoop.end_idx` =
  latch back-edge index preserves this (see Candidate detection). Re-roll runs
  well upstream in tccgen, so the flag is set long before regalloc.
- **LICM / IV-SR (still legacy, still upstream in tccgen):** they run before
  the new pass and see un-unrolled loops after retirement (today they see
  post-unroll IR). Expected benign — a fully-unrolled loop is gone before their
  detection would matter, and a loop the unroller *declines* is unchanged. Their
  own migrations are tracked separately.

## Required Unit Tests

v1 runs on flat IR; tests live in the existing isolated harness. Add `ssa_`
variants driven through `ssa_opt_loop_unroll` alongside the legacy driver tests
in `tests/unit/arm/armv8m/test_opt_loop.c` (the 3 legacy driver tests and the
`test_opt_loop_utils.c` core tests stay untouched until retirement, then their
drivers are re-pointed or the legacy-only ones deleted — decide at retirement,
mirroring the ptr_iv port). Assert change counts **and** resulting IR (residual
final-value operands, NOPed span, unrolled clone count, untouched non-member
code):

- **eliminate path:** pure-counter + accumulator collapses to correct
  closed-form finals (port of `test_eliminate_loop_pure_counter_and_accumulator`
  through the SSA driver); unused IV gets no final assign; side-effect body
  declines; zero-trip / no-IV decline.
- **symbolic path:** SELECT closed form emitted for `while(i<n) acc+=c;` with a
  pre-loop guard; symbolic zero-trip is guarded (no unconditional fallback —
  the test-185 shape).
- **unroll path:** register body, trip 3 → 3 clones with IV constants
  substituted and body-local TEMPs renamed per iteration; store-body declines;
  body-with-call declines; internal-JUMPIF declines; trip==0 declines;
  trip>16 declines; `no_unroll`-marked back-edge declines; 4-operand body op
  (MLA/SELECT/*_INDEXED) declines; IV-used-after gets its final value; the
  `insert_instr_at` growth path fires for a single-loop function whose region
  is too small (the 1-element `IRLoops` wrapper — assert the IR grew and the
  clones are correct).
- **need_exit_jump:** loop as the then-arm of an `if` (exit jumps forward past
  an else block) — after elimination *and* after unrolling, assert an explicit
  `JUMP exit_target` is emitted and `exit_target.is_jump_target` set (the
  seed-198468 / test-317 shape, both mutator paths).
- **orientation:** both top-tested and bottom-tested (rotated) candidates fold
  (the legacy tccgen driver currently sees only un-rotated; the SSA pass sees
  rotated post-`ssa:loop_rotate`).
- **candidate-detection facts (new):** seed-278 switch-dispatch backward jump
  produces no candidate (no dominance-verified natural loop); a jump from
  outside into a non-header member block declines (single-entry fact); a nested
  loop declines (inner back-edge is an internal JUMPIF); a non-contiguous
  member span (non-member instruction inside `[start,end]`) declines.
- **idempotency:** second run on the pass's own output returns 0.
- **inert coexistence:** running the SSA pass on IR the legacy pass already
  unrolled returns 0.

Add/refresh the `opt_loop`/`opt_loop_utils` rows in
`tests/unit/PASS_COVERAGE.md` when the suite grows.

## IR Regression Test

`tests/ir_tests/346_loop_unroll_ssa.c` + `.expect` (next free number — 345 is
taken by `ptr_iv_exit_subst`; register in `TEST_FILES` in
`tests/ir_tests/test_qemu.py`):

- a constant-trip accumulator loop (`for(i=0;i<5;i++) sum+=i;` returning `10`)
  the unroller collapses — the shape whose correctness depends on the relocated
  fold happening at all (Step-0 probe 1);
- a symbolic-limit accumulator (`while(i<n) acc+=c;`) exercising the SELECT
  path, called with both `n>0` and `n<=0` to pin the zero-trip guard;
- a then-arm-of-`if` empty counted loop exercising `need_exit_jump` (the 317
  shape) so control does not fall into the else arm;
- a runtime-trip control loop that must **not** unroll and must still behave
  correctly.

Added **first**, green under the legacy pass at `-O0/-O1/-O2/-Os`, to pin
runtime behavior across the migration. The 8 existing unroll/elim IR tests
(108/109/110/185/218/278/308/317) are the acceptance corpus and stay green
throughout.

## Migration Steps

Per the maintainer's decision this migration **includes retirement in the same
task** (rotation/ptr_iv precedent), gated on the local acceptance runs plus the
code-size comparison; the wider fuzz sweep remains maintainer-run.

- [x] Run Step 0; fill in the matrix; pick outcome (a)/(b)/(c). **Outcome (a)/(b)**:
  symbolic path restored at regalloc, closed-form fold is SSA-delegated;
  `try_unroll_loop_ex` firing is an open probe (see Step 0 Results). Steps below
  assume (a)/(b); under (c) revise Placement to the in-place variant first.
- [x] Add IR regression `tests/ir_tests/347_loop_unroll_ssa.c` (+`.expect`)
  (346 was taken by `loop_const_sim`); green under the legacy pass at
  `-O0/-O1/-O2/-Os`; registered in `TEST_FILES`.
- [x] Add the `ssa_` unit tests to `test_opt_loop.c` (4: pure-counter
  eliminates, **nested-carried-accumulator declines** [991216-4 regression],
  idempotent, no-loops). Legacy driver + `test_opt_loop_utils.c` core tests
  untouched. 2864 tests, 0 failed.
- [x] Implement `ssa_opt_loop_unroll` in `ir/opt/ssa_opt_loop.c` (CFG
  candidates + membership + single-entry/contiguity facts + synthetic `IRLoop`
  with latch `end_idx` + 1-element `IRLoops` wrapper + one-then-rebuild fixed
  point calling the three shared `try_*` mutators); declared in
  `ir/opt/ssa_opt.h`. Reuses `lcs_collect_header_members`/`fie_collect_members`.
  **Outermost-only** (see the nested-accumulator bug section — required for
  soundness, matches legacy + const_sim).
- [x] Wire from `ir/regalloc.c` after `ssa:loop_const_sim`, gated
  `tcc_state->opt_loop_unroll`, wrapped in `tcc_ir_opt_pass_disabled` +
  `tcc_ir_dump_after_pass`. Cleanup is **SSA-delegated (outcome a)** — no inline
  cascade (Step-0 + probe resolution: closed forms fold cleanly, body-unroll
  inert so nothing expands).
- [x] Confirmed observability (`-dump-ir-passes=ssa:loop_unroll`) and disable
  knob (`TCC_DISABLE_PASS=ssa:loop_unroll` reverts to legacy behavior — used to
  bisect the 991216-4 hang).
- [x] Confirmed idempotency (unit test) and coexistence (full regression green
  with both passes enabled).
- [x] Local gates with legacy still enabled: unit tests (2864, 0 failed),
  `347_loop_unroll_ssa` at `-O0/-O1/-O2/-Os`, **full regression PASSED**
  (maintainer-run) on the fixed binary.
- [x] **Retirement (done alongside `loop_const_sim`'s retirement).** Removed the
  `tccgen.c` Phase 5a call site + its ≤10-round post-unroll cleanup cascade
  (tombstone comment matching the Phase 4e const_sim tombstone); deleted the
  legacy driver `tcc_ir_opt_loop_unroll` + `__timed` (`ir/opt_loop.c`) and its
  `ir/opt.h` prototype (both replaced by pointer comments); the three `try_*`
  mutators + `collect_body_instructions` stay shared in `ir/opt_loop_utils.c`
  (still called by `ssa_opt_loop_unroll` and covered by `test_opt_loop_utils.c`);
  deleted the 3 legacy driver tests + `emit_unrollable_loop_top` helper +
  prototype + `UT_COVERS("loop_unroll")` + registrations from `test_opt_loop.c`
  (mutator coverage retained in `test_opt_loop_utils.c`; the 4 `ssa_` driver
  tests stay). `make cross` clean; **`make test -j14` passing** (maintainer-run);
  parent tracker ticked. Cascade removal is safe because the pass was
  register-only-inert at the tccgen site (Step 0) and the SSA pipeline folds the
  residual closed forms.

## Retirement hazard (separate gate for removing the legacy call site)

- **Cascade relocation / code size** — the central hazard, unique to this pass:
  the legacy value lives in the post-unroll cleanup cascade. After removal the
  fold happens at SSA time ((a)) or in the inline cascade ((b)); gate removal on
  probe-2 code size over the IR corpus + torture suite. If size regresses and no
  cleanup placement fixes it, the options (in order) are: keep the inline
  cascade minimal-but-sufficient, move the pass earlier relative to LCS, or fall
  back to the in-place migration (Step-0 outcome (c)). Silence is not
  acceptance.
- **Cleanup-cascade parity** — the ≤10-round tail also serviced downstream
  phases (post-unroll constants feeding later tccgen passes). Verify on the
  cascade shapes (the new 346, plus 108/110) that final `-O2` disassembly has no
  surviving loop, residual dead store, or unfolded guard branch relative to
  baseline. Document and explicitly accept any parity gap (20070824-1
  precedent).
- **`-O1` question:** none — legacy is O2-only via `opt_loop_unroll`; the
  replacement keeps the same gate. Confirm `-floop-unroll` at `-O0/-O1` still
  reaches the new pass (manual-flag parity).
- **Compile time:** the pass adds a CFG build per fixed-point round at `-O2` in
  regalloc; bounds are unchanged from legacy (≤4 rounds, same mutator caps).
  Spot-check selfhost compile time.

## Acceptance

For landing `ssa:loop_unroll` (legacy still enabled):

- [ ] New `ssa_` unit tests pass; existing legacy driver + core unit tests pass
  unchanged.
- [ ] All 8 unroll/elim IR regressions + new `346_loop_unroll_ssa.c` pass at
  `-O0/-O1/-O2` (and `-Os`).
- [ ] `make cross -j$(nproc)`; `make test -j16`; GCC torture suite.
- [ ] **Maintainer-run** (per the 2026-07-06 instruction, after implementation):
  `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0 divergent profiles;
  `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` → 0
  divergences; plus a run with `TCC_DISABLE_PASS=ssa:loop_unroll` (pass off must
  equal today's behavior).
- [ ] **Maintainer-run:** zero new divergences; any divergence triggers fixes
  before the branch merges.

For the separate retirement step: all of the above re-run after the call-site
removal, plus the code-size comparison and cleanup-parity disassembly checks
from "Retirement hazard", plus the maintainer's wider fuzz sweep and a
replacement-only run (legacy call site disabled locally) before the legacy
driver is deleted.

## Assumptions

- `ssa:loop_rotate` / `ssa:first_iter_exit` / `ssa:ptr_iv_exit_subst` (and
  `ssa:loop_const_sim` if landed) keep their `ir/regalloc.c` positions; this
  pass slots directly after them and moves with them.
- The three `try_*` mutators and `collect_body_instructions` remain shared
  utilities in `ir/opt_loop_utils.c` and are not deleted with the legacy driver;
  `find_induction_vars_ex` / `find_loop_exit_condition(_op)` /
  `compute_trip_count` stay shared analysis (also consumed by `loop_const_sim`
  and guard-elim).
- The `insert_instr_at` growth path is preserved via the 1-element `IRLoops`
  wrapper; if Step 0 shows that path never fires on realistic post-rotation
  shapes, dropping it (always declining growth) is an acceptable simplification
  — record the decision.
- The `UNROLL_MAX_*` caps and the register-only / 4-operand / `no_unroll` guards
  are considered permanent for this pass; any widening is a new plan with its
  own fuzz gate.
- `dead_loop_elim` (owning test 318) is a separate tracker entry and is not
  touched here despite the shared seed-198468 `need_exit_jump` lineage.
