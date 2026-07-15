# Plan: Retire flat-IR scalar passes in favor of their SSA analogs

**Status:** in progress · **Created:** 2026-07-07 · **Updated:** 2026-07-15 · **Branch:** `legacyOptRemoval`

Follow-on to [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md)
(loop passes — done). Scope here: pre-RA flat-IR **scalar** passes in the
`tccgen.c` tail that duplicate an SSA pass already running at `-O1+` via
`tcc_ir_ssa_opt_run()`. Goal: for each pass, reach a terminal state — either the
flat duplicate is **deleted** (SSA subsumes it) or it is **relocated to
`source/opt/`, kept, and made to share one common core with its SSA analog** (SSA
can't subsume it, so it stays — but as a single implementation with two entry
points, DSL-ified, not a duplicate).

**Reference oracle: `-O2` only.** All gap measurement and codegen verification in
this plan use `-O2` as the sole reference — do NOT measure or verify at O0/O1/Os.

Next beat-HEAD fusion work (new session): see
[`plan_ssa_bitfield_fusion_next.md`](plan_ssa_bitfield_fusion_next.md) — (1) SBFX
signed extract (new opcode), (2) Case 3 bitfield-CMP cross-reload bases. Do #2 first.

Out of scope (not listed): post-RA passes (SSA gone), ARM machine fusions
(`lea_*`, `pack64*`, `gens_*`, `bfi`/`ubfx`, …), and frontend memory-init
lowering (`*_memset_to_store`, `memmove_*`, `block_copy`).

---

## Per-pass sequence (state machine)

Run these steps **in order** for each pass, one pass at a time. Each step gates the
next; a pass is finished only when it reaches a terminal state (★).

**Step 1 — Verify the pass is still live / needed.**
Grep the call sites (`source/opt/function_pipeline.c`, `ir/opt.c`, `regalloc.c`
preamble). If the pass is already **retired** (no live call site — body deleted or
stubbed to `return 0`), the task is **done**: mark it, move it to *§ Done*, and
**stop — end of task**. No further steps.

**Step 2 — Measure the SSA gap.** (only if still live)
Measure the pass's true contribution: **stub the body** (not just
`TCC_DISABLE_PASS`, which misses passes called directly from
`function_pipeline.c`), then `compare_worktree --baseline-commit HEAD --opt o2`.
Measure at **`-O2` only** (never O0/O1/Os). The delta decides which branch this
pass takes:
  - **Delta = 0** → SSA already subsumes it → **Branch B** (removal is
    codegen-neutral now; just delete).
  - **Delta > 0, residual is a shape SSA can express** (measured; guard-relaxable
    without re-entering a known fuzz-miscompile class) → **Branch B** (extend SSA
    first, then delete).
  - **Delta > 0, residual depends on fresh pre-SSA IR / same-block adjacency** the
    post-SSA stage can no longer reconstruct → **Branch A** (flat pass must stay).

### Branch A — flat pass must stay (SSA can't subsume it)

The flat pass can't be removed. But keeping two full duplicate implementations
(flat + SSA analog) wastes compiler binary size, so the terminal goal is:
**relocated into `source/opt`, DSL-ified, and sharing a single common core with
its SSA analog** — one implementation, two entry points, zero divergence.

Examples that reached this terminal: `known_bits`, `var_tmp_fwd` (both still owe A4),
`const_prop_tmp` (A4 via shared `_core`), `value_tracking` and `const_var_prop` (A4 N/A —
no shareable SSA core).

- **A1.** Verify unit tests cover the functionality. If not, **write them** under
  `tests/unit/…` and **register in the UT harness** (`make test`).
- **A2.** **Relocate** the function body from `ir/opt_*.c` into the new
  `source/opt/flat/…` structure (+ its header).
- **A3.** **Rewrite to the flat DSL** if the pattern fits (`OPT_GEN_FLAT` peephole,
  or `tcc_ir_opt_run_stateful_gens` for passes carrying function-level state).
- **A4.** **Extract & share the common core.** Analyze the flat pass against its
  SSA analog (the pass running in the SSA opt loop) and factor out the logic they
  duplicate — operand decode, the lattice/analysis, the rewrite rule — into one
  shared helper under `source/opt/…` that **both** call. The flat pass and the SSA
  pass become thin entry points over a single core (single source of truth, zero
  divergence). This deduplication is the size win that justifies keeping the pass.
  Precedent: `const_prop_tmp` exposes an ungated core
  `tcc_ir_opt_const_prop_tmp_core`; the SSA wrapper `ssa_opt_const_prop_tmp` just
  calls it. If the SSA analog has **no** overlapping logic to share (e.g. the flat
  pass computes a lattice SSA doesn't have — why SSA can't subsume it), record that
  and skip — there is nothing to factor.
- **A5.** Confirm the move + share is faithful: **byte-identical codegen** via
  object-diff at **-O2** (ir_tests + torture). ★ **Terminal:** mark *relocated +
  shared, kept flat*; the pass is NOT retired — task ends here.

### Branch B — extend SSA, then retire the flat pass

- **B1.** Write/extend tests for the behavior in the `source/opt/ssa/…` structure
  and register them in the UT harness (`make test`).
- **B2.** Extend the SSA analog until it covers the measured gap.
- **B3.** Remove the flat call site and re-measure (object-diff at **-O2**). Retire
  the flat pass — delete body + prototype + its now-orphaned UT — **only once
  removal introduces no codegen regression** (object-diff neutral at -O2) **and**
  the user's fuzz differential (`diff_olevels`) is clean.
- **B4.** ★ **Terminal:** mark *removed*, move to *§ Done*.

Notes: fuzz differential is **user-run** (never run sweeps/`diff_olevels` here —
gate on `make test` + object-diff delta, then hand off for fuzz). `dce` itself is
retained (cleanup cascade used by many sites, incl. post-RA). Analog names below
are candidates, not proven subsumption — Step 2 decides.

## Status legend

- `[ ] unmeasured` — Step 1/2 not yet done for this pass.
- `[B~] extending` — Branch B in progress: SSA analog being extended.
- `[A★] kept-flat` — Branch A terminal: relocated to `source/opt`, DSL-ified,
  sharing a common core with the SSA analog (A4), kept.
- `[x] removed` — Branch B terminal: flat pass retired; SSA subsumes.

---

## Worklist — remaining passes (in sequence)

Do the clusters top-to-bottom: propagation feeds forwarding feeds the forwarding
drivers; CSE/DCE/specialized folds follow. Within a cluster, order is flexible.

### 1. Propagation / constants — DONE

- `const_prop` — [x] removed
- `const_var_prop` — [A★] kept-flat
- `const_prop_tmp` — [A★] kept-flat
- `value_tracking` — [A★] kept-flat

### 2. Forwarding (scalar)

> **Cluster architecture (learned 2026-07-14, applies to every entry in this cluster).**
> `ssa:var_forward` already exists twice, and which one runs depends on the SSA path a
> function takes:
> - **Promoted path** (`had_promotable != 0` — at least one VAR promoted to SSA/phi) →
>   `tcc_ir_ssa_opt_run` (ir/opt/ssa_opt.c) → runs the **broad**
>   `ssa_opt_var_to_param_forward` (all dominated value positions, all-or-nothing per VAR,
>   NOPs the def).
> - **Fallback path** (nothing promotable) → regalloc.c `RUN_SSA` loop → runs only the
>   **narrow** `ssa_opt_var_forward` (reload-copy shape `Ty <- Vn` only). `ssa_opt_vinfo`/
>   rename are NOT populated here, so vinfo-dependent helpers degrade to no-ops.
>
> Consequence for Step 2: a flat pass's residual value is often concentrated in the
> **fallback path**, and `TCC_DISABLE_PASS=<flat>` does NOT disable flat passes also called
> directly from `source/opt/function_pipeline.c` — **stub the function body** to measure true
> contribution. Flat passes run on **fresh pre-SSA IR**; their same-block/adjacency guards
> (e.g. var_tmp_fwd's seed-814 adjacency) can be both safe and effective there in ways the
> post-SSA analog cannot match without a fuzz-validated guard relaxation. See
> `ssa-var-forward-already-exists-flat-not-retirable` memory.
>
> **Shared blocker:** the whole scalar-forwarding cluster targets a `ssa:var_forward` /
> `ssa:load_cse` analog. `var_tmp_fwd` proved (below, §Done) that the SSA analog can't yet
> match fresh-IR same-block forwarding without an unvalidated seed-814-class guard relaxation.
> Land that guard (fuzz-validated) first; then re-measure this cluster for Branch B removal.

- **`addrof_var_fwd`** → `ssa:load_cse` — **[B~] gap nearly closed 2026-07-15: +130 → +6.**
  The +120/+130 measured earlier was closed in SSA by two extensions landed in `ssa:load_cse`
  (`source/opt/ssa/memory/load_cse.c`) + one in `ssa:dce` (`ir/opt/ssa_opt_dce.c`), all running
  in **both** the main SSA loop and the regalloc fallback loop (so they cover the promoted and
  fallback paths):
  1. **`ssa:load_cse` addrof-const fold** — seed `V <- #imm` writes to an address-taken INT32 VAR
     into the stack-store tracker keyed by the VAR's learned frame offset + base-VAR; both the
     StackLoc-slot **STORE** form and the **ASSIGN** slot-write form seed it. On the read side, a
     TEMP deref whose `ssa_opt_resolve_lea_stackloc_ex` resolves to that `(offset, base_var)`
     forwards the immediate into arithmetic-embedded `*T` operands. **imm-only** (a forwarded TEMP
     extends live ranges into spills — regressed `vector_xor_eq_self`). Guard: a register-form
     write to the VAR flushes its `&V`-keyed slot entry (`sstore_remove_base_var`).
  2. **`ssa:dce:var_live` use-aware veto** — stopped consulting the sticky `iv->addrtaken` flag;
     the pass already re-derives live escapes (`&V` operands + LEA src) per-run, so a VAR whose
     LEA chain is now dead-and-NOP'd becomes sweepable (the old sticky flag pinned its init cruft
     forever). Precedent: legacy `opt_dce.c` did the same via a per-run `var_has_lea` recompute.
  3. SSA global-store DSE for `STORE_INDEXED`-off-hoisted-`GlobalSym`-base was **already present**
     (`ir/opt/ssa_opt_dce.c` `gs_resolve_global_base` / `gs_classify_indexed`) — no change needed.

  Net effect (isolated worktree, HEAD + these two files, `compare_worktree --opt o2`): **−236,
  0 regressions**; the 8-block cleanup repro collapses to one add-chain + one final store pair
  (matching the legacy flat output). Stubbing the flat pass on top now costs only **+6** (was
  +130): `pr57321` +9 (a pointer stored into an **address-taken** VAR then reloaded and
  dereferenced — `ssa_opt_resolve_lea_stackloc_ex` can't chase through the VAR because the
  `vslot_var_forwardable` addr-taken guard blocks forwarding V's pointer value into the reload)
  and `226_fuzz` +2. **Still Branch B, not terminal:** closing the last +6 needs relaxing that
  addr-taken forwarding guard — the fuzz-sensitive seed-814-class relaxation — so the flat pass
  stays until that's fuzz-validated (user-run). `global_sl_fwd`/`iglh` cluster is independent
  (see below).
- **`global_sl_fwd`** → `ssa:var_forward` — **measured 2026-07-14, heavily load-bearing.**
  Step 2 (stub body): **net +10,807** (115 better, 10,922 worse, broad — worst movers spread
  across the whole corpus). Same profile as `known_bits`. Not removable stand-alone; retires only
  with the `addrof_var_fwd` cluster (above), and if that migration can't close a gap this size,
  falls back to Branch A (relocate + keep).
- `ptr_store_load_fwd` — [x] removed
- `diamond_store_fwd` — [x] removed

### 3. Forwarding drivers — after the scalar cluster is deletable

- **`sl_forward`**, **`entry_store_prop`** / **`esp_cleanup`** → `ssa:var_forward` /
  `ssa:load_cse` — **[ ] unmeasured.** The memory-group store→load forwarding drivers. High
  value (largest remaining cluster) but load-bearing — do **after** the propagation cluster
  is deletable so the flat pipeline isn't relied on to feed them. Step 2 each.

### 4. CSE — DONE

- `globalsym_cse` — [A★] kept-flat, stateful DSL

### 5. DCE / DSE

- **`dse`** → `ssa:dce` — **measured 2026-07-15: net +1525 (178 worse, 17 better). Heavily
  load-bearing.** No pre/post-RA split to make: `tcc_ir_opt_dse` (ir/opt_dce.c) is reachable
  **only** via the `"dse"` gated pass name, which runs pre-RA in two places — the main flat
  `late_cleanup` group, and the SSA-region cfg_cleanup re-run of that same group
  (ir/regalloc.c:4874, `cleanup_group = groups[group_count-1]`, still pre-RA). Both honor
  `TCC_DISABLE_PASS=dse`, so the +1525 is the full contribution. The "post-RA dead-store" work
  is a **different** set of functions (`analyze_pure_via_sret` / `compute_func_write_summary` /
  `collect_tu_func_summary` in source/backend/generators/regalloc.c) — out of scope, not this
  pass. So `dse` stays (Branch A/B TBD — needs `ssa:dce` extended to close +1525, else Branch A);
  the earlier "split pre-RA/post-RA" note was a misread of the shared `opt_dead_store` flag.
- `store_redundant` — [x] removed (Step 1 verified 2026-07-15: deleted in e6f0dffd; SSA analog
  is the global-store DSE path in `ir/opt/ssa_opt_dce.c`).
- **`dead_var_store_elim`** → `ssa:dce` — **measured 2026-07-15: net +253 (31 worse, 6 better).**
  Load-bearing, not subsumed by `ssa:dce`; stays (Branch A/B TBD). Pipeline entry
  `dead_var_store` (opt_pipeline.c, opt_dead_store group).
- **`redundant_assign`** (also a §6 late-cleanup copy; registered **twice** — opt_const_prop group
  line 367 **and** opt_dead_store group line 469) — **measured 2026-07-15: net +2350 (54 worse,
  1 better).** Heavily load-bearing (single `TCC_DISABLE_PASS=redundant_assign` disables both
  registrations). Not removable.

### 6. Specialized folds — Step 2 measured 2026-07-15

Cheap Step 2 recipe used (single `make cross` build, no per-pass rebuild): every §6
pass is in the `ir/opt_pipeline.c` `PASS_GATED` table and run only via `run_group`
(none is a direct `function_pipeline.c` call), so `TCC_DISABLE_PASS=<name>` disables
it faithfully — disabling == source removal (`run_group` checks `pass_disabled` per
pass). Measured with `scripts/regression_disasm.py --suite all` (4256 tests /
~20,400 functions, `-O2`): baseline saved with all passes on, then diffed per pass
with the name disabled. **Net = Σ per-function deltas over the run-intersection**
(the raw "Total TCC instructions" line is polluted by ±2 nondeterministic
NEW/REMOVED corpus members — a no-pass-disabled control run measured **net +0** on
the intersection but +31 on the raw total, so the intersection metric is the trusted
one).

> **⚠ Methodology lesson (learned the hard way this session): net-0 on the corpus ≠
> subsumed.** `cmp_offset_fold` measured **net +0 (0 worse, 0 better)** on the whole
> corpus, so it looked like a clean Branch-B delete — but `make test` then failed
> `test_codegen_asm.py::test_cmp_common_base_offset_fold_fires`, whose inline `lt_if`
> /`gt_if`/… functions are **not** in the regression_disasm corpus and depend on the
> fold firing (collapsing `A=B+1; B=B+3; CMP A,B` to drop the `cmp`). The corpus
> gap-measurement is necessary but **not sufficient**; `make test` (which runs
> `test_codegen_asm.py`) is the real neutrality gate. Always run it before deleting a
> net-0 pass.

Step 2 results (net = pass's marginal contribution at -O2; higher = more load-bearing):

| pass | net | worse / better | disposition |
|---|---:|---|---|
| **`neg_chain_cse`** → `ssa:gvn` | **+0** | 0 / 0 | **[x] RETIRED (inert)** — pipeline entry removed; no firing test; target idiom 961126-1 (in corpus) unaffected → `ssa:gvn` subsumes. make test + make ut green. Body/UT kept pending user fuzz. |
| `cmp_offset_fold` → (no SSA analog) | +0 | 0 / 0 | **[A★] STAYS (Branch A terminal).** Corpus net-0 is a **coverage artifact** — the `A=X+K1,B=X+K2` shape isn't in the corpus, only in `asm/cmp_offset_common_base.c`. Empirically load-bearing: disabling reintroduces the `cmp` at **both -O1 AND -O2** (SSA emits `adds/adds/cmp/ite`; it does not reassociate `A−B` to a constant and isn't close). **Not a bug** — a genuine fold SSA doesn't perform. Already relocated+DSL in `source/opt/flat/scalar/cmp_const_offset.c`; A4 N/A (no SSA analog to share a core with). Guarded by `test_cmp_common_base_offset_fold_fires` (-O1). |
| `const_agg_fold` → (no SSA analog) | n/a | — | **[A★] STAYS (Branch A terminal, 2026-07-15).** No SSA analog exists → nothing can subsume it; Branch A by construction (like `cmp_offset_fold`). Plain-move relocation `ir/opt_const_aggregate.c` → `source/opt/flat/scalar/const_aggregate.c` (git mv, code byte-identical). A3 N/A (whole-function multi-pass dataflow — per-slot constant lattice + control-flow JOIN, not an opcode-triggered peephole/stateful-gens shape). A4 N/A (no SSA analog to share a core with). A5: object-diff **0 changed functions** (20,429 byte-identical at -O2 across ir+torture+all suites; the ±2 NEW are the documented harness-nondeterministic corpus members). Gate: `make ut` (`opt_const_aggregate` 15/15) + `make test` (13,635 pass) green. Guarded by `test_opt_const_aggregate.c` (UT) + `tests/ir_tests/172_const_agg_fold.c`. |
| `return_reuse` → `ssa:dce`/backend | +0 | 3 / 3 | net-zero **with churn** (perturbs 6 functions, not neutral). Not a clean delete; needs SSA to cover the 3 it helps before removal is object-diff-neutral. |
| `float_narrow` → `ssa:narrow` | +9 | 2 / 0 | **measured 2026-07-15.** Load-bearing but tiny — the whole residual is one test (`gcc-execute/20030125-1`: `main` +3, `q` +6). Branch B (extend `ssa:narrow` to cover this single soft-FP demote shape) or A — deferred, low ROI. |
| `single_val_tmp` → `ssa:fold`/`cprop` | +22 | 2 / 0 | load-bearing; Branch B (extend SSA) or A. |
| `setif_or_taut` → (no SSA analog) | +28 | 1 / 0 | **[A★] STAYS (Branch A terminal).** Relocated plain-move to `source/opt/flat/scalar/setif_or_taut.c`; raw `tcc_mallocz`/`tcc_free` swapped for `source/memory` `scoped_vector` (view-pointer idiom per `ir/ssa.c`, ownership auto-freed). A3 N/A (the pass's `goto invalidate_writes` catch-all runs for every writing op, incl. guard-failing SETIF/OR — no post-dispatch hook in `run_stateful_gens` to express it), A4 N/A (no SSA OR-chain-tautology analog). Behavior-neutral: `make test` (13635 pass, incl. `test_codegen_asm.py`) + `make ut` (0 failed, `opt_setif_or_taut` suite) green. |
| `self_copy_elim` → `ssa:cprop` | +33 | 5 / 0 | load-bearing. |
| `stack_nonnull` → `ssa:branch`/`sccp` | +39 | 5 / 0 | load-bearing. |
| `self_arith` → `ssa:fold` | +51 | 8 / 0 | load-bearing. |
| `add_reassoc` → `ssa:reassoc` | +59 | 48 / 18 | load-bearing (NOT "closest to deletable" as guessed — real +59, 48 regressions if removed). |
| `cmp_field_fuse` → `ssa:branch` | +134 | 16 / 0 | load-bearing. |
| `float_branch` → `ssa:branch` (soft-FP) | +136 | 18 / 1 | load-bearing. |
| `cmp_expr_fold` → `ssa:branch`/`cmp_eq_prop` | +166 | 13 / 0 | load-bearing (shares UT `test_opt_cmpfold.c` with cmp_offset_fold). |
| `switch_collapse` → `ssa:branch`/`sccp` | +213 | 1 / 0 | load-bearing (few funcs, big each). |
| `vrp` → (no SSA analog) | +418 | 28 / 0 | **[A★] STAYS (Branch A terminal, 2026-07-15).** No SSA range-prop pass exists → nothing can subsume it (like `cmp_offset_fold`/`const_agg_fold`). Load-bearing (+418, worst movers are dead-branch/compare fold: `compare-3`, `990211-1`, `pr28675`, `920624-1`). **Extracted** from the shared `ir/opt_branch.c` (not a whole-file move) into `source/opt/flat/scalar/vrp.c` (byte-identical body + the vrp-only `vrp_get_slot`/`vrp_read_const32`/`vrp_fold_cmp` helpers + `VRPRange`/`VRP_MAX_POS`; `ir_opt_match_zero_test` and the shared `vrp_negate/swap/implies` cmp-tok helpers stay). A3 N/A (whole-function forward range dataflow with merge-point clearing, deferred-range carry, and scoped-equality constraints — not an `OPT_GEN_FLAT`/`run_stateful_gens` peephole), A4 N/A (no SSA analog to share a core). A5: object-diff **0 changed functions** (20,429 byte-identical at -O2; the ±2 NEW `gcc-execute/*::main` are the documented harness-nondeterministic members). Gate: `make ut` (`opt_vrp` 24/24) + `make test` (13,635 pass) green. Guarded by `test_opt_vrp.c` (UT). |
| `bf_insert_extract` → `ssa:narrow` | +644 | 207 / 18 | **measured 2026-07-15.** Heavily load-bearing. Also *pessimizes* 18 funcs (`20040709-2`/`-3` test\*, +2 each) — minor known interaction, dwarfed by the win. If `ssa:narrow` can't close +644 it falls to Branch A. Branch A/B TBD. |
| `setif_fuse` → `ssa:branch` | +2991 | 255 / 6 | heavily load-bearing. |

Step 2 complete for all §6 passes (2026-07-15). None is a clean net-0 delete; all stay
(Branch A/B TBD per row). `const_agg_fold`/`setif_or_taut`/`cmp_offset_fold`/`vrp` already reached
[A★] terminal (see table). The remaining `[A by construction]` rows (no SSA analog) still owe the
relocate loop; the `→ ssa:*` rows are Branch A/B TBD pending SSA extension.

---

## Out of scope — stay flat by design (do NOT port)

These are not part of this plan's worklist and never reach Step 2:

- ARM machine fusions (`fusion_mla`, `deref_indexed`, `disp_fusion`, `chain_fold`,
  `pair_reorder`, `bool_simplify`) — pattern-match physical/TEMP shapes, not an SSA concern.
- VLA / alloca lowering (`dead_vla_struct`, `zero_vla`, `alloca_load_fwd`, `dead_alloca_vreg`)
  and frontend memory-init lowering (`*_memset_to_store`, `memmove_*`, `block_copy`).
- All **post-RA** passes (SSA is gone by then): post-RA `dse` cleanup, post-RA
  `jump_threading` / `orphan_cmp`, branch-size opt, etc.

---

## Done (terminal — retired or relocated-kept)

Full write-ups: git history of this file
(pre-2026-07-15) + memory notes.

### Branch B — retired (SSA subsumes)

- `const_prop` — gap ported to SSA, −133
- `ptr_load_cse` — ssa:load_cse
- `diamond_store_fwd` — ssa port, net +0
- `lea_cse` — ssa:gvn (gvn_try_lea)
- `bool_cse` — ssa:gvn
- `cse_param_add` — ssa:gvn
- `local_alu_cse` — ssa:gvn, net −106
- `redundant_init_elim` — ssa:dce:var_live
- `deref_fwd` — ssa:load_cse subsumes
- `ptr_store_load_fwd` — dead, deleted
- `store_redundant` — dead, deleted (e6f0dffd); SSA global-store DSE analog
- `neg_chain_cse` — ssa:gvn subsumes; pipeline entry removed 2026-07-15 (inert, net-0, make
  test + make ut green). Body `ir/opt_neg_chain.c` + proto `ir/opt.h:258-259` + UT
  `test_opt_neg_chain.c` + `test_metamorphic.c` registration retained pending user fuzz, then delete.

### Branch A — relocated, kept flat

- `known_bits` — stateful DSL; A4 N/A
- `value_tracking` — plain move; A4 N/A
- `const_var_prop` — plain move; A4 N/A
- `const_agg_fold` — plain move (git mv) `ir/opt_const_aggregate.c` →
  `source/opt/flat/scalar/const_aggregate.c` (2026-07-15); A3 N/A (whole-function multi-pass
  dataflow — per-slot constant lattice + CF JOIN, not peephole/stateful-gens), A4 N/A (no SSA
  analog). A5 object-diff 0 changed functions at -O2; `make ut` + `make test` green. UT
  `test_opt_const_aggregate.c` (15/15) kept in place.
- `setif_or_taut` — plain move to `source/opt/flat/scalar/setif_or_taut.c` (2026-07-15); A3 N/A
  (goto-based catch-all invalidation doesn't fit `run_stateful_gens`), A4 N/A (no SSA OR-tautology
  analog). Raw `tcc_mallocz`/`tcc_free` (tbl/block_start/active_pos) swapped for `source/memory`
  `scoped_vector` (view-pointer idiom per `ir/ssa.c`). Behavior-neutral: make test (13635) + make ut
  (0 failed) green.
- `vrp` — **extracted** (not a whole-file move) from the shared `ir/opt_branch.c` into
  `source/opt/flat/scalar/vrp.c` (2026-07-15): byte-identical `tcc_ir_opt_vrp` body + `_ex` wrapper +
  the vrp-only `vrp_get_slot`/`vrp_read_const32`/`vrp_fold_cmp` helpers + `VRPRange`/`VRP_MAX_POS`.
  `opt_branch.c` stays for its other passes (`float_branch`, `stack_addr_nonnull`, `setif_branch_fuse`,
  `stack_bool_diamond`, `or_bool_diamond`, and the shared `ir_opt_match_zero_test`); the three
  `vrp_negate/swap/implies` cmp-tok helpers live in `opt_utils.c` and are shared with `float_branch`, so
  they stay. Wiring differs from a `git mv` (opt_branch.c stays in top `Makefile` IR_FILES + selfhost list):
  added `vrp.c` to `source/opt/flat/Makefile` FLAT_OPT_SRC and to `UT_MODULE_SRCS` in
  `tests/unit/arm/armv8m/Makefile`; prototypes stay in `ir/opt.h`. A3 N/A (whole-function forward range
  dataflow — merge-point clearing, deferred-range carry, scoped-equality constraints — not a
  peephole/stateful-gens shape), A4 N/A (no SSA analog). A5 object-diff 0 changed functions at -O2;
  `make ut` (`opt_vrp` 24/24) + `make test` (13,635) green. UT `test_opt_vrp.c` kept in place.
- `const_prop_tmp` — shares _core; A4 done
- `var_tmp_fwd` — stateful DSL; A4 TODO
  (seed-814 guard blocks §2 removal)
- `globalsym_cse` — stateful flat DSL in `source/opt/flat/memory/symaddr_cse.c`; A4 N/A
  (inline SYMREF materialization has no SSA instruction for GVN to number). Whole-function
  frequency ranking, pressure selection, and entry insertion remain imperative setup; ADD/STORE
  matching and rewriting use `OPT_GEN_FLAT`. The DSL port also ranks every candidate while keeping
  the 16-hoist pressure cap, and recognizes a SYMREF in either ADD operand: **net −1** instruction
  at -O2 (`gcc-execute/memchr-1:test_narrow`, 601 → 600), with no regressions across 4,256
  objects. Rewriting every counted ASSIGN, FUNCPARAMVAL, and STORE-value use was rejected at
  **net +108** instructions (43 better, 151 worse across 37 regressed functions). Moving the pass
  to the late pre-CFG SSA region was rejected at **net +31,071** instructions. Reusing an existing
  dominating entry-block SYMREF materialization was neutral across the corpus and was dropped.

**A3 DSL-port assessment (2026-07-15) — kept-flat dataflow cluster is terminal, no further ports.**
Audited all five kept-flat scalar *dataflow* passes against the peephole/stateful DSL
(`OPT_GEN_FLAT` / `tcc_ir_opt_run_stateful_gens`: opcode-triggered, first-match-wins one-handler-per-op,
`begin`/`each_pre`/`end` hooks). None is portable and none *should* be — the DSL is for opcode peepholes
with optional BB-local state, these are whole-function/merge-aware analyses:
- `const_prop_tmp` — single linear walk but a **5-transform fan-out per instruction**
  (propagate_src1+src2+fold_binop+cmp_setif+softfp) **and** shares `_core` with the SSA analog; a DSL
  port needs a wildcard gen (defeats opcode dispatch) and would fork the shared core. Keep the `_core`.
- `setif_or_taut` — its `if(!handled) invalidate_writes` catch-all must run for **guard-failed**
  SETIF/OR too; first-match dispatch can't run "matched handler + common post-step" without a new
  `each_post` hook + a coordination flag → strictly worse than the current `switch(op)` (A3 N/A confirmed).
- `const_var_prop` (13-scan whole-fn gather-then-rewrite), `value_tracking` (merge map + backward
  `find_defining_instruction` def-scan), `const_agg_fold` (explicit CF JOIN + per-instr `saved[]`
  snapshots) — fundamentally not single-walk peephole material.
Conclusion: peephole-DSL work for this cluster is **complete**; A3 is terminal-N/A for all five. See
memory `flat-scalar-passes-dsl-and-loop-assessment`.

**Loop-structure / compile-time analysis (2026-07-15).** Structural census of the five passes: the
flagged O(n²) sites are all intrinsic or amortized-linear — `const_agg_fold` L308 (param→call search)
is ~O(1) since params sit adjacent to their call; L498 (JOIN) is intrinsic to the pre-CFG merge and
rare; `value_tracking` L1233 (all-NOP scan) is amortized O(n). No hot quadratic worth fixing. One clean,
zero-risk refactor landed: **`const_var_prop` fused its two independent read-scans** (value-reads
building `var_read`/`tmp_read`, and LEA address-takes building `var_addr_taken`) into a single forward
walk — hot pass (every fn at -O1+), object-diff **0 changed functions** at -O2, `make test` 13,635 +
leak-checked UT 2,704 green. (Allocation modernization of these files → `docs/plans/memory_abstraction_port.md`
Phase A.)

Pending gates (user-run fuzz differential):
- diamond_store_fwd, deref_fwd
- known_bits new folds, ssa:dce:var_live
- globalsym_cse relocation
- neg_chain_cse pipeline removal (inert; object-diff neutral at -O2, delete body/UT once fuzz clean)

### Partial (SSA analog live, flat still runs)

- `copy_prop` — loops still flag-guarded
- `string_calls` — ssa:const_string_fold
- `stack_bool` — ssa:or_bool_diamond
