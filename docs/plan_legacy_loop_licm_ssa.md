# Plan: Migrate `licm` (loop-invariant code motion) off the legacy pre-SSA tail

**Status:** proposed pass plan · **Created:** 2026-07-07

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents:
[`plan_legacy_loop_iv_strength_reduction_ssa.md`](plan_legacy_loop_iv_strength_reduction_ssa.md)
(**the direct cousin** — LICM is IV-SR's *upstream feeder*: `tcc_ir_opt_licm_ex`
computes the loop set and hands it to IV-SR via
`tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops)`. Read its "Why this
pass is different" and its Deferred-v2 coordinated-cluster section — LICM is the
head of the same cluster IV-SR sits inside),
[`plan_legacy_loop_const_sim_ssa.md`](plan_legacy_loop_const_sim_ssa.md) and
[`plan_legacy_loop_unroll_ssa.md`](plan_legacy_loop_unroll_ssa.md)
(the engine-reuse REPLACE precedent — retain the mutator, delete the flat-range
driver, re-drive from a CFG/dominator front-end in `ir/opt/ssa_opt_loop.c`), and
[`plan_legacy_loop_bound_remat_ssa.md`](plan_legacy_loop_bound_remat_ssa.md)
(the observability-first pattern: land a real disable/dump name so the Step-0
firing survey can even run).

## Why this pass is different (read first)

LICM is the largest, most structurally unusual entry in the tracker. Five facts
set it apart from every sibling and shape this whole plan:

1. **It is two passes wearing one hat**, with very different maturity and risk:
   - **dom-LICM (arithmetic hoisting)** — hoists side-effect-free ALU/LEA/ASSIGN
     out of loops. Crucially it **already builds its own CFG + dominators**
     internally (`tcc_ir_cfg_build` / `tcc_ir_cfg_compute_dominators` at
     `ir/licm.c:2383/2385`) and does dominance-verified hoisting. It is *half
     migrated already* — its detection is CFG/dominator-based, not the flat
     range-scan the tracker flags as high-risk.
   - **pure/const-call hoisting** — hoists `FUNCCALLVAL`/`FUNCCALLVOID` of
     PURE/CONST callees into the preheader. This half is driven by the **flat
     `tcc_ir_detect_loops`** pattern detector (with the `+50` forward-jump body
     over-approximation) and carries **the heaviest fuzz history of the entire
     cluster** — legacy bug #7, resolved across ~10 separate defects (memory
     notes [[pure-call-hoist-redisabled]], [[volatile-3583-6116-pure-call-hoist-regression]],
     [[ptr-500-517-pure-hoist-addr-taken-arg]]). Any change here must sweep all
     fuzz profiles.

   These two sub-passes have **different correct migration targets** and should
   be treated as separable migration units, not one monolith.

2. **It is IV-SR's upstream feeder — the mirror image of IV-SR's own coupling.**
   IV-SR was kept in place because *downstream* tccgen consumers needed its
   output in order. LICM has the opposite constraint on its *upstream* side: its
   returned `IRLoops*` is handed straight to
   `tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops)` "to avoid a
   re-detection index mismatch" ([tccgen.c:30066-30073](../tccgen.c#L30066)). If
   LICM relocates out of tccgen (to the regalloc-time region, after IV-SR),
   IV-SR loses `licm_loops` and falls back to self-detection
   (`tcc_ir_opt_iv_strength_reduction(ir)`, the existing `else` arm). Whether
   that fallback is behavior-equivalent is the load-bearing Step-0 question. The
   coupling is **weaker** than IV-SR→downstream (IV-SR only reuses LICM's
   *detection*, not its hoisted code, order-sensitively), but it is real.

3. **It actively fires — it is not a retirement candidate.** Unlike
   `dead_loop_elim` / `loop_guard_elim` / `loop_bound_remat` (which went inert
   once rotation/const_sim/unroll relocated *ahead* of them), LICM runs at
   tccgen **Phase 5**, *before* IV-SR, on un-rotated/un-unrolled loops — its
   input shapes did not change when the shape passes moved to regalloc. It has
   live IR pins (`97`, `100`–`104`, `259`, `268`) and 48 unit tests. Step 0 will
   almost certainly show non-zero firings ⇒ **REPLACE, not RETIRE.** (Step 0
   still runs the firing survey to confirm, per the migration rules.)

4. **There is no SSA-side LICM/PRE to build on.** The SSA optimizer
   (`ir/opt/ssa_opt*.c`) has loop *shape* passes but no invariant hoisting;
   `ssa:gvn` (`ir/opt/ssa_opt_gvn.c`) does dominator-tree **full**-redundancy
   elimination but never inserts a computation into a preheader and is not
   loop-aware. So an in-SSA `ssa:licm` would be a genuine reimplementation, not
   an engine re-drive — the one place LICM diverges from the const_sim/unroll
   precedent.

5. **Its `-flicm` gate is currently broken (confirmed).** At
   [tccgen.c:30058-30060](../tccgen.c#L30058) the `if (tcc_state->opt_licm)`
   guards **only** the `dbg_scan_overlap` debug call; the actual
   `licm_loops = tcc_ir_opt_licm_ex(ir);` runs on the next line, **outside** the
   `if` (missing braces). So `-fno-licm` does *not* disable LICM hoisting today —
   it only skips a debug scan. There is also **no** `dump_ir_after_pass("licm")`
   and **no** `TCC_DISABLE_PASS=licm` for the arithmetic-hoist half (only the
   pure-call sub-phase has `TCC_DISABLE_PASS=pure_call_hoist` at
   `ir/licm.c:1819`). Fixing this gate + adding real observability is the
   concrete, immediately-valuable v1a deliverable.

## Intent

Preserve LICM's behavior **exactly** while (a) giving it real, isolable
observability — a working `-flicm` gate, a `TCC_DISABLE_PASS`/dump name for the
arithmetic-hoist half, keeping the existing `pure_call_hoist` sub-knob — and (b)
recording the staged path to its correct architectural home without perturbing
the fuzz-fragile pure-call hoister.

The behavior to preserve — per detected loop, in one `tcc_ir_opt_licm_ex(ir)`
pass over the flat IR:

1. **Detect loops** (`tcc_ir_detect_loops`, `ir/licm.c:153`) and hand the
   resulting `IRLoops*` to IV-SR.
2. **Pure/const-call hoist** (`tcc_ir_hoist_pure_calls`, `ir/licm.c:1807`):
   copy a PURE/CONST call + its `FUNCPARAM*` into the preheader with a new
   call_id, rewrite the in-loop site to `ASSIGN` from the hoisted temp. Guarded
   by preheader-immediate-predecessor, no-external-entry, no-VLA,
   argument-invariance (`is_operand_loop_invariant_ex`), and
   memory-clobber (`loop_body_may_clobber_memory`, the PR20100 guard) checks.
3. **Dominance-based arithmetic hoist** (inline block, `ir/licm.c:2382-2758`):
   build a CFG + dominators, hoist single-def, provably-invariant
   `ADD/SUB/MUL/AND/OR/XOR/SHL/SHR/SAR/ROR/ASSIGN/LEA` (never a load/deref
   source) whose block dominates all loop exits, capped by
   `tcc_ir_estimate_hoist_budget`; skip the whole function if it contains
   `SWITCH_TABLE`.
4. **Re-detect** loops if anything was hoisted so the returned `IRLoops*` has
   valid indices for the IV-SR consumer (`ir/licm.c:2762`).

Preserve the secondary call site unchanged: per-function purity inference +
cache ([tccgen.c:31633-31643](../tccgen.c#L31633),
`tcc_ir_infer_func_purity` / `tcc_ir_cache_func_purity`) that feeds same-TU
pure-call hoisting. This is analysis, not mutation, and must keep running for any
placement of the hoister.

Behavior intentionally **not** changed in v1 / explicit non-goals:

- **No relocation in v1.** The pass stays at tccgen Phase 5, before IV-SR. Moving
  it is deferred (see "Deferred v2/v3"), gated on Step 0 proving IV-SR's
  self-detection fallback is equivalent (or on the coordinated cluster relocating
  LICM + IV-SR together).
- **No touching the pure-call hoister's mutation or guard logic.** The
  `insert_instruction_before` growth + JUMP/JUMPIF **and SWITCH_TABLE
  side-table** renumber (`ir/licm.c:499-537`), the call-param copy, the
  argument-invariance and memory-clobber guards are all frozen. v1 changes only
  *how the pass is gated/observed*.
- **No SSA-native hoisting, no widening of the hoistable-opcode set, the
  register-pressure budget, or the dominance guards.**
- **No decoupling the shared detection→IV-SR hand-off.** v1 keeps
  `licm_loops` flowing to `_with_loops`; the fallback-equivalence question is a
  Step-0 *measurement*, not a v1 change.

## Current Legacy Shape

- **Primary call site:** [tccgen.c:30056-30073](../tccgen.c#L30056), "Phase 5".
  **Contains the confirmed gate bug** (Why §5): the `if (tcc_state->opt_licm)` at
  `:30058` guards only `dbg_scan_overlap(ir,"Q2-before-licm")` at `:30059`;
  `licm_loops = tcc_ir_opt_licm_ex(ir)` at `:30060` runs **unconditionally**.
  The returned `licm_loops` flows into
  `tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops)` at `:30069`
  (else `tcc_ir_opt_iv_strength_reduction(ir)` at `:30071`), then
  `tcc_ir_free_loops(licm_loops)` at `:30073`. The commented-out block above
  (`:30052-30055`) is a *different*, historically-disabled relocation attempt —
  do not confuse it with the live call.
- **Secondary call site:** [tccgen.c:31633-31643](../tccgen.c#L31633) — per-
  function `tcc_ir_infer_func_purity` + `tcc_ir_cache_func_purity`, gated
  `opt_licm`. Analysis/cache only; preserve as-is.
- **Gate / flag:** `tcc_state->opt_licm` (`-flicm`/`-fno-licm`,
  `libtcc.c:1741`), enabled at **`-O2`** (`libtcc.c:2313`,
  `if (s->optimize >= 2) s->opt_licm = 1;`). Because of the brace bug the flag is
  currently a **no-op for hoisting** — it only toggles a debug scan.
- **Observability today: essentially none for the arithmetic half.** No
  `dump_ir_after_pass("licm")` — the combined LICM+IV-SR output is dumped as
  `"ZZ_iv_strength_red"` under `CONFIG_TCC_DEBUG` ([tccgen.c:30075](../tccgen.c#L30075)).
  The **only** isolation knob is `TCC_DISABLE_PASS=pure_call_hoist`
  (`ir/licm.c:1819`), which disables **just** the pure-call sub-phase; dom-LICM
  arithmetic hoisting cannot be dumped or disabled at all. Closing this is the
  primary v1a deliverable.
- **Driver / engine (`ir/licm.c`, `ir/licm.h`):**
  - `int tcc_ir_opt_licm(TCCIRState *ir)` (`ir/licm.c:2314`) — wrapper returning
    *loop count*, frees loops; used by the metamorphic harness.
  - `IRLoops *tcc_ir_opt_licm_ex(TCCIRState *ir)` (`ir/licm.c:2323` → `__timed`
    at `:2332`) — **the pipeline entry**; returns an owned `IRLoops*` (caller
    frees). Runs detect → pure-call hoist → dom-LICM → re-detect.
  - `int tcc_ir_hoist_pure_calls(TCCIRState *ir, IRLoops *loops)`
    (`ir/licm.c:1807`) — the pure-call sub-pass (no header decl; used
    internally + by unit tests).
  - `IRLoops *tcc_ir_detect_loops(TCCIRState *ir)` (`ir/licm.c:153`) — flat
    pattern detector; `void tcc_ir_free_loops(IRLoops*)` (`:352`).
  - Purity API: `tcc_ir_infer_func_purity` (`:1265`), `_cache_` (`:1196`),
    `_lookup_` (`:1222`), `_get_` (`:1400`).
  - Register-pressure budget: `tcc_ir_estimate_hoist_budget` (`ir/licm.c:71`).
  - **`IRLoop`/`IRLoops` structs** (`ir/licm.h:28-46`): `header_idx`,
    `start_idx`, `end_idx`, `preheader_idx`, `body_instrs`, `num_body_instrs`,
    `depth`. dom-LICM ignores these (uses its own CFG); the pure-call hoister and
    the IV-SR consumer use them.
  - **Dead internals** (migration-awareness): `is_loop_invariant_operand`
    (`#if 0`, `:395`), `is_hoistable_instr` (`#if 0`, `:434`), `hoist_from_loop`
    (`unused`, `:627`), `hoist_const_exprs_from_loop` (`:883`) are never reached
    — do not port them; delete on relocation.
- **Mutation strategy (high-risk, frozen):** `insert_instruction_before`
  (`ir/licm.c:477`) grows `compact_instructions`, shifts the tail, and renumbers
  all JUMP/JUMPIF targets ≥ insert point **and** the `SWITCH_TABLE`
  `default_target`/`targets[]` side tables (`:524-537`) — the fix from bug #7's
  9th defect ([[pure-call-hoist-redisabled]]). Originals are NOP'd. Multiple
  fixpoints (pure-call transitive-invariant `do/while`; dom-LICM invariance
  `while(inv_changed)`; cross-loop index shift after insertions).
- **Existing unit tests** (`tests/unit/arm/armv8m/test_opt_licm.c`, 48 tests,
  `UT_COVERS("licm")` at `:1509`): dom-LICM hoist shapes
  (`test_licm_hoists_invariant_add` `:134`, `_lea_stack_addr_hoisted` `:484`,
  `_deref_source_not_hoisted` `:365`, `_store_not_hoisted` `:447`,
  `_in_loop_def_blocks_hoist` `:407`, `_div_not_hoisted` `:759`,
  nested/preheader/dominance shapes `:532-718`); pure-call hoisting
  (`_hoists_const_call_with_invariant_arg` `:196`,
  `_no_hoist_pure_call_when_loop_writes_memory` `:246`, the PR20100 guard);
  loop-detector API (`:811-947`); budget estimator (`:949-1056`); ~26 purity
  cache/inference tests (`:1058-1507`). Metamorphic registration at
  `test_metamorphic.c:55,83`.
- **Existing IR regressions** (`TEST_FILES`, `tests/ir_tests/test_qemu.py`):
  `97_loop_const_expr.c` (invariant const hoist); `100_pure_func_strlen.c`,
  `101_pure_func_abs.c`, `102_pure_func_strcmp.c`, `103_pure_func_multiple.c`,
  `104_pure_func_variant.c` (pure-call hoisting); `259_pure_call_hoist_addr_taken_arg.c`
  (ptr-500/517 pin); `268_pure_call_hoist_switch_table_targets.c` (the
  switch-table side-table desync pin — **hangs at -O1 without the fix**);
  `test_bubble_licm.c`. All must stay green throughout.
- **Known fuzz history (the heaviest of the cluster):** legacy bug #7 =
  pure-call hoisting, resolved across ~10 defects. The recurring lesson recorded
  in memory: the pass "escaped 3 times via profile-specific patterns (combo =
  switch-table desync, ptr = address-taken arg, volatile = else-arm body
  over-extension)". Any change to the pure-call hoister or `tcc_ir_detect_loops`
  must sweep all fuzz profiles. **v1 must not perturb this half.**

## Step 0: placement / equivalence experiment (decision gate)

Run before writing code; record the matrix here. Step 0 decides **(1)** whether
LICM still fires (REPLACE vs the unlikely RETIRE), **(2)** whether observability
+ the gate-bug fix is the correct v1 (it almost certainly is), and **(3)** which
of the deferred relocation homes is reachable and at what cost. Probes on the
motivating shapes — an invariant-arithmetic loop
(`for(i<n) x = a*b + c; use(x,i)` with `a,b,c` loop-invariant), a stack-address
LEA hoist, a CONST-call loop (`for(i<n) s += abs(k);` with `k` invariant), a
PURE-call-into-memory-writing loop (the PR20100 negative), and a
switch-inside-loop shape (the `268` pin) — compiled `-O1`/`-O2`, IR and
disassembly compared:

1. **Firing survey (REPLACE vs RETIRE).** With a working `-flicm` gate (fix the
   brace bug first, locally), byte-compare object output `-flicm` vs `-fno-licm`
   over `tests/ir_tests/*.c` (`-O1`,`-O2`) + a fuzz corpus (all profiles, a wide
   seed range) + a gcc c-torture ×-O1/-O2 sample. Instrument
   `tcc_ir_opt_licm_ex` to count dom-LICM hoists and pure-call hoists separately
   (do **not** commit the instrumentation). Expect **non-zero** firings (unlike
   the retired tail) ⇒ REPLACE. Record per-sub-pass fire counts; a zero for one
   sub-pass but not the other would justify splitting the migration.

2. **IV-SR self-detection equivalence (the relocation blocker).** The decisive
   probe. Locally force `licm_loops = NULL` at the IV-SR call site (simulating
   LICM having left tccgen) so IV-SR takes the `tcc_ir_opt_iv_strength_reduction(ir)`
   self-detect arm, **while leaving LICM's hoisting running** in its normal
   place. Byte-compare IV-SR's output vs the `_with_loops` path over the corpus.
   - **Identical** ⇒ the `_with_loops` hand-off is a pure convenience; LICM can
     in principle relocate independently (Deferred v2), IV-SR just self-detects.
   - **Divergent** ⇒ the hand-off is load-bearing; LICM cannot leave tccgen
     without IV-SR moving too — the coordinated cluster (Deferred v3). Record the
     first divergent shape.

3. **Observability-only sufficiency (v1a).** Confirm that fixing the brace gate +
   registering `tcc_ir_opt_pass_disabled("licm")` and
   `tcc_ir_dump_after_pass(ir, "licm")` at the call site delivers the tracker's
   asks: `-fno-licm` now actually suppresses all hoisting;
   `TCC_DISABLE_PASS=licm` isolates the arithmetic half independently of IV-SR
   and of `pure_call_hoist`; `-dump-ir-passes=licm` shows the hoisted IR. Confirm
   `TCC_DISABLE_PASS=pure_call_hoist` still isolates just the call hoister.

4. **Relocation-home feasibility (Deferred v2 vs v3 vs in-SSA).** Two homes to
   evaluate, since LICM (unlike the shape siblings) has a from-scratch option:
   - **(4a) Flat Phase-A relocation** — wire the *existing* `tcc_ir_opt_licm_ex`
     into `ir/regalloc.c` Phase A (after `ssa:decrement_to_zero`,
     ~`regalloc.c:4602`, before CFG/SSA build), as `ssa:licm`, matching the six
     siblings. dom-LICM already builds its own CFG there; the pure-call hoister
     runs on flat IR (its natural form). Verify the hoisting still fires and no
     later SSA pass un-does it (it should not — GVN commons within
     available-expression scope, and there is no LICM/PRE to re-hoist). This is
     viable **only if probe 2 is "identical"** (IV-SR self-detects) or as part of
     the coordinated cluster.
   - **(4b) In-SSA reimplementation** — sketch (do not build) an `ssa:licm` inside
     `tcc_ir_ssa_opt_run` near `ssa:gvn` using `IRSSAOptCtx` use-def chains +
     `ctx->cfg` dominators. In SSA, arithmetic invariance is trivial (a value is
     invariant iff every operand's def dominates the preheader or is constant) —
     no aliasing fixpoint. Record whether the pure-call hoister's memory-clobber
     guards translate cleanly to SSA memory modeling (they likely do **not**
     without extra work — this is why pure-call hoisting is the harder half and
     may stay flat/legacy longest).

5. **Sub-pass separability.** Given (1) and (4), decide whether dom-LICM
   (CFG-mature, low fuzz risk, in-SSA-friendly) and pure-call hoisting
   (flat-detector, high fuzz risk) should migrate as **one** unit or **two**.
   Recommended framing: they can and probably should be split — dom-LICM is the
   natural first in-SSA pass; pure-call hoisting is the conservative last mover.

Outcomes:

- **(a)** probe 1 shows firing, probe 3 shows the gate-fix + disable/dump names
  deliver the asks → **v1 = observability + gate-bug fix, in place** (fix the
  brace, register `licm`/keep `pure_call_hoist`, no detector change, no
  relocation, no retirement). **Recommended default**, mirroring IV-SR v1a and
  bound_remat v1a. Relocation choice (v2 flat / v3 cluster / in-SSA) is recorded
  from probes 2/4/5 but deferred.
- **(b)** probe 2 is "identical" *and* probe 4a shows the flat relocation fires
  and survives to interval construction → **Deferred v2 (flat Phase-A `ssa:licm`)
  is viable independently** and can be promoted after v1a, with IV-SR left to
  self-detect. Take only if probe 2 is cleanly identical over the whole corpus.
- **(c)** probe 2 is "divergent" → **relocation waits for the coordinated cluster
  (Deferred v3)**: LICM + IV-SR (+ bounded `local_alu_cse`) move into Phase A
  together, preserving the "LICM before IV-SR" order. This is the realistic path
  and is materially smaller than the IV-SR plan first assumed, because two of
  IV-SR's four downstream consumers (`loop_bound_remat`, `loop_postinc_fusion`)
  are now **retired** and `decrement_to_zero` is now `ssa:decrement_to_zero`.
- **(d)** unlikely: probe 1 shows both sub-passes inert now → follow the
  `dead_loop_elim`/`guard_elim`/`bound_remat` **retirement** methodology
  (fire-count + byte-compare proof) instead. Do not assume this; measure.

Record the shape-by-shape matrix here before implementation starts.

### Step 0 Results

**Decision: full relocation to the ssa: region, reusing the proven engine.**
Landed 2026-07-07.

The plan's central premise (LICM is IV-SR's upstream feeder, coupling constrains
relocation) was **already obsolete** when implementation started: `IV-SR was
independently relocated to the regalloc-time region` (`ssa_opt_iv_strength_reduction`,
`ir/regalloc.c`, commit d572eb87) and self-detects its own loops. So at tccgen
the returned `licm_loops` was **dead** — computed by `tcc_ir_opt_licm_ex` then
freed, consumed by nothing. The LICM→IV-SR coupling probe (probe 2) was moot:
there is no hand-off left to preserve.

That collapsed outcomes (b)/(c) into a clean relocation:

- **Placement.** `ssa_opt_licm` added as the **first** Phase-A pass in
  `tcc_ir_ssa_regalloc` (`ir/regalloc.c`, before `ssa:loop_rotate`), so it runs
  before `ssa:iv_strength_reduction` — restoring the legacy "LICM before IV-SR"
  order in the new location, on un-rotated loops (closest to the legacy Phase-5
  input shapes). Gated `opt_licm` (-O2); knob `TCC_DISABLE_PASS=ssa:licm`; dump
  `ssa:licm`.
- **Engine reuse (not rewrite).** `ssa_opt_licm` is a thin driver over the
  retained, proven `tcc_ir_opt_licm_ex` (dom-LICM arithmetic + `tcc_ir_hoist_pure_calls`).
  This deliberately does **not** re-derive the pure-call hoister's ~10 fixed
  defects (bug #7) in a new implementation — the guard/mutation logic is
  byte-identical, only the *driver location* moved. `TCC_DISABLE_PASS=pure_call_hoist`
  still isolates the call-hoisting sub-phase.
- **tccgen retirement + brace-gate fix.** The dead tccgen Phase-5 block (and its
  stale "DISABLED" comment) was deleted, which also **erases the brace-gate bug**
  (`-fno-licm` previously only skipped a debug scan).
- **Latent bug found + fixed (net-new, the requested "bug fix").** Removing the
  tccgen LICM call — whose `insert_instruction_before` had been shifting indices —
  exposed a pre-existing crash in `tcc_ir_opt_select`: `ir_skip_nops_forward`
  (`ir/opt_utils.c`) indexed `compact_instructions[start]` with a **negative**
  `start` (an unresolved/sentinel jump target reaching a call-diamond), an
  out-of-bounds read (ASan: "12 bytes before" the array). LICM's index-shifting
  had coincidentally kept `opt_select` off that diamond. Fixed by guarding
  `start < 0 → return n` (every caller already treats `>= n` as not-found).
  Regression: gcc.c-torture `pr42716` at -O1/-O2 (now green at all levels).
- **Validation:** full `make test-ir` **13504 passed** (the 2 `pr42716` crashes
  fixed), 246 skipped, 1 xfailed; unit tests **2848 passed / 0 failed** (incl. the
  48 `test_opt_licm` + metamorphic + new `test_ssa_opt_licm_hoists_invariant`);
  `ssa:licm` proven to *fire* (object output differs enabled vs
  `TCC_DISABLE_PASS=ssa:licm` on 97/100/101); `diff_olevels --seeds 0-1500` →
  **0 divergences** (extended range in progress at time of writing).

Consequences vs the original plan: the sub-pass split (arithmetic vs pure-call)
and the in-place-v1a step were **not needed** — the coupling that motivated them
was already gone. The aspirational "in-SSA rewrite near GVN" remains a possible
future refinement, but the engine-reuse relocation is the landed state.

## Replacement Design (in-place v1)

(Assumes Step 0 outcome (a). Under (b)/(c) the relocation design in "Deferred
v2/v3" is promoted; under (d) switch to the retirement methodology.)

### Placement: unchanged — tccgen Phase 5, before IV-SR

The call site and ordering stay exactly as today
([tccgen.c:30056-30073](../tccgen.c#L30056)), before IV-SR Phase 6. This
preserves the `licm_loops`→IV-SR hand-off and keeps LICM pre-SSA. No code moves
to `ir/regalloc.c` in v1.

### Fix the gate + naming, observability, disable knobs (the primary v1a deliverable)

- **Fix the brace bug.** Wrap the actual call so `opt_licm` gates it:
  `if (tcc_state->opt_licm) { dbg_scan_overlap(...); licm_loops = tcc_ir_opt_licm_ex(ir); }`
  Confirm `licm_loops` stays `NULL` when `opt_licm` is off so IV-SR takes its
  self-detect arm (the existing `else`). This makes `-fno-licm` actually disable
  hoisting for the first time — a genuine behavior change gated behind an opt-out
  flag, and the reason probe 2 (self-detect equivalence) must be run first.
- **Register a real pass name.** Add
  `if (tcc_state->opt_licm && !tcc_ir_opt_pass_disabled("licm")) { ... }` at the
  call site and `tcc_ir_dump_after_pass(ir, "licm")` after it (net-new dump —
  today only the combined `ZZ_iv_strength_red` exists). `TCC_DISABLE_PASS=licm`
  then isolates the whole LICM pass independently of IV-SR.
- **Keep the sub-knob.** `TCC_DISABLE_PASS=pure_call_hoist` (`ir/licm.c:1819`)
  stays as the finer isolation for just the call hoister; `licm` disables both
  halves.
- **Name decision (maintainer call, flag in Step 0):** v1 runs at **tccgen, on
  flat pre-SSA IR**, so — matching the IV-SR/bound_remat plans' reasoning — the
  honest v1 name is the **bare `licm`** (the legacy-tccgen-pipeline convention,
  like `const_prop`/`pure_call_hoist`), **not** `ssa:licm`. Reserve **`ssa:licm`**
  for the day it relocates to the regalloc-time region (Deferred v2/v3), where
  every sibling uses the `ssa:` prefix. State the choice in the call-site comment.
- Add `licm` (and confirm `pure_call_hoist`) to `tests/fuzz/triage_olevels.sh`'s
  `KNOBS` array so bisection can attribute LICM independently — mirroring the
  `ssa:loop_rotate` / `loop:bound_remat` entries already there.

### Comment policy (applies to every code change in this migration)

Per [[comments-max-one-liner]] and the sibling plans: new code (the gate fix,
disable/dump wiring, unit tests) carries **no comment blocks** — at most a
single-line comment where the code cannot express the constraint. Any legacy
comment in *touched* code is deleted or compressed to one line. The frozen
engine bodies in `ir/licm.c` are left untouched by the observability-only v1 so
their heavy comments are neither moved nor preserved into new code.

### Candidate detection — unchanged

Keep `tcc_ir_detect_loops`, the internal dom-LICM CFG build, and every guard
exactly. v1 is gate/observability only. (A CFG *validation* gate, the outcome-(b)
option other plans carry, is **not** offered here: dom-LICM is already
dominator-verified, and the pure-call hoister's fuzz history is about
argument-invariance / memory-clobber / side-table correctness, not false-loop
detection — so a natural-loop validation gate would buy little and risk the
fragile half. Loop-detection hardening belongs to the Deferred relocation, not
v1.)

### Mutation strategy and staleness — unchanged

`insert_instruction_before` growth, the JUMP/JUMPIF + SWITCH_TABLE side-table
renumber, the call-param copy, and every fixpoint are preserved verbatim. v1
touches no mutation path.

### Cleanup cascade — unchanged

None is added. The hoisted invariants are consumed downstream by `local_alu_cse`
(tccgen) and later CSE/GVN exactly as today.

### Interaction with other passes

- **IV-SR (downstream, still legacy in tccgen):** the load-bearing interaction.
  v1 keeps the `licm_loops`→`_with_loops` hand-off. The gate fix means `-fno-licm`
  now routes IV-SR through self-detection; Step-0 probe 2 must confirm that arm
  is equivalent before the fix ships enabled-by-default behavior changes.
- **`local_alu_cse` (downstream, tccgen `:30078`):** CSEs LICM's hoisted
  arithmetic; unchanged.
- **The secondary purity cache (`tccgen.c:31633`):** unchanged; still feeds
  same-TU pure-call hoisting regardless of placement.
- **The migrated regalloc-time siblings + `ssa:gvn`:** all run after tccgen,
  hence after LICM. GVN must not re-hoist/re-common LICM's preheader insertions
  (it does not — no PRE, commons within available-expression scope). This is a
  *constraint* only for the Deferred relocation; at the v1 pre-SSA placement it
  is unchanged and validated by shipping behavior.

## Required Unit Tests

v1 adds no new mutation surface, so the 48 existing `test_opt_licm.c` tests cover
the transform and stay green. Add, alongside them:

- **observability / gate (v1a):** a driver test asserting that with the pass
  disabled (via a helper honoring `tcc_ir_opt_pass_disabled("licm")`, mirroring
  the `test_loop_bound_remat_disable_knob_wired` pattern) an invariant-hoist loop
  is left un-hoisted (no IR growth, original op intact), and with it enabled the
  hoist fires. Pins the disable knob — the concrete v1 deliverable. (The disabled
  path is not force-testable in the shared UT binary since
  `tcc_ir_opt_pass_disabled` memoizes the env process-wide; pin the enabled/CI
  default, as the bound_remat test does.)
- **regression pins (no behavior change expected):** re-assert
  `test_licm_deref_source_not_hoisted`, `test_licm_store_not_hoisted`, and
  `test_licm_no_hoist_pure_call_when_loop_writes_memory` (PR20100) stay green
  after the call-site wiring changes.

Refresh the `test_opt_licm` row in `tests/unit/PASS_COVERAGE.md` if the suite
grows.

## IR Regression Test

The existing `97` + `100`–`104` + `259` + `268` pins are the acceptance corpus
and must stay byte-behavior-identical across v1 (v1 is behavior-preserving when
`opt_licm` is on, which is the `-O2` default). Add a new pin only if the
gate-fix changes an *observed* `-O2` outcome (it must not — `-O2` keeps LICM on),
or to pin the `-fno-licm`-now-actually-disables behavior end-to-end:

`tests/ir_tests/350_licm_disable_knob.c` (+`.expect`) — next free number (348 =
`dead_loop_elim`, 349 = `decrement_to_zero`); register in `TEST_FILES`. A
counted invariant-hoist loop producing a correct runtime result at
`-O0/-O1/-O2/-Os`, whose result is unchanged with `TCC_DISABLE_PASS=licm`. Added
first, green under the legacy pass. If Step 0 lands on outcome (a), this new pin
is optional and the existing pins suffice — record the decision.

## Migration Steps

Unlike the six shape siblings, **v1 does not retire the legacy engine** — it is
an in-place hardening, so the "delete legacy code" step is absent by design and
deferred to v2/v3.

- [ ] Run Step 0; fill in the matrix; pick outcome (a)/(b)/(c)/(d). Steps below
  assume (a).
- [ ] **v1a:** fix the brace-gate bug at [tccgen.c:30058-30060](../tccgen.c#L30058);
  register `tcc_ir_opt_pass_disabled("licm")` + `tcc_ir_dump_after_pass(ir, "licm")`
  at the call site; add `licm` to `tests/fuzz/triage_olevels.sh` `KNOBS`. Add the
  observability unit test.
- [ ] Confirm observability: `-dump-ir-passes=licm` shows the hoisted IR;
  `TCC_DISABLE_PASS=licm` restores the un-hoisted loop; `TCC_DISABLE_PASS=pure_call_hoist`
  still isolates just the call hoister; `-fno-licm` now suppresses all hoisting
  and routes IV-SR through self-detection (probe 2 must be green first).
- [ ] Confirm the 48 driver tests + the `97`/`100`–`104`/`259`/`268` pins are
  byte-behavior-identical at `-O2` (v1 is behavior-preserving with `opt_licm` on).
- [ ] Add `350_licm_disable_knob.c` only if pinning the new `-fno-licm` behavior;
  otherwise rely on existing pins.
- [ ] Local gates: unit tests, `make cross -j$(nproc)`, `make test -j16` (incl.
  GCC torture compile).

## Retirement hazard / Deferred v2/v3 (NOT part of v1)

Relocation/retirement of LICM out of tccgen is a **v2/v3 effort**, not this task.
Recorded so the tracker checkbox is understood to stay **open** after v1:

- **Deferred v2 — flat Phase-A `ssa:licm` (independent).** Only if Step-0 probe 2
  is cleanly "identical": relocate `tcc_ir_opt_licm_ex` into `ir/regalloc.c`
  Phase A (after `ssa:decrement_to_zero`, before CFG/SSA build) as `ssa:licm`,
  reusing the existing engine (delete the dead `#if 0` internals; keep the CFG
  build). IV-SR left to self-detect. Fold in the `tcc_ir_detect_loops` `+50`
  over-extension hardening the pure-call fuzz history flagged. Gate the removal on
  a disassembly/code-size comparison vs baseline over the IR + torture corpus.
- **Deferred v3 — coordinated cluster (realistic if probe 2 divergent).** Move
  LICM + IV-SR + bounded `local_alu_cse` into Phase A **together**, preserving
  the "LICM before IV-SR" order, per the IV-SR plan's Deferred-v2. Now smaller
  than that plan assumed: `loop_bound_remat`/`loop_postinc_fusion` are retired and
  `decrement_to_zero` already moved, so the cluster is just these three. Split the
  shared detection hand-off cleanly; keep `insert_instruction_before` shared.
- **Aspirational — in-SSA `ssa:licm` near `ssa:gvn`.** The architecturally
  correct end state for the **dom-LICM (arithmetic)** half: a from-scratch pass
  inside `tcc_ir_ssa_opt_run` using `IRSSAOptCtx` use-def chains + dominators,
  where invariance is trivial and the flat-detector fragility vanishes. Requires
  proving parity with the retained legacy engine before deletion (migration
  rules). The **pure-call hoisting** half likely stays flat/legacy longest (its
  memory-clobber guards do not translate cheaply to SSA) — the strongest argument
  for the sub-pass split (Step-0 probe 5).
- **GVN/CSE re-commoning** — as for bound_remat, any relocated placement must run
  where no later value-numbering re-hoists the preheader insertions; the pre-SSA
  Phase-A home satisfies this (GVN runs after and does no PRE), the post-SSA home
  would not for a pass that *inserts* into preheaders.

## Acceptance

For landing v1 (legacy engine retained, in place):

- [ ] New observability/gate unit test passes; the 48 existing `test_opt_licm.c`
  tests pass unchanged.
- [ ] `97` + `100`–`104` + `259` + `268` pass at `-O0/-O1/-O2` (and `-Os`),
  byte-behavior-identical to pre-change at `-O2`.
- [ ] `-dump-ir-passes=licm` / `TCC_DISABLE_PASS=licm` / `TCC_DISABLE_PASS=pure_call_hoist`
  work; `-fno-licm` now actually disables hoisting (probe 2 green).
- [ ] `make cross -j$(nproc)`; `make test -j16`; GCC torture suite.
- [ ] **Maintainer-run** (after implementation): `tests/fuzz/sweep_all_chunks.py`
  over a wide range → 0 divergent profiles;
  `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` → 0 divergences;
  a run with `TCC_DISABLE_PASS=licm` must match `-fno-licm` behavior on the
  corpus. Sweep **all** fuzz profiles (the pure-call hoister escaped 3× via
  profile-specific patterns).
- [ ] Zero new divergences; any divergence triggers fixes before merge.

The parent tracker's `licm` checkbox stays **open** after v1 (gate fix +
observability + hardening only); it is ticked when a Deferred relocation
(v2/v3/in-SSA) lands and the tccgen driver is removed.

## Assumptions

- IV-SR keeps its current tccgen Phase 6 position; v1 preserves the
  `licm_loops`→`_with_loops` hand-off. The gate fix routes IV-SR through
  self-detection only when `-fno-licm` is given (Step-0 probe 2 gates whether
  that arm is equivalent).
- `tcc_ir_detect_loops` / `tcc_ir_free_loops` / `insert_instruction_before` /
  `tcc_ir_estimate_hoist_budget` / the purity API remain shared utilities,
  unmodified by v1 (the mutation and detection paths are frozen).
- The secondary per-function purity inference/cache
  ([tccgen.c:31633](../tccgen.c#L31633)) keeps running under `opt_licm` for any
  placement of the hoister.
- `ssa:gvn` and the regalloc-time siblings (all after tccgen) do not re-hoist the
  preheader insertions at the current pre-SSA placement (shipping behavior); this
  becomes a *constraint* only for the Deferred relocation.
- The `-flicm`/`opt_licm` gate, once fixed, is the sole `-f` control; a
  `pure_call_hoist`-only `-f` flag is **not** added in v1 (the
  `TCC_DISABLE_PASS=pure_call_hoist` env knob remains the sub-isolation).
