# Plan: Post-RA consolidation — shrink and de-ifologize the post-RA stage

**Status:** planned ·
**Created:** 2026-07-08

Sibling precedent: [`plan_legacy_flat_ir_ssa_retire.md`](plan_legacy_flat_ir_ssa_retire.md)
(per-step gate = `make test` + corpus size delta, `TCC_DISABLE_PASS` knobs for
bisection, observability-before-surgery as proven by the `loop_bound_remat`
retirement).

## Question this plan answers

Can the SSA passes be made generic enough to replace the post-RA passes — or
could post-RA disappear entirely given better SSA optimization?

**No to both, but.** SSA passes depend on the single-def invariant and DU
chains; after `tcc_ir_ssa_regalloc` a physical register has many defs and the
phis are gone, so SSA passes structurally cannot run there. And the garbage
post-RA cleans up (`mov R,R` residuals, orphaned flag-setters, fall-through
jumps from diamonds whose phi copies became identities) *does not exist before
RA* — RA creates it, so no pre-RA pass can remove it. Every production
compiler keeps a post-RA stage for exactly this (LLVM
MachineCopyPropagation/BranchFolder, GCC cprop_hardreg/peephole2).

What CAN change: roughly half of today's post-RA work doesn't need to be
post-RA, and the half that stays is complex for structural reasons that are
fixable, not inherent. Target end state: a small principled post-RA core
(branch cleanup, `mov R,R` deletion, frame/spill compaction, codegen
peepholes) running as a declared pass group over a shared analysis context,
instead of ~500 lines of inline blocks in `tccgen.c` plus hand-rolled scans in
every pass.

## Inventory: what runs after `tcc_ir_ssa_regalloc` today

Sequence in `tccgen.c` (`gfunc_epilogue`-adjacent region, ~30666–31100):

| # | step | where | size | category |
|---|------|-------|------|----------|
| 1 | `ra:backedge_phi_hoist` | `ir/opt_promote.c:1684` | ~240 L | keep (reads LS intervals) |
| 2 | `tcc_ir_opt_post_ra_forward_diamond` | `ir/opt_promote.c:1924` | ~163 L | shrink (downstream of weak coalescing) |
| 3 | `tcc_ir_opt_abort_tail_merge` | `ir/opt_promote.c:2207` | ~136 L | keep post-RA (see below), move to pass table |
| 4 | jump-thread / elim-fallthru / jumpif-invert / orphan-cmp / DCE fixpoint | inline `tccgen.c` | ~20 L driver | keep; already shares engines with `ssa:cfg_cleanup` (`ir/regalloc.c:4611`) |
| 5 | stack re-compaction scan #1 | inline `tccgen.c` | ~30 L | dedupe into helper |
| 6 | return-register swap heuristic | inline `tccgen.c` | ~120 L | **delete** — becomes an RA hint |
| 7 | `tcc_ir_avoid_spilling_stack_passed_params` + frame-shrink scan #2 + `tcc_ls_compact_stack_locations` + spill-slot liveness scan | inline `tccgen.c` | ~180 L | keep; dedupe scans |
| 8 | `tcc_ir_move_coalescing` | `ir/regalloc.c:5052` | ~342 L | shrink toward identity-`mov` deletion |
| 9 | frame-shrink scan #3 (post-coalesce) | inline `tccgen.c` | ~60 L | dedupe into helper |
| 10 | codegen-time peepholes (STRD pairing, MLA fusion, in-place increment coalescing, …) | `ir/codegen.c`, `arm-thumb-gen.c` | — | keep; port to shared liveness queries |

Categories: **keep** = genuinely needs physical registers / LS results;
**shrink** = exists only because an earlier mechanism is incomplete;
**delete** = compensates for a missing RA capability;
**dedupe** = three nearly identical STACKOFF frame-shrink scans.

## Root causes of the ifology

1. **No shared analysis post-RA.** Pre-RA passes get `IROptCtx` (DU chains,
   block starts, loops) and the `IRPassGroup` driver (`ir/opt_pipeline.c`).
   Post-RA passes get nothing: each hand-rolls reachability walks, "is this
   physical reg live here", "does anything read flags before the next
   clobber", "is this a jump target". Most of the conditionals in these
   passes are exactly these hand-rolled safety checks. The underlying data
   already exists (`ls.live_regs_by_instruction`, LS intervals); it is just
   not packaged as a queryable context.

2. **Index-keyed state forbids `compact_nops`.** The standing comment at the
   post-RA cleanup loop (`tccgen.c` ~30696) forbids compaction because
   renumbering perturbs what downstream peepholes key off. Three distinct
   index spaces are in play:
   - `orig_index`-keyed side tables (`barrel_shifts`, `bfi_params`,
     `shift64_dead_half`) — these are compaction-SAFE (`orig_index` is a
     per-quad field, not an array position; see
     [`side_table_orig_index_bounds.md`](side_table_orig_index_bounds.md)).
   - LS interval `start`/`end` and `live_regs_by_instruction` — array-position
     keyed; compaction would need a remap.
   - Peepholes keying on raw adjacency/array indices (in-place increment
     coalescing et al.).
   Because compaction is forbidden, every post-RA pass must skip NOP runs and
   tolerate holes — a whole class of edge cases exists only because of this.

3. **The same problem is solved in several places, each incompletely.**
   Copy coalescing exists three times: RA assign hints, the graph-based
   pre-scan coalescer (`ir/regalloc.c` Stage 1–4), and post-RA
   `tcc_ir_move_coalescing`. Diamond cleanup exists twice: `ssa:cfg_cleanup`
   and `post_ra_forward_diamond`. Each late duplicate exists because the
   earlier mechanism doesn't finish the job; each is a separate bug surface.

The bug history concentrates here — STRD fuse across jump target (test 251),
move-coalesce bitmap orphan (test 292), reverse-coalesce dest-redef (test
293), expire-coalesced-reg-share (test 342), ASSIGN-STRD deref-src (test 296)
— which is both the argument for this cleanup and the reason every step gets
the full gate.

## Non-goals

- No new optimizations. Byte-for-byte output parity is the target for every
  phase except 4 and 5 (which must be net-non-negative on the corpus delta).
- No SSA reconstruction post-RA (a reaching-def dataflow over physical regs is
  as far as any future need should go, and nothing in this plan needs it).
- Codegen peepholes in `ir/codegen.c` / `arm-thumb-gen.c` stay where they are;
  they only gain shared queries (phase 2).

## Phases

Per-step gate, all phases: `make test -j16` (+ `make test-asm`), corpus size
delta via `metrics/compare_worktree.py`, and a `TCC_DISABLE_PASS=ra:<name>`
knob per extracted pass so fuzz bisection (`scripts/bisect_opt.py` /
`TCC_DISABLE_PASS` sweeps) can isolate it later. Fuzz sweeps deferred to the
end as with the SSA retirement (expensive; user-run).

### Phase 0 — observability (no behavior change)

- Firing counters per post-RA step (same v1a method that proved
  `loop_bound_remat` inert): count `move_coalescing` fwd/rev erasures,
  `forward_diamond` inversions, return-reg swaps, abort merges, cleanup-loop
  iterations, per corpus build. Log under a `TCC_LOG_` scope (reuse
  `TCC_LOG_LS`).
- Baseline numbers recorded in this doc before any surgery, so phases 4–5 can
  show the duplicates' firing counts dropping instead of guessing.

### Phase 1 — mechanical extraction (parity)

- New `ir/opt_postra.c` (or extend `opt_promote.c`): move inline `tccgen.c`
  blocks #4, #5, #6, #7, #9 behind `tcc_ir_opt_*` functions.
- Single `tcc_ir_frame_shrink_scan(ir, &loc, func_var)` helper replacing the
  three near-identical STACKOFF scans (#5, #7-part, #9). They differ only in
  the spilled-vreg offset translation; parameterize that.
- Declare the whole sequence as a post-RA `IRPassGroup` run through
  `tcc_ir_opt_run_group` with `ra:*` names, `tcc_ir_dump_after_pass` after
  each, and per-pass disable knobs. `tccgen.c` shrinks to one call.
- Gate: identical corpus binaries (delta = 0).

### Phase 2 — shared post-RA context

- Add a small `IRPostRACtx`: flat-IR CFG (reuse `ir/cfg.c` — it already runs
  on flat IR at `ssa:loop_rotate` time), plus queries backed by existing LS
  state: `postra_reg_live_at(ir, reg, idx)` (from
  `live_regs_by_instruction`), `postra_flags_read_before_clobber(ir, idx)`
  (the orphan-cmp scan, exported), `postra_is_jump_target(ir, idx)`
  (`is_jump_target` flag, already maintained).
- Port passes #1–#3, #8 and the riskiest codegen peepholes (STRD pairing,
  increment coalescing) to these queries, deleting their private scans. This
  is where the ifology dies; each port is its own gated step.
- Gate per pass: delta = 0.

### Phase 3 — make post-RA compaction legal (enabler, optional but high-value)

- Audit every array-position-keyed consumer past RA: LS interval
  `start`/`end`, `live_regs_by_instruction`, peephole adjacency assumptions.
- Either (a) add an index remap to `tcc_ir_opt_compact_nops` that rewrites LS
  intervals + the liveness table when called post-RA, or (b) keep the
  no-compact rule but make it enforceable: a debug assert that nothing
  compacts between RA and codegen, so the constraint is checked rather than
  folklore.
- (a) removes the NOP-hole tolerance from every pass ported in phase 2;
  decide (a) vs (b) after measuring how much phase-2 code the NOP-skipping
  actually costs. `orig_index` side tables need no change (already
  position-independent).

### Phase 4 — move work out of post-RA

- **Return-reg swap → RA hint (delete #6).** `IRLiveInterval.incoming_reg0`
  already records the wanted register; honor it in `ra_linear_scan`'s
  free-register tie-break for VAR intervals (mirroring `ra:narrow_pref`,
  which set the precedent for preference-order tweaks). Then delete the
  120-line post-hoc swap + atomic bitmap patch. Gate: delta ≤ 0 and the
  phase-0 swap counter at zero.
- **`abort_tail_merge` placement experiment.** Its header comment argues for
  post-RA (argc==0 callee ⇒ no per-site arg setup; sink operands physical).
  Counter-argument: pre-RA placement would let `ssa:cfg_cleanup` do the NOP
  tidying and shorten live ranges before allocation. A/B it like the reroll
  Step-0 experiment; keep whichever wins on the corpus, but in the pass table
  either way. No presumption.
- **`post_ra_forward_diamond`** is not moved directly — it shrinks as a side
  effect of phase 5 (it exists to catch diamonds whose phi copies became
  identity moves). Re-measure its firing count after phase 5; if ~0, retire
  it through the standard inertness gate.

### Phase 5 — coalescing consolidation

Three mechanisms → aim for two, with the post-RA one trivial:

- Strengthen in-RA coalescing where phase-0 counters show `move_coalescing`
  firing: known gaps are the phi-resolution copies emitted "for every operand"
  (`ir/regalloc.c:3081` pre-RA path) that allocation then makes identity, and
  ASSIGN-chains the graph coalescer's pressure gate skips.
- Do NOT try to make RA-time coalescing complete — the graph coalescer's
  pressure gate exists for a reason (regressions when coalescing raises
  pressure). The goal is only to shrink `move_coalescing`'s job toward
  "delete `mov R,R`; nothing else", at which point its 342 lines (interval
  surgery, bitmap maintenance, fwd/rev dest-redef guards — the site of tests
  292/293) collapse to a ~30-line sweep.
- Each strengthening step: delta ≤ 0, all IR tests, and the phi-copy identity
  counter trending down.

## Retirement criteria

A post-RA pass is retired only via the standard inertness gate: phase-0
counter at zero across the full corpus + torture suite, then removal with
delta = 0, knob kept one release for bisection (the `loop_guard_elim` /
`loop_bound_remat` procedure).

## Risks

- This region owns a disproportionate share of historical miscompiles; parity
  phases (1–2) must not be batched — one pass per commit, full gate each.
- Phase 3(a) touches the LS-interval index space that codegen
  (`scratch_push_sp_bias`, spill addressing) reads; if the remap audit finds
  more consumers than listed, fall back to 3(b) — the plan does not depend
  on 3(a).
- Phase 5 interacts with `loop_phi_locked` / pair-allocation constraints
  (test 301) and the survivor-mask expire fix (test 342); coalescing changes
  must re-run those pinned tests explicitly.
