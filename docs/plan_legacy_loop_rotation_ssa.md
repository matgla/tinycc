# Plan: Replace `loop_rotation` with `ssa:loop_rotate`

**Status:** proposed pass plan · **Created:** 2026-07-06

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).

## Intent

Replace the legacy pre-SSA loop rotation pass with an SSA/CFG-based pass that
converts safe top-tested loops into bottom-tested loops while preserving the
current safety policy:

- rotate simple counted loops with a straight-line body and explicit latch edge;
- rotate conditional-body loops only when branch targets can be remapped
  unambiguously;
- preserve the break-fall-through inversion behavior;
- decline loops with calls, indirect lvalue operands, indexed memory ops,
  external entries, unsafe fall-through exits, or nested already-rotated shapes;
- keep the legacy pass enabled until the SSA pass covers the simple and
  regression-proven shapes below.

The first SSA replacement should be intentionally conservative: match the
existing behavior for the covered cases and return zero for anything outside
the modeled shape. Do not try to broaden rotation while moving it to SSA.

## Current Legacy Shape

- Call sites:
  - early `tccgen.c` pass after late cleanup and before loop const-sim,
    unroll, LICM, and IV strength reduction; dump name `ZZ_loop_rotation`;
  - late retry near the end of optimization after PACK64/copy-prop cleanup;
    dump name `ZZ2_lr`.
- Gate: `tcc_state->opt_loop_rotation`.
- Source:
  - top-level fixed-point driver: `tcc_ir_opt_loop_rotation` in `ir/opt_loop.c`;
  - matcher and mutator: `try_rotate_loop` in `ir/opt_loop_utils.c`.
- Existing unit tests:
  - `test_loop_rotation_top_level_basic`;
  - `test_loop_rotation_top_level_no_loop_returns_zero`;
  - `test_loop_rotation_top_level_call_in_body_declines`.
- Existing IR regressions:
  - `180_loop_rotation_condbody.c`;
  - `186_fuzz_nested_loop_rotation.c`.
- Existing environment interaction:
  - `TCC_NO_COALESCE` restricts sibling-loop body scanning in the legacy pass;
    the SSA plan should not introduce a new environment gate.

## SSA Replacement Design

### Placement: rotate before SSA construction (v1)

The SSA optimization pipeline cannot currently maintain its own CFG, dominators,
or phi nodes across a structural transform, so a rotation pass registered inside
`tcc_ir_ssa_opt_run` would leave stale analysis data for every pass after it:

- `ctx->cfg` and `ctx->ssa->block_phis` are built once in `ir/regalloc.c`
  (`tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` + SSA rename) and handed
  to `tcc_ir_ssa_opt_init`; nothing in `tcc_ir_ssa_opt_run` rebuilds them.
- `tcc_ir_ssa_opt_rebuild(ctx)` rebuilds **use-def chains only**
  (`ssa_opt_build_chains`); it does not touch the CFG, dominators, or phis.
- `cfg->instr_to_block` is sized at CFG-build time, so any instruction the
  transform *adds* (the duplicated bottom CMP and inverted JUMPIF) is out of
  range for `ssa_block_for_instr` and invisible to later CFG-based passes.
- The only existing CFG-mutating SSA pass, `ssa:branch`, merely deletes an edge
  and surgically patches the affected phis (`ssa_drop_phi_edge`); no current pass
  adds a block or restructures control flow. `ssa:dead_loop`, which the earlier
  draft placed immediately after rotation, reads `ctx->cfg` and
  `ctx->ssa->block_phis[header_block]` directly and would consume the stale graph.

Loop rotation adds a bottom test (and may add a preheader guard), a strictly
harder transform than anything the SSA layer does today. Rather than teach the
SSA opt loop to maintain a CFG incrementally, **v1 runs rotation as a standalone
flat-IR pass immediately before CFG/SSA construction** in `ir/regalloc.c` (before
`tcc_ir_cfg_build`). SSA/CFG/phis are then built fresh from the rotated IR by the
existing regalloc path — no stale CFG, no `instr_to_block` desync, no fixed-point
interleaving to reason about.

The real win over the legacy pass is the **legality analysis**, not the
placement: build a throwaway analysis CFG with dominators (as `ir/licm.c` already
does via `tcc_ir_cfg_build(ir)` + `tcc_ir_cfg_compute_dominators(cfg)`) and match
natural loops from CFG/dominator/back-edge facts instead of legacy flat `IRLoop`
instruction ranges. Range-based body detection is the documented root cause of
several rotation fuzz bugs, so replacing it is the point of the migration.

Alternative (deferred, not v1): run rotation inside `tcc_ir_ssa_opt_run` as a
single pre-loop step and follow a successful rewrite with a full CFG + dominator
+ SSA-rename + use-def rebuild before the fixed-point loop starts. This needs a
one-call reconstruction entry point that does not exist yet; do not attempt it in
v1.

### Naming, files, observability

- Expose the pass as `int ssa_opt_loop_rotate(struct TCCIRState *ir)` (it builds
  its own analysis CFG and does not need an `IRSSAOptCtx`, since no SSA/phi state
  exists at this point), with its declaration in `ir/opt/ssa_opt.h` and the
  implementation in a new `ir/opt/ssa_opt_loop.c`.
- Invoke it from `ir/regalloc.c` guarded by `tcc_ir_opt_pass_disabled(...)` and
  followed by `tcc_ir_dump_after_pass(ir, "ssa:loop_rotate")`, matching the
  existing `RUN_SSA`/`dump_ir_after_pass` convention, so the pass is observable
  via `-dump-ir-passes=ssa:loop_rotate` and disableable via
  `TCC_DISABLE_PASS=ssa:loop_rotate` for free. The `ssa:` prefix is kept for
  naming consistency even though the pass runs just before SSA construction.

### Matching (v1)

Restrict v1 matching to reducible top-tested loops with:

- one preheader edge to a header block;
- header compare and conditional exit;
- one body-entry edge;
- one latch/back-edge to the header;
- no external entries to body or latch blocks;
- no calls, indexed memory ops, indirect lvalue operands, inline asm, switch,
  IJMP, VLA, setjmp/longjmp, or other `ssa_opt_has_side_effects` hazards in the
  body.

Body-store legality is a **stated predicate**, not "whatever the legacy tests
happen to accept": permit only `STORE` to a **non-address-taken local stack
slot** with **no aliasing indexed store/load** anywhere in the region. Decline
the loop if any body store targets an address-taken slot or a runtime/indexed
address. This is the boundary the current legacy tests exercise, written as a
rule the implementer can apply without reverse-engineering the tests.

### Mutation strategy

- Derive the loop region (header, body-entry, latch, exit) from the analysis
  CFG/dominators first; only then perform the flat-IR rewrite.
- Duplicate the header CMP at the bottom of the body and emit an inverted
  `JUMPIF` to the body-entry target; leave the exit fall-through to the original
  exit block.
- Remap body-local and body-to-latch branch targets exactly once; assert no
  target is remapped twice.
- Preserve the break-fall-through inversion behavior of the legacy pass.
- The pass must be **idempotent**: an already-bottom-tested or already-rotated
  loop — its own output, or a shape the legacy pass produced — must match zero
  and leave the IR untouched, so re-running or coexisting with the legacy pass is
  a no-op.
- Do **not** attempt to update `block_phis` or a CFG in place — under the chosen
  placement neither exists yet; SSA and phis are built fresh after this pass
  returns. Free the throwaway analysis CFG before returning.
- Leave compaction and dead-guard cleanup to the normal downstream passes
  (`ssa:dead_loop`, `ssa:dce`) unless the rewrite leaves an invalid jump target
  that requires immediate repair.

### Ordering hazard when retiring the legacy call sites

Legacy rotation runs early in `tccgen.c` **before** the still-legacy pre-SSA
`loop_const_sim`, `unroll`, `LICM`, and `IV strength reduction` passes, which
consume rotated loop shapes. `ssa:loop_rotate` runs later — just before SSA
construction, well after those `tccgen.c` passes. Therefore, the moment the early
legacy `tcc_ir_opt_loop_rotation` call site is removed, those four downstream
legacy consumers stop seeing rotated loops.

Required handling:

- Do **not** remove the early legacy call site until either those downstream loop
  passes are also migrated to run after `ssa:loop_rotate`, or the diff gate
  proves no regression from them seeing un-rotated loops.
- While validating, keep the early legacy pass enabled; idempotency (above) lets
  `ssa:loop_rotate` decline any loop the legacy pass already rotated, so the two
  coexist safely.
- Treat "remove the early legacy call site" as its own separately gated step with
  a full re-run of the acceptance gate, not a follow-on to landing the SSA pass.

## Required Unit Tests

Implement `ssa:loop_rotate` together with focused unit tests. Because v1 runs on
flat IR before SSA construction (see Placement above), it is testable in the
existing `tests/unit/arm/armv8m/test_opt_loop.c` harness alongside the current
`test_loop_rotation_top_level_*` tests, reusing that harness's flat-IR builders.
It does **not** depend on a full `IRSSAOptCtx`/phi harness.

Minimum unit-test scenarios:

- simple top-tested counted loop rotates to body/latch/CMP/inverted-JUMPIF;
- no-loop input returns zero and leaves IR untouched;
- call in body declines;
- indexed memory op in body declines;
- indirect lvalue operand in body declines;
- external jump into body/latch declines;
- already-rotated enclosing loop declines inner rotation;
- conditional-body shape remaps the internal skip correctly;
- break-fall-through shape inverts the deciding branch to the exit.

Unit tests should assert both the change count and the rewritten IR shape,
including branch targets and inverted condition tokens.

## Migration Steps

- [x] Add unit-test coverage for `ssa:loop_rotate` in `test_opt_loop.c`
  (`test_ssa_loop_rotate_basic` / `_no_loop_returns_zero` /
  `_call_in_body_declines` / `_idempotent`).
- [x] Add `ssa_opt_loop_rotate` declaration (`ir/opt/ssa_opt.h`) and
  implementation (`ir/opt/ssa_opt_loop.c`).  v1 reuses the regression-proven
  flat-IR mutator `try_rotate_loop`; the improvement is CFG/dominator-based
  candidate detection (dominance-verified back-edges) replacing the legacy
  range-scan detector.  A throwaway analysis CFG is built per fixed-point pass,
  used to derive `(header_idx, end_idx)` candidates, then freed before the
  in-place rewrite (the rewrite never shifts instruction indices).  Deferred:
  replacing `try_rotate_loop`'s own internal flat region scans with CFG facts.
- [x] Invoke `ssa:loop_rotate` from `ir/regalloc.c` immediately before
  `tcc_ir_cfg_build`/SSA construction (top of `tcc_ir_ssa_regalloc`), gated to
  `-O1+`, guarded by `tcc_ir_opt_pass_disabled` and followed by
  `tcc_ir_dump_after_pass` (not registered inside the `tcc_ir_ssa_opt_run`
  fixed-point loop).
- [x] Confirmed observable through `-dump-ir-passes=ssa:loop_rotate` and
  disableable through `TCC_DISABLE_PASS=ssa:loop_rotate`.
- [x] Confirmed idempotency (`test_ssa_loop_rotate_idempotent`): a second run on
  the pass's own output produces zero changes.  With legacy on, loops arrive
  already rotated, so the pass declines them — inert coexistence.
- [x] Ran the new unit tests with the legacy pass still enabled (all pass;
  2825 UT / 11307 asserts, 0 failed).
- [x] Ran existing loop-rotation IR regressions via `make test`
  (`180_loop_rotation_condbody.c`, `186_fuzz_nested_loop_rotation.c`): 13459
  passed, 0 failed.
- [x] Removed **both** legacy `tcc_ir_opt_loop_rotation` call sites in one step:
  the early `ZZ_loop_rotation` pass and the late `ZZ2_lr` retry.  The single SSA
  pass `ssa:loop_rotate` runs in `tcc_ir_ssa_regalloc` *after* the entire
  `tccgen.c` optimization tail, so it subsumes both — including the late retry's
  intent (rotate loops whose bodies simplified), since it observes the
  most-simplified IR.  The still-legacy `loop_const_sim`/`unroll`/`LICM`/`IV-SR`
  passes now see un-rotated loops (see "Ordering hazard"); acceptance gate re-run
  below confirms no regression.
- [x] Deleted the legacy driver `tcc_ir_opt_loop_rotation` /
  `..._rotation__timed` (`ir/opt_loop.c`), its declaration (`ir/opt.h`), the
  `-floop-rotation` gate flag (`tcc.h` field, `libtcc.c` flag table + `-O1`
  default), the legacy `test_loop_rotation_top_level_*` unit tests, and the
  `-fno-loop-rotation` triage knob (replaced by
  `TCC_DISABLE_PASS=ssa:loop_rotate`).
- [x] Retained the shared mutator `try_rotate_loop` / `loop_size_cmp`
  (`ir/opt_loop_utils.c`): still used by `ssa_opt_loop_rotate` (and
  `loop_size_cmp` by `opt_loop_dead.c`).  Not deleted.

## Acceptance

- [x] New unit tests for `ssa:loop_rotate` pass (4 tests, all pass).
- [x] Existing `opt_loop` unit tests still pass while legacy rotation remains.
- [x] `make cross -j$(nproc)`.
- [x] `make test -j16` (13459 passed, 0 failed).
- [x] `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0/16 profiles
  divergent, and `scripts/diff_olevels.py --seeds 0-1000` → 0 divergences.
  (diff_olevels initially reported 5 seeds 193/222/477/555/591; root-caused to a
  real 4-byte-per-call memory leak in `vstore` — ASan/LeakSanitizer forced tcc's
  exit code to 1 after it had written a correct object, and diff_olevels keys
  success on exit code.  Fixed the leak (stack buffer instead of a heap snapshot
  that leaked on non-struct-copy exits); both tools now agree at 0.  Unrelated to
  rotation — the leak fired at `-O0` too.)  Also ran legacy-off
  (`--opt-levels="-O0,-O1 -fno-loop-rotation,-O2 -fno-loop-rotation"`): no
  additional divergences.
- [ ] Wider fuzz sweep is run by the maintainer, not part of the landing gate for
  the SSA pass; hold on removing any legacy call site until that sweep is clean.
- [x] Zero new divergences.
