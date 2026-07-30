# Plan: Replace Legacy Loop Optimizations with SSA-Based Passes

**Status:** proposed tracker · **Created:** 2026-07-06

This document is the source of truth for retiring the legacy pre-SSA loop
optimization tail and replacing it with SSA/CFG-driven passes. It is a tracking
plan only: detailed design for each specific loop optimization should be added
after this general plan is in place.

## Goal

Remove handwritten pre-SSA loop transforms from the late `tccgen.c`
optimization sequence only after SSA-based replacements are functionally
equivalent, deliberately narrower with documented tradeoffs, or proven
unnecessary.

The replacement path should converge loop optimization work around the existing
SSA infrastructure:

- `ir/regalloc.c` constructs SSA and runs `tcc_ir_ssa_opt_run()` at `-O1+`.
- `ir/opt/ssa_opt*.c` already owns SSA use-def chains, value rewrites, branch
  simplification, strength reduction, dead-loop handling, and target hooks.
- Legacy loop transforms currently mutate flat IR before SSA construction, which
  makes them sensitive to stale instruction indices, loop redetection, and
  pass-order side effects.

## Current State

Legacy loop behavior is still driven directly from `tccgen.c`, after much of
the non-SSA propagation/memory cleanup pipeline and before SSA regalloc. These
passes are not fully represented in the declarative `ir/opt_pipeline.c` tables,
so removing them is not just a file cleanup; their call sites, gates, dump names,
tests, and follow-up cleanup cascades must be migrated deliberately.

Current legacy loop inventory:

| Legacy behavior | Main entry point | Primary source |
|---|---|---|
| Loop-invariant code motion (arithmetic hoist + pure/const-call hoist) | `tcc_ir_opt_licm_ex` | `ir/licm.c` |
| Loop rotation | `ssa_opt_loop_rotate` (was `tcc_ir_opt_loop_rotation`, removed) | `ir/opt/ssa_opt_loop.c` / `ir/opt_loop_utils.c` |
| First-iteration exit peeling | `tcc_ir_opt_loop_dead_first_iter` | `ir/opt_loop_dead.c` |
| Pointer-IV exit substitution | `ssa_opt_ptr_iv_exit_subst` (was `tcc_ir_opt_loop_ptr_iv_exit_subst`, removed) | `ir/opt/ssa_opt_loop.c` |
| Loop constant simulation | `tcc_ir_opt_loop_const_sim` | `ir/opt_loop_const_sim.c` |
| Loop unrolling / pure-counter elimination | `tcc_ir_opt_loop_unroll` | `ir/opt_loop.c` / `ir/opt_loop_utils.c` |
| IV strength reduction | `tcc_ir_opt_iv_strength_reduction*` | `ir/opt_loop.c` / `ir/opt_loop_utils.c` |
| Loop-bound rematerialization | `tcc_ir_opt_loop_bound_remat` (retired, removed) | `ir/opt_loop.c` |
| Loop post-increment fusion | `tcc_ir_opt_loop_postinc_fusion` | `ir/opt_fusion.c` |
| Identical-block re-rolling | `ssa_opt_reroll` (was `tcc_ir_opt_reroll`, relocated) | `ir/opt_reroll.c` |
| Decrement-to-zero | `tcc_ir_opt_decrement_to_zero` | `ir/opt.c` |
| Dead-loop elimination | `tcc_ir_opt_dead_loop_elim` | `ir/opt_dce.c` |
| Sequential guard elimination | `tcc_ir_opt_loop_guard_elim` | `ir/opt_loop.c` |
| Back-edge phi hoisting (post-RA branch save) | `tcc_ir_opt_backedge_phi_hoist` | `ir/opt_promote.c` |

Adjacent non-loop cleanup and fusion passes must be audited but not removed as
part of this work unless a specific replacement plan calls them out. Examples:
late copy propagation, DSE/DCE cleanup cascades, stack-address CSE/folding,
postinc assign folding, branch cleanup, and ARM target SSA fusions.

LICM was added to the inventory 2026-07-07 (it was previously missing despite
being the head of the IV-SR cluster). Re-rolling (`reroll`) was enumerated and
migrated 2026-07-07. `backedge_phi_hoist` — the last remaining tail entry — was
enumerated and relocated 2026-07-07; it is a **post-RA** pass (not pre-SSA as
earlier noted here), so it is kept and given a `ra:` name rather than ported to
SSA. With that, every enumerated loop transform is resolved.

## Migration Rules

- Migrate one legacy loop optimization at a time.
- Write or identify regression coverage before changing behavior or removing a
  legacy pass.
- Implement every new SSA-based loop optimization together with focused unit
  tests for the new SSA pass surface.
- Keep the legacy implementation available until the SSA replacement has passed
  the full gate for that pass.
- Preserve pass observability with stable dump/pass names, preferably
  `ssa:<feature>` names compatible with `-dump-ir-passes` and
  `TCC_DISABLE_PASS`.
- Do not carry temporary bisection gates such as `TCC_SKIP_SSA*` into committed
  code.
- Treat direct instruction insertion, side-table remapping, and loop redetection
  as high-risk areas; prefer SSA/CFG facts over flat range scans where possible.
- Delete old code only after callers, prototypes, build lists, tests, and docs
  are updated in the same migration step.

## TODO Tracker

### General

- [ ] Inventory every legacy loop optimization call site in `tccgen.c`.
- [ ] Map each legacy pass to its source file, gate flag, dump name, existing
  IR tests, and unit tests.
- [ ] Decide the SSA pass naming convention and final dump names.
- [ ] Define the SSA loop-analysis substrate needed by replacements.
- [ ] Decide whether replacements live in existing `ssa_opt_dead_loop.c` /
  `ssa_opt_strength.c` or new `ssa_opt_loop*.c` files.
- [ ] Add or extend host unit-test harness coverage for every new SSA loop pass.
- [ ] Define the minimum acceptance gate for disabling each legacy pass.
- [ ] Migrate one legacy loop optimization at a time.
- [ ] Remove obsolete legacy code only after the SSA replacement passes all
  gates.

### Per Legacy Pass

- [x] `loop_rotation` — ported to `ssa:loop_rotate`. See [`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md).
- [x] `loop_dead_first_iter` — ported to `ssa:first_iter_exit` (accepted 20070824-1
  codegen regression). See [`plan_legacy_loop_dead_first_iter_ssa.md`](plan_legacy_loop_dead_first_iter_ssa.md).
- [x] `loop_ptr_iv_exit_subst` — ported to `ssa:ptr_iv_exit_subst` (legacy was inert;
  SSA pass restores the pr49644 `p != &a[N]` fold via a LEA-aware consumer CMP+JUMPIF
  fold). See [`plan_legacy_loop_ptr_iv_exit_subst_ssa.md`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md).
- [x] `loop_const_sim` — ported to `ssa:loop_const_sim` (CFG/dominator front-end
  drives the retained ~2k-line `lcs_fold_region` engine; outermost disjoint loops
  folded per CFG build). See [`plan_legacy_loop_const_sim_ssa.md`](plan_legacy_loop_const_sim_ssa.md).
- [x] `loop_unroll` — ported to `ssa:loop_unroll` (outermost-only for soundness;
  CFG/dominator front-end + retained shared mutators; restores the symbolic-SELECT
  closed form). See [`plan_legacy_loop_unroll_ssa.md`](plan_legacy_loop_unroll_ssa.md).
- [x] `licm` — relocated to `ssa:licm` 2026-07-07 (first Phase-A pass, before
  `ssa:iv_strength_reduction`, gated `opt_licm`/-O2, knob `TCC_DISABLE_PASS=ssa:licm`).
  Thin driver over the retained proven engine (`tcc_ir_opt_licm_ex`: dom-LICM arithmetic
  + `tcc_ir_hoist_pure_calls`) — no rewrite, so bug #7's ~10 fixed pure-call defects are
  not re-derived. The IV-SR coupling that motivated an in-place v1 was **already gone**
  (IV-SR self-detects since its own relocation), so the tccgen Phase-5 block is deleted
  (erasing the `-fno-licm` brace-gate bug). Removing it exposed + fixed a latent
  out-of-bounds read: `ir_skip_nops_forward` indexed a **negative** jump target from
  `tcc_ir_opt_select` (guard `start < 0 → return n`; fixes `pr42716` -O1/-O2). Validation:
  `make test-ir` 13504 passed, unit tests 2848/0-fail (new `test_ssa_opt_licm_hoists_invariant`),
  `ssa:licm` proven firing, `diff_olevels` 0-1500 → 0 divergences. See
  [`plan_legacy_loop_licm_ssa.md`](plan_legacy_loop_licm_ssa.md).
- [x] `iv_strength_reduction` — relocated to `ssa:iv_strength_reduction` 2026-07-07
  (full relocation; Step 0 hit outcome (c) as the downstream cluster had dissolved).
  CFG/dominator front-end, outermost-only; `local_alu_cse` coupling proven benign.
  See [`plan_legacy_loop_iv_strength_reduction_ssa.md`](plan_legacy_loop_iv_strength_reduction_ssa.md).
- [x] `loop_bound_remat` — retired 2026-07-07, no SSA replacement (v1a observability
  proved legacy byte-identical/inert; `ra:bound_remat` revival design kept; 216 pin
  kept). See [`plan_legacy_loop_bound_remat_ssa.md`](plan_legacy_loop_bound_remat_ssa.md).
- [x] `loop_postinc_fusion` — retired, no SSA replacement (disabled by default +
  unsound; `LOAD/STORE_POSTINC` opcodes kept for a post-regalloc revival). See
  [`plan_legacy_loop_postinc_fusion_ssa.md`](plan_legacy_loop_postinc_fusion_ssa.md).
- [x] `reroll` — relocated to `ssa:reroll` 2026-07-07 (thin driver over the
  retained engine `tcc_ir_opt_reroll`, first in the regalloc flat region). The
  win is placement: the legacy pass ran **pre-propagation** and was
  counterproductive (its counter-loop inhibited downstream const-folding of
  foldable macro-unrolled runs → larger code); **post-propagation** those runs
  are already collapsed, so `ssa:reroll` only fires on non-foldable repetition
  that genuinely survives (e.g. period-3 opaque-call runs → counted loop, smaller
  + correct). Knob `TCC_DISABLE_PASS=ssa:reroll`; matcher criteria unchanged
  (no fuzz-risk broadening). See [`plan_legacy_loop_reroll_ssa.md`](plan_legacy_loop_reroll_ssa.md).
- [x] `decrement_to_zero` — ported to `ssa:decrement_to_zero` (SUBS/CMP#0 fusion
  unblocked by scanning over the out-of-SSA identity-move phi copy; retained
  `dtz_try_region` engine; new IR pin 349). See
  [`plan_legacy_loop_decrement_to_zero_ssa.md`](plan_legacy_loop_decrement_to_zero_ssa.md).
- [x] `dead_loop_elim` — retired, no new port (Step 0 proved legacy inert; `ssa:dead_loop`
  owns collapse at -O2). Behavior change: `-fno-dce` no longer suppresses collapse;
  `TCC_DISABLE_PASS=ssa:dead_loop` is the knob. See
  [`plan_legacy_loop_dead_loop_elim_ssa.md`](plan_legacy_loop_dead_loop_elim_ssa.md).
- [x] `loop_guard_elim` — retired, no SSA replacement (sound but inert; `ssa:sccp`+`ssa:branch`
  cover the residue; `ssa:guard_elim` fallback design kept). See
  [`plan_legacy_loop_guard_elim_ssa.md`](plan_legacy_loop_guard_elim_ssa.md).
- [x] `backedge_phi_hoist` — relocated to `ra:backedge_phi_hoist` 2026-07-07 (kept, not
  ported: it is a **post-RA** pass reading `ir->ls` intervals, so SSA is unavailable).
  Step 0 proved it materially live (fires+matters on 29/416 IR tests at -O1, 26/416 at
  -O2 — loop_unroll/rotation/carried_store/elim + pure_func strlen loops), so retirement
  is rejected. Migration = observability only: added `TCC_DISABLE_PASS=ra:backedge_phi_hoist`
  knob + `ra:backedge_phi_hoist` dump name at the call site; engine + unit tests unchanged.
  See [`plan_legacy_loop_backedge_phi_hoist_ssa.md`](plan_legacy_loop_backedge_phi_hoist_ssa.md).

## Detailed Pass Plans

- [`licm` -> `ssa` (planned, not started)](plan_legacy_loop_licm_ssa.md)
- [`loop_rotation` -> `ssa:loop_rotate`](plan_legacy_loop_rotation_ssa.md)
- [`loop_dead_first_iter` -> `ssa:first_iter_exit`](plan_legacy_loop_dead_first_iter_ssa.md)
- [`loop_ptr_iv_exit_subst` -> `ssa:ptr_iv_exit_subst`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md)
- [`loop_const_sim` -> `ssa:loop_const_sim`](plan_legacy_loop_const_sim_ssa.md)
- [`loop_unroll` -> `ssa:loop_unroll`](plan_legacy_loop_unroll_ssa.md)
- [`iv_strength_reduction` -> `ssa:iv_strength_reduction`](plan_legacy_loop_iv_strength_reduction_ssa.md)
- [`loop_bound_remat` (retired)](plan_legacy_loop_bound_remat_ssa.md)
- [`loop_postinc_fusion` (retired)](plan_legacy_loop_postinc_fusion_ssa.md)
- [`reroll` -> `ssa:reroll`](plan_legacy_loop_reroll_ssa.md)
- [`dead_loop_elim` -> `ssa:dead_loop` (retired)](plan_legacy_loop_dead_loop_elim_ssa.md)
- [`decrement_to_zero` -> `ssa:decrement_to_zero`](plan_legacy_loop_decrement_to_zero_ssa.md)
- [`loop_guard_elim` (retired)](plan_legacy_loop_guard_elim_ssa.md)
- [`backedge_phi_hoist` -> `ra:backedge_phi_hoist` (relocated, post-RA)](plan_legacy_loop_backedge_phi_hoist_ssa.md)

## Per-Pass Migration Template

Copy this block into a new section when investigating a specific pass.

```markdown
## <legacy pass name> -> <ssa pass name>

### Intent

- [ ] State the behavior the replacement must preserve.
- [ ] State any behavior that will intentionally not be preserved.

### Current Legacy Shape

- [ ] List call sites in `tccgen.c`.
- [ ] Record gate flags, dump names, and disable knobs.
- [ ] Record source files and helper ownership.
- [ ] Record existing IR and unit tests.
- [ ] Record known fuzz regressions tied to this pass.

### SSA Replacement Design

- [ ] Define required SSA/CFG facts.
- [ ] Define candidate loop pattern(s).
- [ ] Define legality guards and side-effect rules.
- [ ] Define mutation strategy.
- [ ] Define pass ordering relative to existing SSA passes.
- [ ] Define pass name used by dumps and `TCC_DISABLE_PASS`.

### Migration Steps

- [ ] Add or update regression tests.
- [ ] Add SSA implementation behind the chosen pass name together with focused
  unit tests.
- [ ] Verify replacement with legacy pass still enabled.
- [ ] Disable legacy call site for the covered behavior.
- [ ] Run the full validation gate.
- [ ] Delete obsolete legacy code and tests, or mark retained helpers as shared.

### Acceptance

- [ ] `make cross -j$(nproc)`
- [ ] `make test -j16`
- [ ] `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu`
- [ ] Zero new divergences.
```

## Acceptance Criteria

For this documentation-only plan:

- [ ] Markdown renders cleanly.
- [ ] Referenced filenames and pass names match the current tree.
- [ ] No build is required.

For each future implementation step:

- [ ] Add or preserve focused regression tests under `tests/ir_tests/`.
- [ ] Add or update unit tests where the pass has a host-testable surface.
- [ ] Rebuild with `make cross -j$(nproc)`.
- [ ] Run `make test -j16`.
- [ ] Run `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu`.
- [ ] Confirm zero new fuzz divergences before deleting legacy code.

## Assumptions

- This first step creates only the Markdown tracker in `docs/`.
- Specific loop-pass investigations will be planned and implemented separately.
- Legacy code remains in place until each SSA replacement is covered and
  validated.
- A replacement may be narrower than the legacy pass only when the skipped
  behavior is documented and covered by tests or explicit non-goals.
