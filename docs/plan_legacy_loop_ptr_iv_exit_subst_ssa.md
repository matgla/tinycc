# Plan: Replace `loop_ptr_iv_exit_subst` with `ssa:ptr_iv_exit_subst`

**Status:** implemented; legacy driver retired (2026-07-06) · **Created:** 2026-07-06

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents: [`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md)
(placement, detector-swap strategy, in-task retirement) and
[`plan_legacy_loop_dead_first_iter_ssa.md`](plan_legacy_loop_dead_first_iter_ssa.md)
(Step-0 gate, shared-helper extraction, cascade-parity handling).

## Intent

Replace the legacy pre-SSA pointer-IV exit-value substitution pass
(`tcc_ir_opt_loop_ptr_iv_exit_subst`) with a CFG/dominator-driven pass that:

- for a counted loop whose trip count N is statically known and > 0 (counter
  IV with constant init/limit/step), recognizes pointer induction variables —
  VAR vregs initialized in the preheader to `Addr[StackLoc[X]]` with a unique
  in-loop self-add `V = V + step` (direct or copy-through) — and rewrites
  **post-loop value-reads** of V to the closed-form exit value
  `Addr[StackLoc[X + step*N]]`;
- preserves the entry-guard side effect: a rotated loop's zero-trip guard
  (`CMP iv, #limit; JUMPIF past-loop` between the counter-IV init and the
  loop) is NOPed once trip>0 is proven, so the substitution walk is not
  blocked by the guard's stale `is_jump_target`;
- keeps the legacy conservatism: any shape outside the modeled pattern is
  declined and the IR left untouched.

The payoff is unchanged: idiomatic post-loop checks like
`if (p != &a[N]) abort();` (GCC pr49644 idiom) fold to a constant compare
that branch folding + DCE then kill.

Behavior intentionally **not** preserved / explicit non-goals for v1:

- No broadening. Only `tag=STACKOFF` VAR value-reads are substituted; deref
  uses (`*p`, `tag=VREG`) are never touched. `PTRIV_MAX` stays 8. Trip counts
  fold only for the conditions `compute_trip_count` understands (GE/UGE,
  GT/UGT, NE); LT/LE-exit loops keep declining. The 32-bit final-offset
  overflow guard stays.
- No dominance-based use rewriting in v1. The forward walk from `exit_target`
  with its merge-point / redef / backward-jump retirement stays semantically
  as-is; replacing it with dominator-scoped use replacement is a possible v2.
- No attempt to also migrate the *consumer* fold. `cmp_stack_addr_fold`
  remains a flat pass; whether it must be invoked at the new call site is a
  Step-0 question, not a rewrite target.

## Current Legacy Shape

- Call site: single site in `tccgen.c` (`tccgen.c:29987-30003` at time of
  writing), gated `tcc_state->opt_const_prop` only (set at `-O1+`, cleared by
  `-fno-const-prop`; no dedicated flag). On success (`> 0`) it runs a cleanup
  cascade: `cmp_stack_addr_fold` → `branch_folding` → `dce` (if `opt_dce`) →
  `compact_nops`. The call-site comment records the placement constraint:
  the pass must run **while pointer IVs are still VARs** — the later
  VAR→TEMP promotion/forwarding passes (`ir/opt_promote.c`) rename the IV
  and defeat the pattern match. This is the central migration risk (see
  Step 0 probe 3).
- Ordering drift already in the tree: legacy rotation and first-iter-exit,
  which historically ran immediately before this pass (phases 4c / 4c.5),
  have both been migrated to regalloc time and their `tccgen.c` call sites
  removed. The legacy site therefore now sees **un-rotated** loops, so its
  rotated-loop guard-NOP branch is likely dead there today (Step 0 probe 1
  measures this). The replacement, running after `ssa:loop_rotate`,
  **restores** the historic rotation → substitution order.
- Observability: **none**. No `dump_ir_after_pass` name, not registered for
  `TCC_DISABLE_PASS`, no `-f` flag, no bisect knob — isolation today means
  `-fno-const-prop`, which disables far more. The replacement fixes this.
- Source: `ir/opt_loop.c:1029-1415` — header comment with the pattern spec,
  `struct PtrIV` + `PTRIV_MAX`, local statics `ptr_iv_find_loop_step`,
  `ptr_iv_find_init`, `ptr_iv_unique_loop_def`, `ptr_iv_subst_uses_in_instr`,
  and the driver `tcc_ir_opt_loop_ptr_iv_exit_subst` (prototype
  `ir/opt.h:818`). Structure of the driver, per loop from
  `tcc_ir_detect_loops`:
  1. counter-IV anchor: `find_induction_vars_ex(..., allow_copy_through=1)` +
     `find_loop_exit_condition` (first IV with an exit condition wins) +
     `compute_trip_count`; require trip > 0;
  2. pointer-IV collection over the flat `[start_idx, end_idx]` range (skip
     counter IVs; unique in-loop def; preheader init `V = Addr[StackLoc[X]]`
     found by a backward walk from `preheader_idx` that stops at merges;
     `int32_t` overflow check on `X + step*N`);
  3. entry-guard NOP (only when ≥1 pointer IV found): find
     `CMP primary_iv, #limit; JUMPIF target≥end_idx` between the counter-IV
     init and `start_idx`, NOP both, then an **O(n²) function-wide rescan**
     to clear the target's `is_jump_target` if no other in-edge remains;
  4. forward walk from `exit_target`: substitute STACKOFF value-reads of each
     live pointer IV with `Addr[StackLoc[X + step*N]]`; retire an IV on
     redefinition; retire **all** IVs at any `is_jump_target` after
     `exit_target` or at any backward JUMP. Returns total substitutions.
- Shared helpers consumed (all remain shared, not legacy-owned):
  `find_induction_vars_ex`, `find_loop_exit_condition`, `compute_trip_count`
  (`ir/opt_loop_utils.c`, also used by `loop_const_sim` and guard-elim), and
  `tcc_ir_detect_loops`/`tcc_ir_free_loops` (`ir/licm.c`).
- Known-fragile spots (the parent tracker's designated high-risk areas):
  flat `[start_idx, end_idx]` body scans on legacy `IRLoop` ranges (a
  "self-add" matched inside the range but outside the actual loop, or on a
  conditional path in the body, would poison the closed form — nothing
  verifies the step executes once per iteration); the `is_jump_target`
  side-table mutation in step 3; `exit_target` semantics differing between
  top-tested (JUMPIF target) and rotated (fall-through) shapes inside
  `find_loop_exit_condition`; the copy-through look-back capped at 3
  instructions.
- Existing unit tests: 2 in `tests/unit/arm/armv8m/test_opt_loop.c`
  (`test_ptr_iv_exit_subst_substitutes_post_loop_use`, line 721 — asserts one
  substitution and the rewritten `Addr[StackLoc[52]]`/`is_lval==0` operand;
  `test_ptr_iv_exit_subst_deref_use_not_substituted`, line 762 — asserts the
  VREG-deref use is untouched).
- Existing IR regressions: **none dedicated**. The motivating pr49644.c is
  cited in the header comment but is not in the tree; coverage is unit-only
  plus indirect fuzz exposure. Closing this gap is part of the migration.
- Known fuzz regressions tied to this pass: none recorded in `docs/bugs.md`
  or the divergence memory index. Same caveat as first_iter: silence also
  means low fuzzer exposure. (The *consumer* `cmp_stack_addr_fold` has fuzz
  history — its merge-crossing resolver was the real culprit behind the
  derived-IV-SR bug #2 — which is a reason not to grow it during this
  migration.)

## Step 0: Liveness/Coverage Experiment (decision gate)

Run before writing any code; record the results matrix here. Four probes,
all on motivating C shapes (pr49644-style `p != &a[N]` post-loop check, plus
a runtime-trip control), compiled at `-O1`/`-O2`:

1. **Does the legacy pass still fire at its tccgen site at all**, now that
   loops arrive un-rotated there? Check via the pass return value
   (`LOG_LOOP_OPT` under `-DTCC_LOG_LOOP_OPT=1`) and the final disassembly
   (abort path present/absent). If it no longer fires on any realistic
   shape, the migration reduces to "prove inert, pin with tests, retire" —
   but the SSA replacement may still be worth landing to *restore* the
   optimization post-rotation (decide from probe 3).
2. **Does the SSA pipeline already produce the end effect** with the legacy
   call compiled out — is the abort path gone anyway? Expected **no**:
   nothing in `ssa:*` reasons about trip counts × pointer steps
   (`ssa:dead_loop` needs side-effect-free loops; `ssa:sccp`/`ssa:gvn`
   cannot prove `p == &a[N]` across a loop-carried phi).
3. **Does the pattern survive to regalloc time in walk-compatible form?**
   Dump the IR at `ssa:loop_rotate` time (`-dump-ir-passes=ssa:loop_rotate`)
   and check: is the pointer IV still a VAR with a preheader
   `V = Addr[StackLoc[X]]` init, or has `ir/opt_promote.c` (or late
   copy-prop) renamed it to TEMP form / forwarded the init? Also check the
   post-rotation shape: guard + bottom-tested exit, `exit_target` =
   fall-through. **This probe decides the whole design**: if promotion has
   destroyed the VAR encoding by regalloc time, v1 as specified cannot fire
   and the options are (i) extend the matcher to the promoted TEMP form
   (broadening — needs its own legality argument) or (ii) conclude the pass
   must keep a pre-promotion slot, i.e. migrate it in place (detector swap
   only, call site stays in `tccgen.c` order but becomes observable/named).
   Record which option the evidence picks.
4. **Cascade parity probe**: after a substitution at regalloc time, does the
   SSA pipeline fold the resulting `CMP Addr[K], Addr[K]` + JUMPIF
   (`ssa:sccp`/`ssa:fold`/`ssa:branch`), or must the flat
   `tcc_ir_opt_cmp_stack_addr_fold` be invoked at the new call site? Verify
   on final disassembly that no abort path survives.

Outcomes:

- **(a)** probes 1+2 show the behavior is already dead or already covered →
  skip the new pass; migration = regression tests + gated retirement of the
  inert call site.
- **(b)** gaps exist and probe 3 confirms walk-compatible IR at regalloc
  time → implement `ssa:ptr_iv_exit_subst` per the design below.
- **(c)** gaps exist but promotion defeats the pattern at regalloc time →
  fall back to the in-place migration variant from probe 3(ii) and revise
  the Placement section before implementing.

### Step 0 Results (2026-07-06, `bc1ea7e8` + this branch)

| probe | shape | -O1 | -O2 | notes |
|---|---|---|---|---|
| 1 legacy fires today | pr49644 idiom | NO | NO | abort path live in disasm; see root cause below |
| 2 SSA covers w/o legacy | pr49644 idiom | NO | NO | same disasm — nothing else folds `p != &b[N]` |
| 3 pattern at regalloc time | pr49644 idiom | YES | YES | V0/V1 still VARs; preheader `V0 <-- Addr[StackLoc[-256]]`; copy-through self-add; loop arrives **top-tested** (see below) |
| 4 CMP fold at SSA time | post-subst IR | pass-owned fold | pass-owned fold | neither SSA passes NOR the flat `cmp_stack_addr_fold` fold it: the `&a[N]` side is `T = LEA Addr[StackLoc[K]]` and the flat resolver's def-walk handles only ASSIGN/ADD/SUB — never LEA (checked back to the pass's introduction commit, so the legacy cascade never folded this shape either). Decision: the pass folds its own substituted CMP+JUMPIF consumers (`piv_fold_substituted_cmp`, region-scoped LEA/ASSIGN/ADD-chasing resolver, merge-free by the walk's own retirement rules); no `cmp_stack_addr_fold` call at the new site |

Probe-1 root cause (measured on the canonical `for (i=0;i<64;i++) *p++ = i;`
shape at -O2): the frontend lays the loop out split — header `CMP;JUMPIF` at
[2,3], forward `JMP` to the body at 4, increment+back-edge at [5,6], body at
[7,11] jumping back to 5.  The only dominance-real back-edge is `6 → 2`, so
the legacy flat range is `[start=2, end=6]`, which **excludes the body and
the pointer self-add at 8** → `n_pivs == 0` on every realistic shape.  The
pass only ever worked because legacy rotation (phase 4c) rewrote the loop
into a contiguous bottom-tested form first; with rotation migrated to
regalloc time, the tccgen site is inert.  `ssa:loop_rotate` also declines
this split-body layout, so the replacement pass must handle the top-tested
shape as well — which it does via CFG loop membership (see the design
amendment below).

**Outcome: (b)** — implement `ssa:ptr_iv_exit_subst`, with one design
amendment: the synthetic `IRLoop` must span **all member blocks**
(`end_idx = max member-block end_idx`), not `[header, latch]`, because the
natural loop's blocks may lie beyond the latch block in flat index order
(the canonical split layout above).  Member-block + dominates-latch checks
reject matches from non-member code interleaved in that span.

## SSA Replacement Design

(Assumes Step 0 outcome (b); under (c) the Placement subsection is replaced
by the in-place variant and the rest stands.)

### Placement: standalone flat-IR pass before SSA construction (v1)

Same placement and rationale as the two precedents: run in `ir/regalloc.c`
immediately **after** `ssa:first_iter_exit` (wiring precedent
`ir/regalloc.c:4539-4559`) and before `tcc_ir_cfg_build`/SSA construction.
The order rotate → first-iter → ptr-iv-exit-subst reproduces the historic
legacy phase order (4c → 4c.5 → this pass), and running after rotation means
the guard-NOP branch — dead at the current tccgen site — becomes meaningful
again exactly where it was designed to fire.

### Naming, files, observability

- Entry point: `int ssa_opt_ptr_iv_exit_subst(struct TCCIRState *ir)`,
  declared in `ir/opt/ssa_opt.h`, implemented in `ir/opt/ssa_opt_loop.c`
  (parent-tracker decision: loop-shape structural passes share this file).
- Invoke from `ir/regalloc.c` guarded by
  `tcc_ir_opt_pass_disabled("ssa:ptr_iv_exit_subst")` and followed by
  `tcc_ir_dump_after_pass(ir, "ssa:ptr_iv_exit_subst")` — observable via
  `-dump-ir-passes=ssa:ptr_iv_exit_subst`, isolable via
  `TCC_DISABLE_PASS=ssa:ptr_iv_exit_subst`. Both new vs legacy.
- Gate: `tcc_state->optimize >= 1 && tcc_state->opt_const_prop`, matching
  the legacy gate exactly (`-fno-const-prop` keeps its meaning for bisection
  sweeps).

### Candidate detection (the actual improvement)

Replace legacy `tcc_ir_detect_loops` flat ranges with dominance-verified
facts, following the `ssa_opt_first_iter_exit` pattern:

- Throwaway `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` per
  fixed-point pass; candidates are back-edges `latch → header` where the
  header dominates the latch; natural-loop membership via the shared
  backward walk (`fie_collect_members` — generalize/share it rather than
  copying); smallest-first ordering (`loop_size_cmp` semantics).
- **Explicit single-entry guard (new, stronger than legacy):** the header's
  predecessors must be exactly {one out-of-loop preheader predecessor,
  latch}, and no other member block may have an out-of-loop predecessor.
  Trip-count reasoning is entry-path-sensitive — a side entry that skips the
  counter-IV init or the pointer-IV init silently invalidates the closed
  form; legacy never checked this.
- **New step-legality check:** the pointer-IV self-add's block and the
  counter-IV increment's block must each be loop members that **dominate the
  latch** (execute exactly once per iteration). This closes the
  conditional-step / interleaved-non-loop-code hole of the flat range scan
  at near-zero cost, using facts the CFG already provides.
- Synthetic `IRLoop` construction for the shared helpers: `header_idx` =
  `start_idx` = header block's `start_idx`, `end_idx` = latch block's
  `end_idx`, `preheader_idx` derived by replicating the legacy walk (first
  non-JUMP/JUMPIF instruction walking back from `header_idx - 1`,
  `ir/licm.c:199-212`) so `find_induction_vars_ex` and `ptr_iv_find_init`
  behave identically. `body_instrs` stays NULL — nothing in the reused core
  reads it.

### Shared core: reuse, don't rewrite

Extract the legacy driver's per-loop body (`ir/opt_loop.c:1236-1410` — the
counter-IV anchor, pointer-IV collection, guard NOP, and forward
substitution walk) into a shared
`int ptr_iv_exit_subst_loop(TCCIRState *ir, IRLoop *loop)` (final name per
implementer) — the `ld_first_iter_prove` precedent: that walk was exported
via `ir/opt_loop_utils.h` while legacy and SSA drivers coexisted, then moved
wholesale into `ir/opt/ssa_opt_loop.c` (now static there, together with the
rest of the first_iter analysis) once the legacy driver was deleted. Since
this migration retires the legacy driver in the same task, move the core and
the `ptr_iv_*` statics directly into `ir/opt/ssa_opt_loop.c` and, for the
validation window only, declare the core in `ir/opt_loop_utils.h` so the
thin legacy `tcc_ir_detect_loops` iterator can call it; drop that
declaration at retirement. Do not copy-paste the ~180-line core.

### Mutation strategy and CFG staleness

- Operand substitutions rewrite `src1`/`src2` in place — no control-flow
  change, no index shift, CFG stays valid.
- The guard NOP **does** change control flow (a JUMPIF disappears). Rule:
  per CFG build, process candidates smallest-first and stop after the first
  candidate that NOPed a guard; the outer fixed point (cap 4, mirroring
  `SSA_FIRST_ITER_EXIT_MAX_PASSES`) rebuilds and continues. Candidates that
  only substitute (no guard found) do not stop the sweep.
- Change count = substitutions + control-flow changes (guard NOPs and folded
  consumer JUMPIFs), so a guard-only firing is visible to dumps and the fixed
  point. (Legacy counted substitutions only; the return value only gated its
  cascade.)
- Keep the legacy guard-NOP semantics verbatim inside the shared core,
  including the other-in-edge rescan before clearing `is_jump_target`. A
  CFG-side replacement of that rescan is v2 material.
- Idempotency: on the pass's own output the guard is already NOPed and no
  STACKOFF value-reads of V remain after the walk's substitutions, so a
  re-run changes nothing.
- Coexistence: with the legacy site enabled, post-loop uses are already
  substituted by tccgen time, so the SSA pass performs 0 substitutions —
  but it may still legitimately NOP rotation guards the legacy site never
  saw (rotation now happens after tccgen). Coexistence is therefore
  **sound but not necessarily silent**, unlike the two precedents; the
  validation gate runs with both enabled regardless.

### Cleanup cascade

Decided by Step 0 probe 4 — **outcome: pass-owned consumer fold, no external
cascade** (maintainer decision 2026-07-06: fix the fold in the new
implementation rather than growing the legacy consumer):

- Neither the SSA pipeline nor the flat `tcc_ir_opt_cmp_stack_addr_fold`
  folds the substituted compare: the `&a[N]` operand is an LEA-defined temp
  (`T = LEA Addr[StackLoc[K]]; CMP V, T`) and the flat resolver's def-walk
  handles only ASSIGN/ADD/SUB — never LEA (true back to the pass's
  introduction commit, so the legacy cascade never actually folded the
  motivating shape either; the legacy pass+cascade were doubly inert).
- The pass therefore folds its own consumers: when the walk substitutes into
  a CMP, `piv_fold_substituted_cmp` resolves BOTH operands to frame offsets
  with a region-scoped resolver (`piv_resolve_frame_addr`: direct operands,
  LEA/ASSIGN/ADD±imm chains, chased only within `[exit_target, cmp]`, which
  the walk's merge-retirement rules keep straight-line) and, on proven-equal
  operands, precomputes the following JUMPIF (→ JUMP or NOP pair).
  Proven-unequal compares are conservatively left alone.
- A folded JUMPIF is a control-flow change: it is counted in the pass's
  change total and stops the per-CFG sweep like a guard NOP.
- DCE and NOP compaction are left to `ssa:dce`; the legacy `branch_folding`
  / `dce` / `compact_nops` tail is not replicated.  `cmp_stack_addr_fold`
  keeps its second caller (`tccgen.c:30456`), so retirement does not orphan
  it.
- Parity (checked on final disassembly of the motivating shape): abort path
  dead at -O1/-O2 post-retirement — an *improvement* over the pre-migration
  tree, where the inert legacy pass left it live.

### Interaction with other passes

- `ssa:loop_rotate` runs first and produces exactly the guarded bottom-tested
  shape whose guard this pass NOPs; `ssa:first_iter_exit` handles loops that
  provably never run (trip == 0 side of the spectrum); this pass handles
  loops that provably run N > 0 times. No shared shapes.
- The still-legacy tccgen passes (`loop_const_sim`, `loop_unroll`, LICM,
  IV-SR) run **before** the new pass and after the legacy site's removal
  will see un-substituted post-loop uses. Expected benign — substitution
  only affects post-loop code they don't key on — but `loop_unroll` fully
  unrolling a small counted loop *removes* the loop before the new pass sees
  it; the IR regression test uses a trip count large enough to survive
  unrolling, and the gates verify the rest.

## Required Unit Tests

In `tests/unit/arm/armv8m/test_opt_loop.c`, alongside the existing legacy
`test_ptr_iv_exit_subst_*` pair, calling `ssa_opt_ptr_iv_exit_subst` (flat-IR
`utb_*` builders; no ctx needed). Assert change counts **and** resulting
operands/ops:

- ports of both legacy tests (post-loop value-read substituted with the
  correct `Addr[StackLoc[52]]`/`is_lval==0` operand; deref use untouched);
- declines: no counter IV; exit condition `compute_trip_count` cannot fold
  (LT); trip count ≤ 0; pointer IV with a second in-loop def; init not a
  stack address; final-offset `int32_t` overflow; candidate vreg that *is*
  the counter IV;
- walk behavior: substitution stops at a redef of V; all IVs retire at an
  `is_jump_target` merge after `exit_target`; all IVs retire at a backward
  JUMP;
- guard handling: rotated-shape guard NOPed and target's `is_jump_target`
  cleared; guard target with another in-edge keeps `is_jump_target`;
- **new** single-entry guard: side entry into the loop body declines;
- **new** step legality: pointer-IV self-add on a conditional path in the
  body (block not dominating the latch) declines;
- idempotency: second run on own output returns 0;
- inert-on-legacy-output: running the SSA pass on IR the legacy pass already
  processed performs no further substitutions.

## IR Regression Test

`tests/ir_tests/345_ptr_iv_exit_subst.c` + `.expect` (next free number),
registered in `TEST_FILES` in `tests/ir_tests/test_qemu.py`:

- pr49644-style shape: pointer walks an array in a counted loop with a
  compile-time trip count **large enough to defeat full unrolling**, then
  `if (p != &a[N]) abort();` — must print the success path;
- a runtime-trip control loop with the same post-loop check that must NOT be
  folded (and still behave correctly);
- added **first**, green under the legacy pass, to pin behavior across the
  migration.

## Migration Steps

Per the maintainer's decision this migration **includes retirement in the
same task** (rotation precedent), gated on the local acceptance runs; the
wider fuzz sweep remains maintainer-run.

- [x] Run Step 0; fill in the results matrix; pick outcome (a)/(b)/(c).
  Outcome (b) with the member-span amendment (see Step 0 Results).
- [x] Add IR regression test `345_ptr_iv_exit_subst.c` (green at
  -O0/-O1/-O2 before any pass change; pins runtime behavior — the legacy
  pass was already inert so it could not pin the fold).
- [x] Extract the shared per-loop core `ptr_iv_exit_subst_loop` out of the
  legacy driver; legacy behavior unchanged (existing 2 unit tests green).
- [x] Add the new unit tests (list above; plus consumer-fold tests:
  taken/not-taken/unequal-left-alone, and the split-body canonical layout).
- [x] Implement `ssa_opt_ptr_iv_exit_subst` in `ir/opt/ssa_opt_loop.c`;
  declare in `ir/opt/ssa_opt.h`; share (don't duplicate) the loop-membership
  helper with `ssa_opt_first_iter_exit` (`fie_collect_members`).
- [x] Wire from `ir/regalloc.c` after `ssa:first_iter_exit`, gated
  `optimize >= 1 && opt_const_prop`, wrapped in `tcc_ir_opt_pass_disabled` +
  `tcc_ir_dump_after_pass`; cleanup is pass-owned (see Cleanup cascade) — no
  external calls at the site.
- [x] Confirm observability (`-dump-ir-passes=ssa:ptr_iv_exit_subst`) and
  disable knob (`TCC_DISABLE_PASS=ssa:ptr_iv_exit_subst` restores the abort
  branch on the probe).
- [x] Local gates with legacy still enabled: unit tests (2848+157, 0
  failed), `make cross`, `make test -j16` (13467 passed incl. GCC torture
  compile), `345_ptr_iv_exit_subst` at -O0/-O1/-O2/-Os.
- [x] **Retirement (same task):** `tccgen.c` call site + cascade removed
  (pointer comment left in the established format); thin legacy driver
  `tcc_ir_opt_loop_ptr_iv_exit_subst` and its `ir/opt.h` prototype deleted;
  the 2 legacy unit tests ported to the SSA entry point and legacy
  registrations deleted (the temporary inert-on-legacy-output test retired
  with the driver); core + `ptr_iv_*` statics now static in
  `ir/opt/ssa_opt_loop.c` (validation-window declaration dropped from
  `ir/opt_loop_utils.h`); `tests/unit/PASS_COVERAGE.md` updated.
- [x] Re-run the full local gate after retirement (2845+157 unit tests, 0
  failed; `make test` 13467 passed) + the cascade-parity disassembly check
  (abort path dead at -O1/-O2, present at -O0).
- [x] Tick the parent tracker's `loop_ptr_iv_exit_subst` entry (this
  document was already in its "Detailed Pass Plans" list).

## Acceptance

For landing (pre- and post-retirement runs both required):

- [x] New unit tests pass; legacy pair passes (pre-retirement) / ported pair
  passes (post-retirement).
- [x] `345_ptr_iv_exit_subst.c` passes at `-O0/-O1/-O2` (and `-Os`).
- [x] `make cross -j$(nproc)`; `make test -j16`; GCC torture suite
  (13467 passed / 0 failed, both pre- and post-retirement).
- [x] Cascade parity: abort path dead in the disassembly of the motivating
  shape at `-O1`/`-O2` post-retirement (an improvement — the inert legacy
  pass left it live); present at `-O0` as expected.
- [ ] **Maintainer-run** (per 2026-07-06 instruction, after implementation):
  `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0 divergent
  profiles; `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu`
  → 0 divergences; plus an equivalence sweep with
  `TCC_DISABLE_PASS=ssa:ptr_iv_exit_subst`.
- [ ] Zero new divergences; any divergence triggers fixes before the branch
  merges.

## Assumptions

- `ssa:loop_rotate` and `ssa:first_iter_exit` keep their current
  `ir/regalloc.c` positions; this pass slots directly after them and moves
  with them if they move.
- Step 0 probe 3 confirms VAR-encoded pointer IVs survive to regalloc time;
  otherwise the Placement section is revised per outcome (c) before any
  implementation.
- `find_induction_vars_ex` / `find_loop_exit_condition` /
  `compute_trip_count` remain shared utilities (also consumed by
  `loop_const_sim` and guard-elim) and are not deleted with the legacy
  driver.
- The retirement of the two upstream legacy passes (rotation, first-iter)
  is not reverted; if first_iter's in-tree state changes, only the wiring
  neighborhood in `ir/regalloc.c` shifts.
