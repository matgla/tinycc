# Legacy Loop Passes Not Yet Ported to SSA

**Status:** snapshot · **Generated:** 2026-07-07 · **Branch:** `legacyOptRemoval`

Scope: the legacy pre-SSA / post-RA *loop* optimization tail tracked by
[`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
This file is the current not-done list for that tracker. Adjacent non-loop
flat-IR cleanup/fusion passes are intentionally out of scope (see "Out of scope"
below) and are **not** counted as "unported".

## Still not ported (0)

All enumerated loop passes are resolved. The last one, `backedge_phi_hoist`,
was **relocated** on 2026-07-07 to a stable `ra:` pass name (kept, not ported):
it is a **post-RA** pass reading `ir->ls` intervals, so it cannot go through
`tcc_ir_ssa_opt_run()`. Step 0 proved it materially live (fires+matters on
29/416 IR tests at -O1, 26/416 at -O2), so retirement was rejected. Migration
was observability only (`TCC_DISABLE_PASS=ra:backedge_phi_hoist` + dump name);
engine + unit tests unchanged. See
[plan_legacy_loop_backedge_phi_hoist_ssa.md](plan_legacy_loop_backedge_phi_hoist_ssa.md).

`reroll` was migrated to `ssa:reroll` on 2026-07-07 (relocated to the
post-propagation regalloc flat region; thin driver over the retained engine).
See [plan_legacy_loop_reroll_ssa.md](plan_legacy_loop_reroll_ssa.md).

## Already done (12) — for reference

All twelve enumerated loop passes are no longer invoked from the `tccgen.c` tail
(verified: 0 call sites each). Ported to SSA:

- `loop_rotation` → `ssa:loop_rotate` (`ir/regalloc.c`)
- `loop_dead_first_iter` → `ssa:first_iter_exit` (`ir/regalloc.c`)
- `loop_ptr_iv_exit_subst` → `ssa:ptr_iv_exit_subst` (`ir/opt/ssa_opt_loop.c`)
- `loop_const_sim` → `ssa:loop_const_sim`
- `loop_unroll` → `ssa:loop_unroll`
- `licm` → `ssa:licm` (relocated to regalloc-time flat region)
- `iv_strength_reduction` → `ssa:iv_strength_reduction`
- `decrement_to_zero` → `ssa:decrement_to_zero`
- `dead_loop_elim` → `ssa:dead_loop` owns collapse (legacy retired)

Retired with no SSA replacement (proven inert/unsound):

- `loop_bound_remat` (retired; `ra:bound_remat` revival design kept)
- `loop_postinc_fusion` (retired; `LOAD/STORE_POSTINC` opcodes kept)
- `loop_guard_elim` (retired; `ssa:sccp` + `ssa:branch` cover the residue)

Relocated (kept, cannot be `ssa:` — post-RA):

- `backedge_phi_hoist` → `ra:backedge_phi_hoist`

## Out of scope (not "unported")

The tracker explicitly excludes adjacent non-loop flat-IR cleanup and fusion
passes still running in the `tccgen.c` tail — e.g. late copy propagation,
DSE/DCE cascades, stack-address CSE/folding, postinc-assign folding, branch
cleanup, and ARM target SSA fusions. These are not slated for SSA migration by
this plan and are not counted above.
