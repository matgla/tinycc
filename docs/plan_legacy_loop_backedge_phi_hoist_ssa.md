# Plan: `backedge_phi_hoist` -> `ra:backedge_phi_hoist`

**Status:** relocated + kept · **Created:** 2026-07-07

Last enumerated entry of [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Unlike the other tail entries this one is **not** an SSA candidate: it runs
**after** `tcc_ir_ssa_regalloc`, reads physical-register liveness (`ir->ls`
intervals), and rewrites branches over already-allocated code. SSA is gone by
then, so the correct migration outcome is a **relocation with a stable `ra:`
pass name**, not a port to an `ssa:` pass and not retirement.

The main tracker previously mislabeled it a "pre-SSA loop transform"; it is
post-RA. This document corrects that and closes the entry.

## Intent

- Preserve: the post-RA loop branch save. The engine matches
  `CMP; JUMPIF cond -> exit; ASSIGN x N (phi copies); JUMP body(backward)` and
  rewrites it to `ASSIGN x N; JUMPIF !cond -> body`, eliminating one
  unconditional back-edge branch per matched loop while re-homing the coalesced
  phi copies ahead of the inverted test.
- Not preserved / non-goal: an SSA form of this transform. It depends on
  register-allocation facts (spill state, coalescing) that only exist post-RA,
  so it stays a flat post-RA pass.

## Current Legacy Shape

- Call site: [`tccgen.c`](../tccgen.c) immediately after `tcc_ir_ssa_regalloc`,
  gated `tcc_state->optimize > 0`.
- Source: `tcc_ir_opt_backedge_phi_hoist` in [`ir/opt_promote.c`](../ir/opt_promote.c).
- Prototype: `ir/opt.h`.
- Unit tests: `tests/unit/arm/armv8m/test_opt_promote_extra.c`
  (`test_backedge_phi_hoist_inverts_and_hoists`,
  `test_backedge_phi_hoist_spilled_operand_kept`).
- Dedicated IR test: none; exercised incidentally by the loop-heavy IR tests
  (see firing evidence below).
- Known fuzz regressions tied to this pass: none on record.

## Step 0: firing / materiality

Method: compile a corpus with the pass on vs. `TCC_DISABLE_PASS=ra:backedge_phi_hoist`
and byte-compare the objects (`armv8m-tcc -mcpu=cortex-m33 -nostdlib` + newlib
includes, `-c`). A byte difference proves the pass both fires and changes
codegen.

| Corpus | -O1 fires+matters | -O2 fires+matters |
|---|---|---|
| `tests/ir_tests/*.c` (416 files) | 29 / 416 | 26 / 416 |
| `gen_c.py` fuzz seeds 0-599 | 0 / 600 | 0 / 600 |

The fuzz generator is arithmetic/checksum-heavy and rarely emits the matched
back-edge shape, so its 0% is expected and not evidence of inertness. The IR
suite (which contains real loop tests) shows ~7% firing, concentrated exactly
where expected: `108/109_loop_unroll*`, `179_loop_carried_store`,
`180_loop_rotation_condbody`, `185_loop_elim_zero_trip`, and the
`100-103_pure_func_*` strlen/strcmp loops.

Conclusion: **materially live** -> relocate + keep. Retirement (the outcome for
`loop_bound_remat`, `dead_loop_elim`, `loop_guard_elim`) is rejected here
because those were proven inert and this is not.

## Migration (done)

- [x] Rename observability to the `ra:` namespace at the call site: gate becomes
  `optimize > 0 && !tcc_ir_opt_pass_disabled("ra:backedge_phi_hoist")` and a
  `tcc_ir_dump_after_pass(ir, "ra:backedge_phi_hoist")` follows it, matching the
  regalloc-region idiom (`ir/regalloc.c`). Knob:
  `TCC_DISABLE_PASS=ra:backedge_phi_hoist`.
- [x] Knob verified: disabling flips codegen on firing files; a bogus pass name
  is a no-op (name-specific, not a blanket disable).
- [x] Engine unchanged (no rewrite, so its existing safety guards and the
  `test_opt_promote_extra.c` coverage carry over verbatim).
- [x] Enumerate + close the entry in the main tracker.

## Non-goals / follow-ups

- The sibling post-RA passes `tcc_ir_opt_post_ra_forward_diamond` and
  `tcc_ir_opt_abort_tail_merge` also lack `TCC_DISABLE_PASS` knobs, but they are
  branch/tail cleanups, **not loop transforms**, so they are out of scope for
  the loop-migration tracker. Giving them `ra:` knobs is a reasonable separate
  observability cleanup.
- A future post-RA loop-branch cluster (with `ra:bound_remat`, revival design in
  [`plan_legacy_loop_bound_remat_ssa.md`](plan_legacy_loop_bound_remat_ssa.md))
  could co-own `ra:backedge_phi_hoist`; no action needed now.

## Acceptance

- [x] `make cross -j$(nproc)`
- [x] `make test-ir -j16` (13491 passed, 246 skipped, 1 xfailed, 0 failed)
- [x] `run_unit_tests` (2849 tests, 0 failed; both `test_backedge_phi_hoist_*` ok)
- [x] Knob honored + name-specific (Step 0 verification).
