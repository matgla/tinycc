# Plan: Replace `dead_loop_elim` with the SSA dead-loop pass

**Status:** proposed pass plan · **Created:** 2026-07-07

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents:
[`plan_legacy_loop_const_sim_ssa.md`](plan_legacy_loop_const_sim_ssa.md)
(Step 0 decision gate, shared-engine retention, coexistence/idempotency
contract) and
[`plan_legacy_loop_postinc_fusion_ssa.md`](plan_legacy_loop_postinc_fusion_ssa.md)
(the "prove-unnecessary-and-retire" outcome, kept here as a live possibility).

Unlike the other loop passes, the SSA-side replacement **already exists and
ships**: `ssa_opt_dead_loop` (`ir/opt/ssa_opt_dead_loop.c`, dump name
`ssa:dead_loop`) has run in the `-O2` SSA fixed point since the -O1/-O2 split
(2026-07-05, see the parent tracker's
[o1-o2-split-and-dead-loop-gating] rationale). This migration is therefore
**not** "port a legacy pass to SSA"; it is "prove the shipping SSA pass covers
(or is deliberately narrower than) the legacy flat pass, then delete the legacy
one." The bulk of the work is a coverage-gap experiment (Step 0), not new
transform code.

## Intent

Retire the legacy pre-SSA `tcc_ir_opt_dead_loop_elim` (`ir/opt_dce.c`) and its
two `tccgen.c` call sites, keeping `ssa:dead_loop` as the single owner of
dead-loop collapse. Preserve the observable end-to-end behavior:

- collapse a side-effect-free counting loop whose post-loop result is a
  loop-invariant constant into that constant (proven-trip path) or into a
  guarded `(trip>0)?body_const:init` SELECT (runtime-bound path);
- keep this an **-O2-only** optimization (both passes are already gated
  `optimize >= 2`; GCC likewise keeps the empty loop at -O1 and elides it only
  at -O2 — demoting it fails the benchmark-cycle expectations recorded in the
  o1-o2 split);
- preserve the control-flow correctness the legacy pass earned the hard way:
  a loop that is the then-arm of an `if` must retain an explicit exit edge, not
  fall through into the else block (switch fuzz seed 198468, tests 317/318).

Behavior intentionally **not** preserved / non-goals:

- No pre-SSA flat-range mutation. The legacy pass NOPs `[start_idx, end_idx]`
  in place and rewrites operand pools by hand; both its known miscompiles are
  flat-mutation artifacts (see *Known fuzz history*). The SSA pass does the
  same collapse over SSA facts and is structurally immune to both (below).
- No unconditional preheader hoist of the body constant. The legacy pass moves
  `V <- #c` into the preheader with **no trip-count proof**; that is unsound
  when the loop may run zero times and `V`'s incoming value is still live on
  the skip path. `ssa:dead_loop` already does the correct thing: constant only
  on the proven-trip path, guarded SELECT otherwise (test 328).
- No new transform surface beyond closing whatever real gap Step 0 finds.

## What the two passes actually cover (the crux of this migration)

Both passes recognize the same *source-level* shape — "a loop whose body has no
observable effect and whose result is a constant" — but they match it on
different IR, so their coverage only partially overlaps.

`ssa:dead_loop` (`ir/opt/ssa_opt_dead_loop.c`) matches **SSA-promoted scalars**:
a header phi `T_o_phi = phi(init, latch_const)` whose latch operand resolves to
a constant and which has no in-loop use. It rewrites post-loop uses to the
constant (`rewrite_loop_exit_phis`) or materializes a guarded SELECT
(`rewrite_loop_exit_phis_guarded`), then kills the body by converting the header
`JUMPIF` into an explicit `JUMP exit_target` and NOPing the body
(`try_kill_loop_body`). Purity is `ssa_opt_has_side_effects(op)` over the body —
**any `STORE` is a side effect and vetoes the loop.**

Legacy `tcc_ir_opt_dead_loop_elim` (`ir/opt_dce.c:5558`) matches **memory-
resident locals and no-op memory**:

1. body `ASSIGN VAR <- #imm` (a write to a stack local that is *not* SSA-
   promoted — address-taken, complex, or otherwise memory-bound), tracked in
   `const_vars[8]` and hoisted to the preheader;
2. **self-stores** `*p = *p` (`T=*p; *p=T;`) — an observable no-op the alias-
   conservative DSE won't kill; recognized by walking back to the matching
   `LOAD` (`has_self_stores`);
3. a dead counter (`has_loop_counter`) as the qualifying "this really is a
   loop" signal.

It then guards heavily against the loop detector's false positives — a value
leaking out of the range (`leaks_value`), a side entry into the middle of the
range (`side_entry`, incl. `SWITCH_TABLE` targets), a second data-dependent
exit (`body_jumpif > 1 || body_break`), and the missing-exit-edge case
(`need_exit_jump`).

The distinctive legacy coverage is therefore **(1) memory-VAR idempotent bodies
and (2) self-stores** — neither reaches an SSA phi, so `ssa:dead_loop`'s phi
matcher and `STORE`-is-side-effect purity rule both skip them today. Whether
that coverage still *matters* after the recently-landed SSA loop passes run is
exactly the Step 0 question: `ssa:loop_const_sim` (register-only bounded
simulation) and `ssa:loop_unroll` now run pre-SSA-build in `ir/regalloc.c`, and
DSE/`dead_var_store_elim` may already retire the VAR stores or self-stores
before either dead-loop pass looks.

Two correctness properties the SSA pass has for free — both are reasons to
prefer it, and both must be re-confirmed to still hold on the ported shapes:

- **No dropped exit edge.** `try_kill_loop_body` writes `JUMP exit_target`
  unconditionally (`ir/opt/ssa_opt_dead_loop.c:584`), never relying on
  fall-through, so the 317/318 miscompile class is *structurally impossible* in
  the SSA path — the legacy pass needed its bespoke `need_exit_jump` scan to
  reach the same safety.
- **No operand-pool overflow.** The SSA pass allocates fresh vregs/operands
  (`tcc_ir_vreg_alloc_temp`, `tcc_ir_iroperand_pool_add`); the legacy
  `bug_dead_loop_assign_overlap` class (reusing a NOP slot's stale
  `operand_base` and overflowing into the next instruction) cannot occur.

## Current Legacy Shape

- **Entry points:** `tcc_ir_opt_dead_loop_elim(TCCIRState*)`
  (`ir/opt_dce.c:5549`, real body `__timed` at `:5558`) and the unused
  `tcc_ir_opt_dead_loop_elim_ex(IROptCtx*)` wrapper (`ir/opt_dce.c:6023`).
  Declared `ir/opt.h:806` and `:679`.
- **Call sites — two, both in `tccgen.c`:**
  - `tccgen.c:30493` (Phase-tail "Dead Loop Elimination"), guarded
    `tcc_state->opt_dce`, fired only when `optimize >= 2`. On change it runs a
    cleanup cascade: `value_tracking → const_prop_tmp → branch_folding → dce →
    dse → compact_nops`.
  - `tccgen.c:30525` (re-run after the final DCE, to catch loops whose bodies
    were only NOPed by inline-struct-copy DSE on the first pass), same gate;
    cascade `branch_folding → dce → compact_nops`.
- **Gate:** `tcc_state->opt_dce` **and** an explicit `optimize >= 2` check at
  each call site. No dedicated `-f` flag. `-fno-dce` disables it as a side
  effect (shared knob — `dead_loop_elim` bugs currently mis-attribute to
  `dce`).
- **Observability:** `dump_ir_after_pass(..., "ZZ2_dle1")` under
  `CONFIG_TCC_DEBUG` only, emitted once after the first call site. **Not**
  registered with `tcc_ir_opt_pass_disabled`, so `TCC_DISABLE_PASS` cannot
  isolate it (confirmed; the o1-o2 memory notes this too). The SSA replacement
  already fixes both — `ssa:dead_loop` has a stable dump name and a working
  `TCC_DISABLE_PASS` hook.
- **Not in `ir/opt_pipeline.c`** — the declarative tables don't list it; the
  call sites are hand-written in `tccgen.c` (verified: `grep dead_loop
  ir/opt_pipeline.c` → none).
- **Source:** `ir/opt_dce.c:5558-6021` (~460 lines) — self-contained; uses
  `tcc_ir_detect_loops` flat ranges, `write_instr_at_nop`, and direct
  `operand_base` rewrites.
- **Existing tests:**
  - IR regressions (`TEST_FILES`, `tests/ir_tests/test_qemu.py`):
    `318_fuzz_dead_loop_elim_missing_exit_jump.c` (this pass, dropped exit
    edge) and `bug_dead_loop_assign_overlap.c` (operand-pool overflow).
    `317_fuzz_loop_elim_missing_exit_jump.c` is the **same bug class in a
    different pass** (`try_eliminate_loop`, the unroll util) — it pins the
    sibling, not this pass, and must stay green regardless.
  - Unit: `tests/unit/arm/armv8m/test_opt_dead_store.c` links the real symbol
    and has a dedicated block (`UT_COVERS("dead_loop_elim")`):
    `test_dead_loop_elim_const_assign_loop_removed` (the VAR-const-hoist
    positive) and `test_dead_loop_elim_ex_empty`.
- **Known fuzz history (all flat-mutation artifacts):**
  - switch seed 198468 (O1 wrong-code): NOPing the whole range dropped the
    forward exit branch of a loop-in-then-arm; control fell into the else
    ([loop-elim-missing-exit-jump-class], tests 317/318). Fixed by
    `need_exit_jump`. **The SSA path never had this bug.**
  - `bug_dead_loop_assign_overlap` (HardFault at codegen): const-assign written
    over a NOP slot with a stale `operand_base` overflowed into the next
    instruction. Fixed by fresh-slot allocation. **The SSA path never had
    this bug.**

## Step 0: Coverage-Gap Experiment (decision gate)

Determine what, if anything, legacy `dead_loop_elim` still eliminates that is
not already handled — now that `ssa:loop_const_sim`, `ssa:loop_unroll`, DSE, and
`ssa:dead_loop` all run. Do this **before** writing any transform code.

**Method** (local, uncommitted): disable both `tccgen.c` call sites (return 0)
and rebuild `armv8m-tcc`. Then, at `-O2` only:

1. `make test -j16` and the GCC torture suite — record every regression.
2. For each of the two legacy unit shapes and the two legacy IR regressions
   (318, `bug_dead_loop_assign_overlap`), plus the SSA-side 327/328 and the
   benchmark loops named in the o1-o2 split (`bench_conditionals`, `switch`,
   `strcmp`), diff the final `-O2` disassembly against baseline and attribute
   the collapsing pass via `-dump-ir-passes` diffs. Classify each loop:
   - collapses identically under a *different* pass (`ssa:dead_loop`,
     `ssa:loop_const_sim`, `ssa:loop_unroll`, or DSE+DCE) → **subsumed**;
   - survives (loop still present, or residual dead stores / self-stores
     remain) → **gap**, and record the exact IR shape.
3. Specifically probe the two distinctive legacy domains, since these are where
   a gap is expected:
   - **memory-VAR idempotent body**: a loop writing `#c` to an address-taken
     or complex local each iteration (no SSA phi) — does DSE kill the store, or
     does the loop survive?
   - **self-store `*p=*p`**: an inlined-struct-copy shape (the CPOW/CCID case
     the legacy comment cites) — does `ssa:dse`/`dead_var_store_elim` remove it,
     or does the empty loop survive?

**Outcomes:**

- **(a) fully subsumed** — no `make test`/torture regression, every probed loop
  collapses under another pass with acceptable code size. Then this is a
  *retirement*, like `loop_postinc_fusion`: delete the two call sites, the
  driver, the `_ex` wrapper, and the `ir/opt.h` decls; re-point the two
  `test_opt_dead_store.c` unit tests onto `ssa_opt_dead_loop` (or move them to
  `test_ssa_opt_dead_loop.c`) and keep 318 + `bug_dead_loop_assign_overlap` as
  green anti-regression pins. No new transform code.
- **(b) real gaps** (expected: memory-VAR idempotent bodies and/or self-stores
  surviving to regalloc) — extend `ssa:dead_loop` to close them (design below),
  then retire the legacy pass.
- **(c) narrow-and-document** — a gap exists but is a shape we choose not to
  keep (e.g. relies on the unsound unconditional preheader hoist). Document the
  dropped behavior as an accepted narrowing with a covering test, exactly as
  the first-iter plan did for 20070824-1.

Record the loop-by-loop matrix in a *Step 0 Results* subsection here before
implementation starts. **Do not skip to (b) by assumption** — the recent SSA
loop passes plus DSE plausibly cover more than the 2026-07-05 split did.

## Step 0 Results (2026-07-07)

**Outcome: (a) fully subsumed — and stronger: the legacy pass is provably
inert at its tccgen site.** Retire, no SSA extension. Same shape as
`loop_guard_elim` and `decrement_to_zero`: rotation, `loop_const_sim`, and
`loop_unroll` all moved to `ir/regalloc.c` (pre-SSA build) and now run *before*
the late `tccgen.c` tail where `dead_loop_elim` sits, so by the time it runs
every loop is already collapsed/unrolled or in a rotated SSA-ready shape whose
flat-range detector + soundness guards decline.

**Experiment** — both `tccgen.c` call sites forced to `dle_changes = 0`,
`make cross`, then at `-O2` 

| Suite | Baseline | Legacy disabled | Delta |
|---|---|---|---|
| QEMU IR (`tests/ir_tests`, incl. torture execute O0/O1/O2 + pins 188/317/318/327/328/`bug_dead_loop_assign_overlap`) | 13475 passed, 246 skipped, 1 xfailed | 13475 passed, 246 skipped, 1 xfailed | **0** |
| GCC compile torture (`tests/gcctestsuite -m gcc_compile`) | 57 pre-existing fails | identical set | **0 new** |

**Fire count.** Instrumented `tcc_ir_opt_dead_loop_elim` to log every
invocation while compiling the whole torture + `ir_tests` corpus (4306 files) at
`-O2`: the pass **ran 23 696 times with loops present (`nloops>0`) and returned
`changes>0` exactly 0 times.** This is the `loop_guard_elim` retirement standard
(`guard_found=2 guard_removed=0`) at far larger scale — 0 fires ⇒ byte-identical
object output ⇒ retirement is a proven no-op.

**Distinctive-domain probes** (the two shapes the plan expected a gap in). For
each, legacy-enabled and legacy-disabled produced **byte-identical** post-opt IR
at `-O2` — i.e. legacy does not fire on any of them, so retirement changes
nothing:

| Probe | Shape | `-O2` result (identical enabled/disabled) | Owner |
|---|---|---|---|
| memory-VAR, const bound | `int a=0; int*p=&a; for(i<10) a=5; return *p;` | loop fully **unrolled**; loop gone (residual 10 redundant addr-taken stores DSE keeps) | `ssa:loop_unroll` |
| memory-VAR, runtime bound | same, bound `n` | loop **survives** as `V0<-#5`+counter — *legacy never collapsed it either* | (neither; pre-existing) |
| self-store `*p=*p` | `struct S a={1,2}; for(i<n) a=a;` | self-copy removed by **DSE**; residual empty counter loop survives — *legacy never collapsed it either* | `ssa:dse` (+ residual) |

The two "surviving loop" residuals (runtime-bound memory-VAR, empty counter
loop) are **pre-existing** — they are present today with the legacy pass
enabled, because the legacy pass is inert on them. Retirement neither creates nor
fixes them. They are not regressions; closing them would be new optimization
work, out of scope for this migration. The legacy pass's own runtime-bound
memory-VAR arm was the *unconditional preheader hoist* the Intent flags as
unsound (returns `5` when `n<=0`); its inertness means that latent unsoundness
was never actually reachable through the frontend at its call site — another
reason retirement is strictly safe.

Because the outcome is (a) and the legacy pass is a proven no-op, the *SSA
Replacement Design* section below (self-store purity, memory-VAR arm) is **not
implemented** — it is retained only as a design record should a future frontend
change resurrect one of these shapes at the SSA fixed point.

## SSA Replacement Design (only if Step 0 = outcome (b))

Close the gap inside the existing `ssa:dead_loop` pass — keep one dead-loop
owner rather than adding a parallel pass. Two extensions, in priority order:

1. **Self-store bodies are pure.** In `loop_body_has_side_effects`
   (`ir/opt/ssa_opt_dead_loop.c:146`), treat a `STORE` that writes back the
   exact value just loaded from the same slot as non-side-effecting — port the
   legacy `is_self_store` back-walk (`ir/opt_dce.c:5617-5668`). This lets the
   existing kill path handle inlined-struct-copy loops without touching the
   mutation code.
2. **Memory-VAR idempotent bodies.** When the loop result is a stack local that
   never became a phi, `ssa:dead_loop` has nothing to rewrite. Two sub-options,
   decided by what Step 0 shows actually survives:
   - **preferred: rely on DSE.** If Step 0 shows `ssa:dse` +
     `dead_var_store_elim` already remove the redundant VAR store (leaving an
     empty counted loop), then no phi work is needed — `ssa:dead_loop`'s
     existing `try_kill_loop_body` collapses the now-empty loop. The extension
     is just confirming the pass runs *after* those DSE passes have fired
     (it does — the whole SSA fixed point iterates).
   - **fallback: port the VAR-const hoist.** If a memory store genuinely
     survives, add a memory-VAR arm that recognizes "every iteration stores the
     same constant to slot S, S has no other in-loop writer, and the loop is
     otherwise pure" and emits the store **on the proven-trip path only** (or a
     guarded SELECT store on the runtime path) — never the legacy's
     unconditional preheader hoist. This reuses `try_kill_loop_body`'s
     explicit-`JUMP exit_target` so the 317/318 edge is preserved for free.

**Detector coherence (do regardless of (a)/(b)):** `ssa_opt_dead_loop` still
calls the flat `tcc_ir_detect_loops` (`ir/opt/ssa_opt_dead_loop.c:856`) and
carries a large `dead_loop_body_hi` band-aid for the detector's over/under-count
(random-C seeds 51/52/132/281; test 188). Its siblings in
`ir/opt/ssa_opt_loop.c` moved to a dominance-verified CFG front-end
(`lcs_collect_header_members`, `tcc_ir_cfg_dominates`). Migrating `ssa:dead_loop`
onto the same substrate removes the `dead_loop_body_hi` heuristic and the whole
class of body-range bugs it patches. Scope this as a **separate, optional
follow-up** — not on the critical path for retiring the legacy pass, but the
natural place to converge, and it should be listed in the parent tracker.

- **Name/observability:** unchanged — `ssa:dead_loop`, `TCC_DISABLE_PASS`
  already works. No new pass name.
- **Gate:** unchanged — `optimize >= 2` in the SSA fixed point.

## Required Unit Tests

The isolated harness `test_ssa_opt_dead_loop.c` (10 tests) is the home for new
coverage. If Step 0 = (b), add, asserting both change count and resulting IR
(rewritten uses, NOPed body, the emitted `JUMP exit_target`, untouched non-body
code):

- self-store body (`T=*p; *p=T;` + counter) collapses; a real store body does
  not;
- memory-VAR idempotent body (whichever sub-option 2 lands) — proven-trip and
  runtime-bound variants, the latter asserting the guarded SELECT/skip;
- the 318 shape (loop as then-arm of `if`) collapses **with** an explicit exit
  `JUMP` — a direct pin that the SSA path preserves the edge the legacy pass
  needed `need_exit_jump` for;
- idempotency (second run returns 0) and inert coexistence (running on IR the
  legacy pass already collapsed returns 0), matching the sibling contract.

If Step 0 = (a), instead re-point `test_dead_loop_elim_const_assign_loop_removed`
and `test_dead_loop_elim_ex_empty` from `tcc_ir_opt_dead_loop_elim` onto
`ssa_opt_dead_loop` (same shape, SSA driver), or delete them with the symbol and
rely on `test_ssa_opt_dead_loop.c` + 327/328.

IR regressions: 318, `bug_dead_loop_assign_overlap`, 317, 327, 328, 188 all stay
green throughout and after retirement. Add one new
`tests/ir_tests/NNN_dead_loop_elim_ssa.c` (next free number; register in
`TEST_FILES`) pinning the specific shape the retirement makes the SSA pass newly
responsible for (per Step 0), verified green at `-O0/-O1/-O2` before (legacy
covers it) and after (SSA covers it).

Update `tests/unit/PASS_COVERAGE.md` for whichever suite changes.

## Migration Steps

- [x] Run the Step 0 experiment; record the loop-by-loop matrix and the chosen
  outcome (a)/(b)/(c) in *Step 0 Results*.  → **(a)**, and stronger: proven
  inert (0 fires / 23 696 loop-bearing -O2 invocations).
- [x] (only if (b)) Extend `ssa_opt_dead_loop` — **N/A**, outcome was (a).
- [x] Add the new IR regression (`348_dead_loop_elim_retired_shapes.c`) — pins
  the probed shapes as correctness across the retirement.
- [x] Verify with the legacy pass still enabled — the Step 0 experiment IS the
  legacy-vs-disabled diff (identical IR + zero test regressions).
- [x] Delete the two `tccgen.c` call sites (leave tombstones) and their now-dead
  cleanup cascades (the second site's unconditional DSE prep is kept — it is
  general cleanup independent of the retired re-run).
- [x] Delete `tcc_ir_opt_dead_loop_elim`, the `_ex` wrapper, and the two
  `ir/opt.h` decls; remove the two `test_opt_dead_store.c` unit tests (they
  tested the deleted symbol); `PASS_COVERAGE.md` needs no row change (the pass
  had no dedicated row — coverage was the `UT_COVERS("dead_loop_elim")` alias,
  now removed).
- [ ] (optional follow-up, tracked separately) migrate `ssa_opt_dead_loop` off
  `tcc_ir_detect_loops` onto the CFG/dominance front-end and drop
  `dead_loop_body_hi`.
- [ ] **Maintainer-run** before merge: the wider fuzz sweep from *Acceptance*
  (`sweep_all_chunks.py 0 1000 --mode triage`; `diff_olevels.py --seeds 0-5000
  --require-qemu`), which cannot run inside the dev loop.

## Retirement hazard

- **Benchmark-cycle parity.** The o1-o2 split relies on these loops collapsing
  at `-O2` (bench_conditionals/switch/strcmp ~35 cyc, not ~4000). Step 0 must
  confirm `ssa:dead_loop`/`loop_const_sim`/`unroll` still collapse them once the
  legacy pass is gone; a surviving benchmark loop is a hard fail, not a
  documentable narrowing.
- **Cleanup-cascade downstream service.** The two call sites' cascades
  (`value_tracking/const_prop_tmp/branch_folding/dce/dse`) also fold code
  *downstream* of the collapsed loop. After removal that folding shifts to the
  remaining tccgen tail and the SSA fixed point. Verify final `-O2` disassembly
  shows no surviving loop, residual dead/self-stores, or unfolded guard
  branches vs. baseline on the 318/`bug_dead_loop_assign_overlap`/327/328
  shapes.
- **`-fno-dce` meaning.** Legacy `dead_loop_elim` dies with `-fno-dce` today;
  `ssa:dead_loop` is gated only on `optimize>=2`. After retirement, `-fno-dce`
  no longer suppresses dead-loop collapse. This is a *more* correct knob split
  (DCE and dead-loop elimination are separable) but is a behavior change for
  anyone bisecting with `-fno-dce`; note it, and confirm `TCC_DISABLE_PASS=
  ssa:dead_loop` is the documented replacement isolation knob.
- **Two-phase re-run.** The legacy second call site (`:30525`) exists to catch
  loops emptied only after the *final* DCE (inline struct copies). The SSA
  fixed point iterates to convergence, so this should be covered — but Step 0's
  self-store probe is exactly this case; confirm it collapses in the SSA
  fixed point rather than needing a second manual pass.

## Acceptance

For landing any `ssa:dead_loop` extension (legacy still enabled):

- [ ] New + existing `test_ssa_opt_dead_loop.c` unit tests pass; the
  `test_opt_dead_store.c` `dead_loop_elim` tests still pass.
- [ ] 318, 317, 327, 328, 188, `bug_dead_loop_assign_overlap` all green.
- [ ] `make cross -j$(nproc)`
- [ ] `make test -j16`
- [ ] GCC torture suite (`pytest tests/gcctestsuite/ -v`).
- [ ] **Maintainer-run:** `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage`
  → 0 divergent profiles; `python3 scripts/diff_olevels.py --seeds 0-5000
  --require-qemu` → 0 divergences.
- [ ] **Maintainer-run:** same sweep with `TCC_DISABLE_PASS=ssa:dead_loop`
  (pass off must equal today's behavior with legacy also disabled), and a
  replacement-only run (legacy call sites disabled locally) before proposing
  retirement.

For the separate retirement step: all of the above re-run after the call-site
removal, plus the code-size comparison and the disassembly parity checks from
*Retirement hazard*, plus the maintainer's wider fuzz sweep before the legacy
driver is deleted.

## Assumptions

- `ssa:dead_loop` keeps its current position and `optimize>=2` gate in the SSA
  fixed point.
- The legacy pass stays enabled and untouched until Step 0 decides the outcome
  and (for (b)) the extension has passed the gate.
- 317 pins `try_eliminate_loop` (a sibling pass), not this one; it is unaffected
  by this migration and must stay green.
- The `ssa_opt_dead_loop` → CFG-front-end migration is a coherence follow-up,
  not a precondition for retiring the legacy flat pass.
