# Plan: Migrate `iv_strength_reduction` off the legacy pre-SSA tail

**Status:** LANDED (full relocation) · **Created:** 2026-07-06 · **Landed:** 2026-07-07

> **Update 2026-07-07:** This plan was written assuming v1 = observability-only
> in-place (the four-consumer downstream cluster blocked relocation). By the time
> it ran, three of the four consumers had been retired/relocated, so **Step 0
> landed on outcome (c)** and the full relocation (the plan's "Deferred v2") was
> done directly instead: `ssa:iv_strength_reduction` in `ir/opt/ssa_opt_loop.c`,
> driven from `ir/regalloc.c` after `ssa:loop_unroll` and before
> `ssa:decrement_to_zero`; the legacy tccgen call site and the
> `tcc_ir_opt_iv_strength_reduction` / `_with_loops` drivers were removed; the
> shared engine `iv_strength_reduction_core` and the entire mutation/escape/
> `APPLY_SHIFT` path are retained frozen. See **Step 0 Results** below for the
> evidence. The sections below describing v1-in-place are kept for the historical
> record; the "Deferred v2" section is what shipped.

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents:
[`plan_legacy_loop_rotation_ssa.md`](plan_legacy_loop_rotation_ssa.md)
(placement, detector-swap strategy, in-task retirement),
[`plan_legacy_loop_dead_first_iter_ssa.md`](plan_legacy_loop_dead_first_iter_ssa.md)
(Step-0 gate, shared-helper extraction, cascade-parity handling),
[`plan_legacy_loop_ptr_iv_exit_subst_ssa.md`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md)
(member-span synthetic `IRLoop`, single-entry/step-legality facts,
process-one-then-rebuild rule — **and the pass that consumes IV-SR-shaped
pointer IVs' cousins**), and
[`plan_legacy_loop_unroll_ssa.md`](plan_legacy_loop_unroll_ssa.md)
(the IR-growth / `insert_instr_at` side-table-remap precedent — IV-SR is the
*other* legacy pass that grows the flat IR).

## Why this pass is different (read first)

The four landed siblings (`ssa:loop_rotate`, `ssa:first_iter_exit`,
`ssa:ptr_iv_exit_subst`, `ssa:loop_const_sim`) share a property that made them
cleanly relocatable to `ir/regalloc.c` before SSA build: **nothing downstream in
`tccgen.c` consumed their specific output**. Their results either fed
SSA/regalloc directly or were tidied by general cleanups.

`iv_strength_reduction` is the opposite. It is the **hub of a tightly ordered
tccgen loop-pass cluster**:

- **Upstream feeder:** LICM (`tcc_ir_opt_licm_ex`, Phase 5) computes the loop
  set and hands it to IV-SR; the call site prefers
  `tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops)` precisely to
  avoid a re-detection index mismatch (`tccgen.c:30164-30180`).
- **Downstream consumers that MUST run after IV-SR** (all still legacy, all at
  tccgen time, i.e. *before* regalloc):
  - `local_alu_cse` (`tccgen.c:30195`, comment: "MUST run AFTER IV strength
    reduction" — CSE'ing the stride SHL+ADD chains before IV-SR collapses a
    distinct stride pointer into a stale base breaks the loop);
  - `loop_postinc_fusion` (`tccgen.c:30461`, comment: "Must run after IV
    strength reduction (Phase 6) which creates the latch ADD pattern");
  - `loop_bound_remat` (`tccgen.c:30467`, **gated on `opt_iv_strength_red`**,
    rematerializes the SP-relative end pointer IV-SR hoisted into the
    preheader — a separate tracker entry but functionally IV-SR's tail);
  - `decrement_to_zero` (`tccgen.c:30473`, "Must run late, after IV-SR has
    eliminated body uses of loop counters").

Relocating IV-SR alone to `ir/regalloc.c` (the sibling pattern) places it
*after* all four consumers, inverting the required order and breaking
`loop_postinc_fusion` and `loop_bound_remat` outright. **Therefore v1 cannot
relocate IV-SR.** This plan's v1 is an *in-place* migration: full observability
integration plus optional candidate-detection hardening, with the call site and
ordering preserved. Full relocation to the regalloc-time flat region is deferred
to a coordinated cluster plan, sequenced only after `loop_postinc_fusion` and
`loop_bound_remat` have their own SSA migrations (see "Deferred v2").

IV-SR is also the **most fuzz-fragile** legacy loop pass and the **only other
one besides `loop_unroll` that grows the flat IR** (`insert_instr_at` + the
`APPLY_SHIFT` index-shift bookkeeping, `ir/opt_loop_utils.c:2003-2039`). Its
recorded fuzz history is about the escape scan and the shared-DIV dedup, **not**
about flat-range false loops (the class CFG detection fixes). That inverts the
cost/benefit versus `ptr_iv_exit_subst`: the CFG-detection payoff is smaller
here and the mutation machinery it must not disturb is far larger.

## Intent

Preserve `iv_strength_reduction`'s behavior **exactly** while giving it the
observability and candidate-detection hygiene the tracker wants, without
disturbing the fragile insertion/shift machinery or its downstream cluster.

The behavior to preserve — the legacy driver, per detected loop, transforms
array-indexing recurrences `base + i*stride` (a per-iteration SHL+ADD, or an
MLA-fused form) into a maintained stride pointer `ptr += stride`, enabling
post-increment addressing, then optionally eliminates the counter IV against a
hoisted end pointer:

1. IV selection: `find_induction_vars_ex(..., allow_copy_through=1)`.
2. Derived-IV discovery over `loop->body_instrs`: `find_derived_ivs`
   (ADD/SHL/MUL forms plus the newer MLA-fused form with its own extended scan
   range, `ir/opt_loop_utils.c:142-260`).
3. Duplicate-DIV dedup (same `(iv, stride, base)` recurrence → `share_with`;
   duplicates deferred to the driver's re-detection loop rather than sharing a
   rewrite — docs/bugs.md #2 invariant, `ir/opt_loop_utils.c:1896-1935`).
4. **One DIV transformed per loop per call** (`transform_derived_iv`), then
   `APPLY_SHIFT` fixes up every remaining DIV/IV/loop index by the exact number
   of inserted instructions (init + optional post-nop + stride, some pushed past
   FUNCCALLs), then `goto try_elim` (`ir/opt_loop_utils.c:1947-2052`).
5. Counter-IV elimination against the cheapest end pointer among transformed
   DIVs (`try_eliminate_iv_counter`, cost heuristic preferring SP-relative
   bases, `ir/opt_loop_utils.c:2066-2100`).
6. Driver re-detects loops and repeats up to 8 times (`_with_loops`: reuse LICM
   loops for round 1, fresh detection for rounds 2-8, `ir/opt_loop.c:176-227`).

Behavior intentionally **not** changed in v1 / explicit non-goals:

- **No relocation.** The pass stays at tccgen Phase 6; the whole downstream
  cluster is untouched. (Deferred v2.)
- **No retirement of the legacy driver in this task.** Unlike the four siblings,
  v1 is not a replace-and-delete; the "SSA" driver *is* the legacy driver with
  observability wired in (and optionally a hardened candidate front end). The
  `_with_loops` / re-detection driver, the three shared cores, and the shift
  machinery are all retained verbatim.
- **No SSA-native (phi-aware) strength reduction**, no partial/runtime forms, no
  new DIV shapes, no widening of `MAX_IV`/`MAX_DIV` or the cost heuristic.
- **No touching the mutation path.** `transform_derived_iv`,
  `try_eliminate_iv_counter`, `insert_instr_at`, and the `APPLY_SHIFT` block are
  frozen. v1 may only change *how candidate loops are discovered and validated*
  and *how the pass is named/observed* — never how a transform mutates.

## Current Legacy Shape

- **Call site:** single site in `tccgen.c` "Phase 6"
  (`tccgen.c:30169-30183` at time of writing), gated
  `tcc_state->opt_iv_strength_red`. Dispatches
  `tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops)` when LICM ran
  (`licm_loops != NULL`), else `tcc_ir_opt_iv_strength_reduction(ir)`
  (self-detecting). No post-pass cleanup cascade at the site itself — the value
  is realized by the downstream cluster (local_alu_cse, postinc_fusion,
  bound_remat) listed above and by the general late cleanup tail.
- **Second, related site:** `loop_bound_remat` at `tccgen.c:30467`, gated on the
  same `opt_iv_strength_red` flag. It is a *separate* tracker entry
  (`loop_bound_remat`) but shares IV-SR's gate and depends on its output; note
  the coupling but do not migrate it here.
- **Gate / flag:** `tcc_state->opt_iv_strength_red` (`tcc.h:1046`), the `-f`
  flag `iv-strength-red` (`libtcc.c:1744`), enabled at `-O1+`
  (`libtcc.c:2320-2324`). Unlike `ptr_iv_exit_subst` (which had *no* flag), IV-SR
  is already isolable via `-fno-iv-strength-red` — but that flag *also* disables
  `loop_bound_remat` (shared gate).
- **Observability today:** `dump_ir_after_pass(..., "ZZ_iv_strength_red")` under
  `CONFIG_TCC_DEBUG` only (`tccgen.c:30181-30183`). **Not** registered with
  `tcc_ir_opt_pass_disabled`, so `TCC_DISABLE_PASS` cannot isolate it; **not**
  in the `-dump-ir-passes` name space. Logging scope `TCC_LOG_IV_SR` exists
  (`-DTCC_LOG_IV_SR=1`). Closing the `TCC_DISABLE_PASS` / `-dump-ir-passes` gap
  is the primary concrete deliverable of v1.
- **Driver:** `tcc_ir_opt_iv_strength_reduction` (`ir/opt_loop.c:176`),
  `tcc_ir_opt_iv_strength_reduction_with_loops` (`ir/opt_loop.c:199`), both
  calling `iv_strength_reduction_core` (`ir/opt_loop_utils.c:1846`). Prototypes
  `ir/opt.h:795,799`. Do not confuse with `tcc_ir_opt_strength_reduction`
  (`ir/opt_loop.c:143`, MUL→shift, Phase 7, gate `opt_strength_red`, dump
  `ZZ2_strength_red`, also registered as pipeline pass
  `tcc_opt_strength_reduction` in `tccopt.c:284`) — that is a **different**,
  non-loop pass and is out of scope for this entry.
- **Shared cores/analysis (all in `ir/opt_loop_utils.c`, declared in
  `ir/opt_loop_utils.h:44-73`, exercised directly by unit tests — all remain
  shared, none is legacy-owned):** `find_induction_vars_ex`, `find_derived_ivs`,
  `transform_derived_iv`, `try_eliminate_iv_counter`, `insert_instr_at`,
  `find_loop_exit_condition(_op)`, `compute_trip_count`. `iv_strength_reduction_core`
  itself is also declared shared (`ir/opt_loop_utils.h:57`) — a v1 candidate
  front end would call it, not reimplement it.
- **`body_instrs` is load-bearing (unlike every sibling).** `find_derived_ivs`
  scans `loop->body_instrs[]` for ADD/SHL/MUL DIV computations
  (`ir/opt_loop_utils.c:191-260`); `transform_derived_iv`'s escape scan uses
  `body_instrs[0..n-1]` bounds (`ir/opt_loop_utils.c:1031-1048`, docs/bugs.md
  #2); `APPLY_SHIFT` fixes up every `body_instrs[bi]`
  (`ir/opt_loop_utils.c:2036-2037`). Any candidate-detection swap **must**
  populate `body_instrs`/`num_body_instrs` for the synthetic loop exactly as
  LICM/`tcc_ir_detect_loops` would, or these cores misbehave. This is the single
  biggest structural difference from `ptr_iv_exit_subst` (whose core left
  `body_instrs = NULL`).
- **IR growth + side-table remap.** `transform_derived_iv` inserts instructions
  via `insert_instr_at` and the core patches sibling loop records
  (`ir/opt_loop_utils.c:3095-3180` region for the unroll-family; IV-SR's own
  patch is the `APPLY_SHIFT` block at `:2003-2039`). This is the parent
  tracker's designated high-risk "direct instruction insertion + side-table
  remapping" area and is **frozen** in v1.
- **Existing unit tests:**
  - `tests/unit/arm/armv8m/test_opt_loop.c` — driver-level:
    `test_iv_sr_loop_with_no_derived_ivs_converges_to_zero` (`:220`) covering the
    no-DIV / NULL-loops / empty-loops decline paths through
    `tcc_ir_opt_iv_strength_reduction` and `_with_loops`;
    `UT_COVERS("loop_iv_strength_reduction")` (`:1379`),
    `UT_COVERS("loop_bound_remat")` (`:1380`).
  - `tests/unit/arm/armv8m/test_opt_loop_utils.c` — rich core coverage:
    `find_induction_vars_ex` battery (`:210-395`); `transform_derived_iv`
    (`test_transform_derived_iv_skips_memory_feeding_div` `:687` — the
    docs/bugs.md #2 escape-scan guard; `..._reduces_register_only_div` `:748`;
    `..._shared_path_refused` `:836`); `find_derived_ivs`
    (`..._shl_add_pattern` `:1059`, `..._mul_variant_and_operand_order` `:1095`,
    `..._shl_multi_use_not_nopable_skipped` `:1128`) — note `:1063` documents
    that `find_derived_ivs` *requires `loop->body_instrs[]` populated
    explicitly*.
- **Existing IR regressions** (in `TEST_FILES`, `tests/ir_tests/test_qemu.py`):
  `110_iv_strength_reduction.c` (array_sum / short_array_sum / array_copy) and
  `258_derived_iv_strength_reduction.c` (addr_sum / addr_sum_single / opaque —
  the memory [[bug2-derived-iv-reenabled]] pin, test 258).
- **Known fuzz history tied to this pass:** docs/bugs.md #2 — the escape scan in
  `transform_derived_iv` must skip a DIV whose result feeds memory that escaped
  the `[start_idx..end_idx]` scan (the memory-feeding-DIV guard, pinned by
  `test_transform_derived_iv_skips_memory_feeding_div` and by test 258). Memory
  [[bug2-derived-iv-reenabled]]: derived-IV SR was re-enabled once the real
  culprit (`cmp_stack_addr_fold`'s merge-crossing resolver) was fixed via a
  taint escape scan over `body_instrs` (test 258). The shared-DIV dedup
  (`share_with`) exists specifically to *defer* duplicates to re-detection
  rather than share a rewrite that had no escape analysis. **The lesson: IV-SR's
  correctness lives in the body-scan / escape / dedup logic, not in loop-range
  discovery — so v1 must not perturb those.**

## Step 0: Feasibility / value experiment (decision gate)

Run before writing code; record the matrix here. Unlike the siblings, Step 0's
job is **not** "does the pattern survive to regalloc time" (v1 does not
relocate). It is: *what is the safest, most valuable in-place migration, and is
any relocation possible at all?* Probes on the motivating shapes
(`for(i=0;i<n;i++) sum += arr[i];` accumulator; a `short[]` variant; an
array-copy `b[i]=a[i]`; an MLA-fused index; a multi-DIV struct-field loop),
compiled `-O1`/`-O2`:

1. **Relocation blocker confirmation.** Compile out the tccgen Phase 6 call and
   wire a *relocated* IV-SR into `ir/regalloc.c` after `ssa:loop_const_sim`.
   Confirm that `loop_postinc_fusion` (tccgen 30461) and `loop_bound_remat`
   (tccgen 30467) then run on **un-strength-reduced** loops and produce
   *worse-or-broken* codegen (no latch ADD to fuse, no SP-relative end pointer
   to rematerialize). Expected: confirmed blocker → v1 stays in place. If, and
   only if, this probe shows the cluster is *already* inert at tccgen time
   (e.g. postinc_fusion/bound_remat never fire on the corpus), the relocation
   option reopens — record the evidence, do not assume.
2. **Observability-only sufficiency.** Does adding a `TCC_DISABLE_PASS` name +
   `-dump-ir-passes` integration (v1a, no detector change) already deliver the
   tracker's asks for this entry (isolable, dumpable, bisectable independently
   of `loop_bound_remat`)? Confirm `TCC_DISABLE_PASS=<name>` restores the
   un-reduced loop on the probe and `-fno-iv-strength-red` still gates both
   IV-SR and bound_remat as today.
3. **CFG-detection value/risk (decides whether v1b happens).** Enumerate the
   candidate loops IV-SR currently processes via LICM-loops / `tcc_ir_detect_loops`
   on the corpus and ask: are any of them *false loops* or *external-entry*
   shapes that a dominance-verified detector would reject (the class CFG
   detection fixes)? IV-SR's fuzz history is escape-scan, not false-loop, so the
   expected answer is "few or none." **If the corpus shows no false-loop
   exposure, v1b (CFG detection) is not worth the risk of reconstructing
   `body_instrs` and is dropped** — v1 = observability only. If it shows real
   exposure, scope v1b as a *validation gate* (build a throwaway CFG, and
   *decline* any LICM/detected loop that is not a dominance-verified natural
   loop with a single entry) rather than a full detector replacement — the
   cheaper, safer half of the CFG win that never has to reconstruct
   `body_instrs`.
4. **LICM-loop provenance.** Confirm whether keeping LICM's loop records (the
   `_with_loops` path) vs. self-detection changes results on the corpus. The
   `_with_loops` path exists to avoid an index mismatch; if v1b adds any CFG
   validation, verify it composes with LICM-provided ranges (validate, don't
   rebuild, their `body_instrs`).

Outcomes:

- **(a)** probe 1 confirms the relocation blocker and probe 3 shows no
  false-loop exposure → **v1 = observability-only, in place** (register the
  disable/dump name; no detector change; no retirement). Recommended default.
- **(b)** probe 3 shows real false-loop / external-entry exposure → **v1 =
  observability + a CFG *validation* gate** (decline non-natural / multi-entry
  candidates; keep LICM ranges and `body_instrs` otherwise). Still in place,
  still no retirement.
- **(c)** probe 1 shows the downstream cluster is already inert at tccgen time
  (unexpected) → the relocation option reopens; revise "Deferred v2" into the
  primary path and re-scope with the cluster-migration coupling made explicit.

Record the shape-by-shape matrix here before implementation starts.

### Step 0 Results

**Ran 2026-07-07. Outcome (c) — the downstream cluster had already dissolved, so
full relocation (Deferred v2) became the primary path and was executed.**

The plan's central "v1 cannot relocate" argument rested on **four** downstream
consumers that must run after IV-SR. Re-checking the tree (the plan's line refs
were written 2026-07-06; the tail was heavily reworked 2026-07-07), three of the
four are gone:

| Downstream consumer (plan's blocker) | Status on 2026-07-07 |
|---|---|
| `loop_postinc_fusion` | **Retired** — driver deleted, no source refs |
| `loop_bound_remat` | **Retired** — driver deleted (only a test-comment ref) |
| `decrement_to_zero` | **Relocated** to regalloc as `ssa:decrement_to_zero` |
| `local_alu_cse` | Still at tccgen (Phase 6b feeder), the only real remaining coupling |

So the relocation blocker (probe 1) is *dissolved*, not merely inert: there is no
longer a downstream cluster at tccgen for a relocated IV-SR to run behind.

**`local_alu_cse` coupling (the last thread) — proven benign, no need to
relocate/replicate it.** Empirically and by source audit:

- *After-side covered.* A regalloc-time IV-SR@(after `ssa:loop_unroll`) is
  followed by SSA construction and `tcc_ir_ssa_opt_run`, which runs `ssa:gvn`
  (`ir/opt/ssa_opt_gvn.c`) and `ssa:load_cse` — strictly *more* capable than
  `local_alu_cse` at the loop-IV case, because the IV is SSA-versioned there
  (the flat-IR "multiple defs across the function" limitation `local_alu_cse`'s
  own comment cites no longer applies).
- *Before-side safe.* `local_alu_cse` at tccgen now runs on un-reduced code, but
  the address chains it dedups are exactly the memory-feeding ones IV-SR's escape
  scan **skips** (`transform_derived_iv`, `ir/opt_loop_utils.c:1033-1062`), and
  `find_derived_ivs` requires a real SHL/MUL feeder, so a deduped copy just yields
  fewer DIVs, never a mis-transform. The "MUST run after IV-SR" comment is an
  untriaged conservative guard (commit `e53b23e5`), with no `docs/bugs.md` entry.

**Firing / parity probe (the motivating shapes).** `array_sum` (`sum += arr[i]`)
is byte-identical enabled vs disabled at the *legacy* site too — legacy IV-SR
already leaves `LOAD_INDEXED` (`ldr [rb,rm,lsl#k]`) for the simple accumulator
(consistent with the `loop_bound_remat` retirement finding). Where IV-SR does
fire — the **register-only derived IV** (`258`'s `addr_sum`, address accumulated
never dereferenced) — the relocated pass produces **byte-identical** codegen to
legacy for all four `258` functions (`addr_sum`/`addr_sum_single`/`varargs_fill3`/
`varargs_fill9`), and `TCC_DISABLE_PASS=ssa:iv_strength_reduction` flips the
output (pass is live, not inert). The `258` escape-scan / single-trip
`cmp_stack_addr_fold` soundness (docs/bugs.md #2) holds at the new site because
rotation runs first, so the body is contiguous and dense `body_instrs`
=`[eff_start..eff_end]` fully covers it.

**`body_instrs` reconstruction — trivial, not the net-new risk the plan feared.**
Because the CFG member span is verified *contiguous* (the `unroll`/`dtz`
precedent's check), `body_instrs` is exactly the dense range `[eff_start..eff_end]`
— which is all `find_derived_ivs`, the escape scan, and `APPLY_SHIFT` read. No
`+50` forward-jump heuristic is needed (rotation removed the "detached body after
the back-edge" shape from the regalloc-time input; a non-contiguous survivor is
*declined*, not mis-scanned).

**Result:** implemented the relocation directly (see below), keeping the shared
engine `iv_strength_reduction_core` and the whole mutation/escape/`APPLY_SHIFT`
path frozen. Outermost-only (matching the `unroll`/`dtz`/`const_sim` precedent):
a nested inner loop's array-DIV is a narrowing, not a correctness risk (no fuzz
divergence). Placement: after `ssa:loop_unroll`, before `ssa:decrement_to_zero`
(`ir/regalloc.c`), preserving the legacy "decrement_to_zero after IV-SR" order.

Gates (2026-07-07): `make cross` clean; `make test` 13483 passed / 0 failed;
GCC torture compile 5879 passed / 0 failed; all 8 UT binaries green (UT1 2847
tests, incl. the retargeted driver tests + the docs/bugs.md #2 escape-scan pin
and `share_with` dedup pin); `110`/`258` byte-behavior-identical.

## Replacement Design (in-place v1)

(Assumes Step 0 outcome (a) or (b); under (c) the design is replaced by the
Deferred-v2 relocation variant and re-scoped.)

### Placement: unchanged — tccgen Phase 6

The call site and ordering stay exactly as today
(`tccgen.c:30169-30183`), between LICM (Phase 5) and the downstream cluster.
This is the load-bearing constraint (see "Why this pass is different"). No code
moves to `ir/regalloc.c` in v1.

### Naming, observability, disable knob (the primary v1a deliverable)

- Register the pass with `tcc_ir_opt_pass_disabled(<name>)` at the call site so
  `TCC_DISABLE_PASS=<name>` isolates IV-SR **independently of `loop_bound_remat`**
  (today `-fno-iv-strength-red` kills both). Replace the `CONFIG_TCC_DEBUG`-only
  `ZZ_iv_strength_red` dump with `tcc_ir_dump_after_pass(ir, <name>)` so
  `-dump-ir-passes=<name>` works in normal builds.
- **Name decision (maintainer call, flag in Step 0):** the four siblings use the
  `ssa:` prefix, but that prefix on those passes means "runs in the regalloc-time
  flat region," not "operates on SSA form." An in-place tccgen pass is neither.
  Using `ssa:iv_strength_reduction` here would be doubly misleading. Recommended
  v1 name: **`loop:iv_strength_reduction`** (a `loop:` namespace for in-place
  tccgen loop passes), reserving `ssa:iv_strength_reduction` for the day it
  relocates (Deferred v2). If the maintainer prefers a single namespace, use
  `ssa:iv_strength_reduction` now and accept the naming debt; state the choice
  in the code comment.
- Keep the `-fiv-strength-red` / `opt_iv_strength_red` gate exactly (it also
  still gates `loop_bound_remat`; unchanged). `TCC_LOG_IV_SR` logging unchanged.
- Add the new disable name to `scripts/bisect_opt.py`'s knob list so IV-SR and
  `loop_bound_remat` attribute separately during bisection (today the shared
  `-f` flag conflates them).

### Candidate detection

- **Outcome (a): no change.** Keep `_with_loops(licm_loops)` / self-detecting
  `tcc_ir_detect_loops`; keep `body_instrs` provenance exactly. The migration is
  naming/observability only. This is the recommended, lowest-risk v1.
- **Outcome (b): validation gate only.** Before processing, build a throwaway
  `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` (verify it is callable at
  tccgen time — the flat IR fully exists there; if it is not, fall back to (a))
  and **decline** any LICM/detected loop whose header does not dominate its
  latch or whose header has an out-of-loop predecessor other than the
  preheader (the single-entry fact). Free the CFG before any mutation. Crucially,
  **do not rebuild `body_instrs` from CFG membership** — keep LICM's/`detect_loops`'
  `body_instrs` for every loop that passes validation; the CFG is used only to
  reject, never to construct the range the fragile cores read. This captures the
  false-loop/external-entry win at near-zero risk to the escape scan and shift
  machinery. Share `fie_collect_members` (from `ssa_opt_first_iter_exit`) for
  the membership walk rather than copying it.

### Mutation strategy and staleness — unchanged

The driver's existing "transform one DIV per loop per call, `APPLY_SHIFT` the
rest, `goto try_elim`, re-detect and repeat up to 8×" loop is preserved
verbatim, including the `insert_instr_at` growth, the sibling-record patch, the
`share_with` dedup deferral, and the `try_eliminate_iv_counter` cost heuristic.
A validation gate (outcome b) only prunes the candidate list *before* the first
transform; once a loop is accepted, nothing in the mutation path changes, so the
CFG staleness that plagued relocation candidates never arises (the CFG is freed
before any mutation and the driver re-detects loops between rounds exactly as
today).

### Cleanup cascade — unchanged

No cascade is added or removed. The downstream cluster
(`local_alu_cse` / `loop_postinc_fusion` / `loop_bound_remat` /
`decrement_to_zero`) and the general late-cleanup tail continue to realize
IV-SR's value in tccgen order. This is the whole reason v1 stays in place.

### Interaction with other passes

- **LICM (upstream, still legacy, not in tracker scope):** unchanged provider of
  loop records. Outcome (b) validates but does not rebuild them.
- **`loop_bound_remat` (downstream, separate tracker entry, shared gate):**
  unchanged. When that pass migrates, decouple the shared `opt_iv_strength_red`
  gate so each has its own; note it in the bound_remat plan.
- **`loop_postinc_fusion` / `local_alu_cse` / `decrement_to_zero` (downstream):**
  unchanged; their "must run after IV-SR" ordering is preserved by keeping IV-SR
  in place.
- **The migrated regalloc-time siblings (`ssa:loop_rotate` /
  `ssa:first_iter_exit` / `ssa:ptr_iv_exit_subst` / `ssa:loop_const_sim`):** run
  *after* all of tccgen, hence after IV-SR. `ssa:ptr_iv_exit_subst` rewrites
  post-loop reads of *source-level* pointer IVs; IV-SR creates *compiler-derived*
  stride pointers from array indexing — different vregs, different loops. Confirm
  on `345_ptr_iv_exit_subst.c` (trip >16 to survive unrolling) that IV-SR's
  in-place output does not disturb the ptr_iv shape, and vice versa.

## Required Unit Tests

v1 adds no new mutation surface, so most coverage already exists
(`test_opt_loop_utils.c` core battery, unchanged). Add, in
`tests/unit/arm/armv8m/test_opt_loop.c` alongside the existing driver test:

- **observability (v1a):** a driver test asserting that with the pass disabled
  (`TCC_DISABLE_PASS=<name>` semantics — call through a helper that honors
  `tcc_ir_opt_pass_disabled`, mirroring the sibling wiring) an unrollable/
  reducible array loop is left un-reduced (change count 0, IR unchanged), and
  with it enabled the reduction fires (change count > 0, stride-pointer ADD
  present). This pins the disable knob.
- **validation gate (v1b, only if outcome b):** a synthetic false-loop shape
  (a backward jump that is not a dominance-verified natural loop, seed-278
  class) is declined by the validation gate → `iv_strength_reduction_core`
  returns 0; a side-entry into a non-header member block is declined; a genuine
  natural loop with valid `body_instrs` still reduces (the gate must not
  over-reject). Assert the accepted loop's `body_instrs` is the LICM/detected
  one (validation did not rebuild it).
- **regression pins (no behavior change expected):** re-assert the docs/bugs.md
  #2 escape-scan decline (`test_transform_derived_iv_skips_memory_feeding_div`
  stays green) and the `share_with` dedup path
  (`test_transform_derived_iv_shared_path_refused` stays green) after the front
  end changes.

Refresh the `test_opt_loop` / `test_opt_loop_utils` rows in
`tests/unit/PASS_COVERAGE.md` if the suite grows.

## IR Regression Test

The existing `110_iv_strength_reduction.c` and `258_derived_iv_strength_reduction.c`
are the acceptance corpus and must stay byte-behavior-identical across v1 (v1 is
behavior-preserving). Add one new pin only if v1b changes any candidate outcome:

`tests/ir_tests/347_iv_strength_reduction_ssa.c` + `.expect` (next free number —
346 is reserved by the `loop_unroll` plan; register in `TEST_FILES` in
`tests/ir_tests/test_qemu.py`):

- an array accumulator loop that IV-SR reduces (correct runtime sum), to pin the
  observability wiring end-to-end;
- an MLA-fused index loop (exercising the extended-scan DIV path);
- a multi-DIV struct-field loop (exercising the `share_with` dedup + the
  re-detection loop) whose result must be unchanged from `-O0`.

Added **first**, green under the legacy pass at `-O0/-O1/-O2/-Os`. If Step 0
lands on outcome (a) (observability only), the new IR test is optional and the
existing 110/258 pins suffice — record the decision.

## Migration Steps

Unlike the four siblings, **v1 does not retire the legacy driver** (see Intent).
It is an in-place hardening, so the "delete legacy code" step is absent by
design and deferred to v2.

- [ ] Run Step 0; fill in the matrix; pick outcome (a)/(b)/(c). Steps below
  assume (a)/(b); under (c) switch to the Deferred-v2 relocation design and
  re-scope with the cluster coupling.
- [ ] Decide the pass name (`loop:iv_strength_reduction` recommended vs.
  `ssa:iv_strength_reduction`); record in the call-site comment.
- [ ] **v1a:** register `tcc_ir_opt_pass_disabled(<name>)` at the tccgen Phase 6
  call site; replace `ZZ_iv_strength_red` with
  `tcc_ir_dump_after_pass(ir, <name>)`; add `<name>` to `scripts/bisect_opt.py`.
  Add the observability unit test.
- [ ] **v1b (only if outcome b):** add the CFG *validation* gate in front of
  `iv_strength_reduction_core` (decline non-natural / multi-entry candidates;
  keep LICM `body_instrs`; free CFG before mutation; share `fie_collect_members`).
  Add the validation unit tests. No change to the transform/shift path.
- [ ] Add `347_iv_strength_reduction_ssa.c` (+`.expect`) if v1b changes any
  outcome; register in `TEST_FILES`. Otherwise rely on 110/258.
- [ ] Confirm observability: `-dump-ir-passes=<name>` shows the reduced IR;
  `TCC_DISABLE_PASS=<name>` restores the un-reduced loop; `-fno-iv-strength-red`
  still gates both IV-SR and `loop_bound_remat` (unchanged).
- [ ] Confirm 110/258 and the full `test_opt_loop_utils` core battery are
  byte-behavior-identical (v1 is behavior-preserving).
- [ ] Local gates: unit tests, `make cross -j$(nproc)`, `make test -j16` (incl.
  GCC torture compile).

## Retirement hazard (deferred — NOT part of v1)

Retirement of the legacy IV-SR driver and its removal from tccgen is a **v2
cluster effort**, not this task. Recorded here so the tracker does not
mistakenly tick IV-SR "done" after v1:

- **Downstream-order inversion** — the central blocker. IV-SR cannot leave
  tccgen until `loop_postinc_fusion` and `loop_bound_remat` have SSA/regalloc
  migrations, so the *whole cluster* relocates together preserving the
  "consumers after IV-SR" order. Until then IV-SR stays in place.
- **`body_instrs` reconstruction** — a relocated pass that uses CFG detection
  must rebuild `loop->body_instrs` for `find_derived_ivs`/escape-scan/`APPLY_SHIFT`;
  the ptr_iv precedent (which left it NULL) does not apply. This is net-new
  code that must be validated against the escape-scan fuzz history (test 258).
- **IR-growth side-table remap** — `insert_instr_at` + sibling-record patching
  moves to the regalloc-time model (the `loop_unroll` plan's 1-element `IRLoops`
  wrapper is the precedent). High-risk; separate gate.
- **Shared gate split** — `opt_iv_strength_red` currently gates `loop_bound_remat`
  too; splitting it is part of the bound_remat migration.

## Deferred v2: coordinated cluster relocation (out of scope here)

For completeness — the eventual end state the tracker wants. Once
`loop_postinc_fusion` and `loop_bound_remat` have their own pass plans, migrate
IV-SR + postinc_fusion + bound_remat + (a bounded) local_alu_cse as **one
coordinated block** into the regalloc-time flat region, slotting after
`ssa:loop_const_sim` and before CFG/SSA build, in their current relative order.
That block is where IV-SR finally gets:

- CFG-verified candidates with reconstructed `body_instrs`;
- the `insert_instr_at` growth path under the `loop_unroll` 1-element-`IRLoops`
  precedent;
- the `ssa:iv_strength_reduction` name and full retirement of the tccgen
  drivers.

This is explicitly **not** attempted in v1; it is named here so the sequencing
is on record and IV-SR's tracker checkbox is understood to remain open (v1
delivers observability + hardening, not removal).

## Acceptance

For landing v1 (legacy driver retained, in place):

- [ ] New observability (and, if outcome b, validation) unit tests pass; the
  full existing `test_opt_loop` / `test_opt_loop_utils` battery passes unchanged.
- [ ] `110_iv_strength_reduction.c` and `258_derived_iv_strength_reduction.c`
  pass at `-O0/-O1/-O2` (and `-Os`), byte-behavior-identical to pre-change.
- [ ] `-dump-ir-passes=<name>` and `TCC_DISABLE_PASS=<name>` work;
  `-fno-iv-strength-red` unchanged.
- [ ] `make cross -j$(nproc)`; `make test -j16`; GCC torture suite.
- [ ] **Maintainer-run** (per the 2026-07-06 instruction, after implementation):
  `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0 divergent profiles;
  `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` → 0
  divergences; a run with `TCC_DISABLE_PASS=<name>` must equal today's
  `-fno-iv-strength-red`-off-only-IV-SR behavior on the corpus.
- [ ] Zero new divergences; any divergence triggers fixes before merge.

The parent tracker's `iv_strength_reduction` checkbox stays **open** after v1
(observability + hardening only); it is ticked when the Deferred-v2 cluster
relocation lands and the tccgen drivers are removed.

## Assumptions

- The downstream cluster (`local_alu_cse`, `loop_postinc_fusion`,
  `loop_bound_remat`, `decrement_to_zero`) keeps its current tccgen positions and
  its "after IV-SR" ordering; v1 preserves this by not moving IV-SR.
- `find_induction_vars_ex` / `find_derived_ivs` / `transform_derived_iv` /
  `try_eliminate_iv_counter` / `iv_strength_reduction_core` / `insert_instr_at` /
  `find_loop_exit_condition(_op)` / `compute_trip_count` remain shared utilities
  and are not modified by v1 (the mutation path is frozen).
- LICM continues to provide loop records to the `_with_loops` path; v1b (if
  taken) validates but does not rebuild them or their `body_instrs`.
- `loop_bound_remat` (separate tracker entry, shared `opt_iv_strength_red` gate)
  is not touched here; the gate split happens in its own migration.
- `tcc_ir_cfg_build` is callable at tccgen time (needed only for outcome b); if
  Step 0 shows it is not safely callable pre-regalloc, v1 falls back to
  outcome (a) (observability only) with no candidate change.
