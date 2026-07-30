# Optimizer Pass Deduplication & Compile-Time Plan

**Status**: Phase 0 landed (§7); Phase 1 partially landed (§8); Phases 2–6 not started
**Created**: 2026-07-19
**Scope**: `source/opt/` (261 files, ~78k LOC)
**Goal**: Cut optimizer compile time by not running passes that cannot fire, and
retire ~4–5k LOC of copy-pasted analysis by hoisting it into shared helpers.

This plan is about **the optimizer's own cost and structure**, not about output
code quality. No transform should change its result; every phase below is
expected to be output-neutral (golden-IR diff = 0) unless explicitly noted.

---

## 1. Measured baseline

All numbers below were measured on this tree at `57ebbd0b`, **ASan build**
(`./configure` default). Corpus: the first 300 files of
`tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/compile/`, of
which 276 compile.

### Optimizer share of compile time

```bash
while read f; do ./armv8m-tcc -O$LEVEL -c "$f" -o /dev/null >/dev/null 2>&1; done < corpus.txt
```

| Level | Wall (300 files) |
|-------|------------------|
| `-O0` | 0.41 s |
| `-O1` | 0.88 s |
| `-O2` | 0.92 s |

**The optimization pipeline is ~55% of `-O2` compile wall time** (0.51 s of
0.92 s). It is the single largest phase. ASan inflates absolute numbers; the
*share* is what this plan targets.

### Where that time goes, and how much of it accomplishes anything

```bash
TCC_PASS_TIMING=1 ./armv8m-tcc -vv -O2 -c f.c -o /dev/null   # timing to stdout, traces to stderr
```

`pipeline_trace_pass` (`source/opt/engine/pipeline_run.c:18-24`) prints **only
when a pass returns `changes > 0`**, so cross-referencing the `-vv` trace
against the `TCC_PASS_TIMING` call counts yields a productivity rate per pass.
Over the 276-TU corpus:

| Metric | Value |
|--------|-------|
| Timed pass invocations (excl. `P:requirements`) | **52,373** |
| Invocations that changed anything | **1,139 (2.17%)** |
| Same, excluding `dce` and `const_prop_tmp` | **262 (0.58%)** |
| Instrumented optimizer time | 191.6 ms |
| Time spent by passes that **never** fired, corpus-wide | **55.4 ms (27%)** |

Twenty-five distinct pass names produced **zero** changes across all 276
translation units. The worst offenders:

| Pass | Time | Calls | Productive |
|------|-----:|------:|-----------:|
| `uninit_dom_ret` | 20.7 ms | 1214 | 0 |
| `const_agg_fold` | 6.3 ms | 1214 | 0 |
| `redundant_assign` | 6.0 ms | 1808 | 0 |
| `symref_prop` | 5.6 ms | 1214 | 0 |
| `redundant_var_assign` | 5.5 ms | 1808 | 0 |
| `reroll` | 2.9 ms | 392 | 0 |
| `value_tracking` | 35.8 ms | 2458 | **6 (0.24%)** |
| `known_bits` | 22.2 ms | 1240 | 18 (1.45%) |
| `global_sl_fwd` | 10.9 ms | 2838 | 4 (0.14%) |

`uninit_dom_ret` alone is 10% of instrumented optimizer time and has never once
fired on this corpus.

#### Caveats — read before quoting these numbers

1. **Eight pass names are double-counted.** `dce`, `dse`, `const_prop_tmp`,
   `const_var_prop`, `known_bits`, `value_tracking`, `global_sl_fwd`,
   `sl_forward` are timed both by the pipeline driver
   (`engine/pipeline_run.c:154-164`) and by their own `__timed` wrapper under
   the *same* name. Their µs and call counts are inflated ~2×.
2. **`redundant_assign` and `redundant_var_assign` are the same pass** under two
   names (table name vs. wrapper name), so its 11.4 ms is one pass counted twice.
3. **`-vv` only traces table-level passes.** Work done inside `kb_cascade`,
   `const_cascade`, `branch_cleanup_cascade`, `esp_cleanup`
   (`engine/pipeline_table.c:26-87,186-202`) is timed but never traced, so
   productivity for `dce`/`const_prop_tmp`/`known_bits` is **understated**. The
   zero-productivity list is unaffected — those passes have no cascade path.
4. **Only the flat pipeline is instrumented.** 191.6 ms of an estimated 510 ms
   delta. The SSA driver (`ssa/engine/driver.c`) and the 9-pass loop cluster
   invoked from `ir/regalloc.c:4393-4483` emit no timing at all. Phase 0 fixes this.
5. Compile-only torture corpus. A corpus with more loops would shift `reroll` /
   `licm_ex` productivity.

### Duplication census

A normalized-window clone detector (identifiers kept, literals/strings/numbers
canonicalized, preprocessor and brace-only lines dropped) over all 261 files:

| Window | Cross-file clone groups | Removable LOC (top groups) |
|--------|------------------------:|---------------------------:|
| ≥10 normalized lines | 38 | ~1,083 |
| ≥6 normalized lines | 132 | ~1,671+ |

Largest single groups: `ssa/loop/{decrement_to_zero,iv_strength_reduction,
loop_const_sim,loop_unroll}.c` share 51 lines each (~153 removable);
`flat/dce/{infinite_self_recursion,noreturn_collapse,uninit_ub}.c` ~72;
`flat/memory/invariant_{global_load,temp_deref}_hoist.c` 51 lines **verbatim**.

Structural duplication that the line-window detector cannot see (found by
reading) pushes the realistic total to **~4,000–5,000 LOC**.

---

## 2. Root causes

Three structural facts explain nearly every finding.

**RC1 — The pipeline has no dirty tracking.** `tcc_ir_opt_run_group`
(`engine/pipeline_run.c:143-173`) runs every enabled pass in the group on every
fixpoint iteration. `propagation` is 22 passes × 10 iterations; `memory` is 10
passes × 12 iterations with a `kb_cascade` slot that is itself 8 × 8. A pass
that has been idle for nine rounds still runs in round ten as long as *any*
other pass is still changing something. Only two mechanisms exist today: the
whole-round `round_changes == 0` break (`:182`) and `trigger_idx`, which only
`memory` and `entry_store` set (`:229-238` — `propagation`, `fusion` and
`late_cleanup` all pass `-1`).

**RC2 — `IROptCtx`'s caches are mostly unreachable, so passes re-derive
everything locally.** ✅ Verified: `IR_PASS_REQUIRES_MERGE`, `_BLOCKS` and
`_LOOPS` (`include/opt_pipeline.h:26`) are referenced **only** in their own
definitions and in `pipeline_run.c:41-46` — **no pass table entry sets any of
them**. So `ctx->merge_bitmap`, `ctx->block_starts` and `ctx->loops` are never
populated, and their invalidation bookkeeping is pure overhead. Only 5 fusion
passes set `IR_PASS_REQUIRES_DU` (`pipeline_table.c:121-126`). Meanwhile ✅
`pipeline_apply_invalidations` (`pipeline_run.c:49-53`) is:

```c
if (invalidates) tcc_ir_opt_ctx_invalidate(ctx);   /* ctx->generation++ */
```

— so `INVALIDATES_DU` / `_CFG` / `_LOOPS` / `_ALL` are **runtime-identical**.
Consequence: passes hand-roll def-use tables, merge bitmaps, addr-taken sets and
loop detection inline, which is both the duplication and the quadratic cost.

**RC3 — The SSA-migration drivers were copy-pasted, not shared.** The seven
`ssa/loop/` passes are thin drivers over retained flat engines (by design, per
`docs/plan_legacy_loop_ssa_replacement.md`), but each grew its own private copy
of the candidate enumerator. ✅ Verified: 7 of them call `tcc_ir_cfg_build` +
`tcc_ir_cfg_compute_dominators` inside their own 4-to-8-round convergence loop
(`SSA_*_MAX_PASSES`), discarding the CFG each time — **up to 32 CFG builds plus
32 iterative-dominator computations per function**, none shared, even between
adjacent passes that changed nothing.

---

## 3. Phased plan

Ordered by payoff ÷ risk. Phases 0–3 are compile time; 4–6 are LOC. Each phase
is independently landable.

### Phase 0 — Make the work measurable (prerequisite, ~1 day) ✅ DONE

Nothing below can be validated without this. **Landed — results in §7.**

| Item | Change | Files |
|------|--------|-------|
| 0.1 | Remove the duplicate `__timed` wrappers on the 8 double-counted passes, or rename them so the two layers are distinguishable | `flat/dce/dce.c`, `flat/memory/{dse,sl_forward,var_store_elim}.c`, `flat/scalar/{const_prop_tmp,const_var_prop,known_bits,value_tracking}.c`, `flat/loop/{licm,reroll}.c` |
| 0.2 | Unify the `redundant_assign` / `redundant_var_assign` name collision | `engine/pipeline_table.c:107,176` |
| 0.3 | Wrap the SSA driver pass list and the `ir/regalloc.c:4393-4483` loop cluster in `tcc_pass_timing_add` | `ssa/engine/driver.c:41-117`, `ir/regalloc.c` |
| 0.4 | Emit a productivity column directly: have the driver accumulate `(calls, productive_calls)` per pass so `-vv` cross-referencing is no longer needed | `engine/pass_timing.c`, `engine/pipeline_run.c` |
| 0.5 | Check in the corpus profiling script and the clone detector under `scripts/` | new |

**Verify**: `TCC_PASS_TIMING=1` totals over the corpus stay within noise of the
pre-change 191.6 ms; SSA/loop passes now appear in the dump.

### Phase 1 — Stop running passes that cannot fire (largest compile-time win)

**Partially landed — 1.1, 1.2 and part of 1.7 done; see §8, which corrects the
sizing of 1.2 downward and of 1.7 upward.**

Target: the 27% of instrumented time spent by passes that never change anything,
plus the long tail of sub-1%-productivity passes.

- **1.1 — Fix the cascade reporting blocker first.** `tcc_ir_opt_dce(ir)` at
  `pipeline_table.c:41,193,199` and `tcc_ir_opt_compact_nops(ir)` at `:194,200`
  discard their return values, so a cascade can mutate the IR while returning 0.
  Any dirty-tracking scheme is unsound until these are propagated. *Pure
  bookkeeping change; no behavior difference.*

- **1.2 — Per-pass dirty tracking in the group driver.** `ctx->generation` is
  already the right token: `pipeline_apply_invalidations` bumps it on every
  reported change. Add `uint32_t last_clean_gen[]` per group; skip a pass whose
  previous run returned 0 **and** for which `generation` has not advanced since.
  `engine/pipeline_run.c:143-173`. Given the measured 2.17% productivity this
  should eliminate the large majority of pass invocations.

- **1.3 — Give `propagation` and `late_cleanup` a `trigger_idx`** (e.g. `dce`
  and `branch_cleanup`), matching what `memory` already does, so an idle first
  pass short-circuits the round. `engine/pipeline_table.c:229-238`.

- **1.4 — Make `tcc_ir_opt_ctx_invalidate` bit-selective** (independent
  generation counters per cached analysis), then correct the over-declared
  passes. Reported as genuinely DU-only despite declaring `INVALIDATES_ALL`
  (each verified against the pass body by the audit, **re-verify before
  changing**): `const_agg_fold` (`:104`), `zero_vla` (`:162`),
  `dead_vla_struct` (`:159`), `dead_alloca_vreg` (`:172`), `entry_store`
  (`:205`). Note this buys nothing on its own until 1.2 lands — today all four
  bits collapse to the same `generation++`.

- **1.5 — One `ctx->fn_features` bitmask.** At least 13 passes each do a full
  whole-array walk to answer "does this function contain
  IJUMP / SETJMP / INLINE_ASM / VLA_ALLOC / SWITCH_TABLE / a backward jump?"
  before bailing: `flat/memory/{dead_lea_store.c:69,byte_store_merge.c:856,
  global_base_share.c:91,dead_local_slot.c:89,var_store_elim.c:111,
  sl_forward.c:2879}`, `flat/dce/{zero_vla_elim.c:31,dead_vla.c:362,
  dead_trailing_addrvar_store.c:40,uninit_ub.c:82,dce.c:66,
  body_essential.c:665}`. Compute once per generation next to
  `ctx->merge_bitmap`; each bail becomes a single-bit test. Removes ~13 full
  scans × up to 12 fixpoint iterations.

- **1.6 — Wire up the dead caches.** Set `IR_PASS_REQUIRES_MERGE` / `_BLOCKS` on
  the passes that currently rebuild those bitmaps locally
  (`flat/scalar/{branch_fold.c:65,add_reassoc.c:45,neg_chain_cse.c:86,
  symref_const_prop.c:63,const_prop_tmp.c:551}`, `flat/memory/sl_forward.c:246`,
  `flat/memory/var_store_elim.c:263`) and delete the local builders. **Note a
  latent correctness fix here**: `var_store_elim.c:263-274` omits
  `SWITCH_TABLE` targets that `util/block_scan.c:35-49` handles, so switch case
  labels are currently not treated as merge points there.

- **1.7 — Investigate `uninit_dom_ret` and `value_tracking` specifically.** Two
  passes are 28% of instrumented time between them at ≤0.24% productivity. Either
  add a cheap precondition test (the `fn_features` bit may be enough) or
  reconsider whether they earn their place in the fixpoint at all.

**Risk**: medium. Dirty tracking can mask a real change if any pass mutates
without reporting — 1.1 is the guard, and `make test-golden-ir` is the gate.
**Verify**: golden-IR diff = 0 across the IR suite; corpus instruction count
unchanged; `TCC_PASS_TIMING` total drops.

### Phase 2 — Kill the O(n²) inner loops

`tcc_ir_find_defining_instruction` (`util/vreg_def_use.c:16-31`) is an unbounded
backward scan; `tcc_ir_vreg_has_single_use` (`:33-60`) and
`tcc_ir_vreg_has_single_def` (`util/vreg_query.c:31-52`) are full forward scans.
All three are called *per instruction* at 25+ sites, giving O(n²) per pass ×
up to 10 fixpoint iterations — while `ctx->du` already answers all three in O(1).

- **2.1 — Route the per-instruction def/use queries through `ctx->du`.** Highest
  concentration: `flat/scalar/{cmp_expr_fold.c:202-257 (5 calls per CMP),
  bitfield.c:64-66,156,390,431,465,467, cmp_field_fuse.c:71,97,
  add_reassoc.c:94, var_tmp_fwd.c:166, value_tracking.c:283, branch.c:212}`,
  `flat/memory/rmw_byte_clear.c:81,125,231,415`,
  `flat/fusion/{shift_pair_ubfx.c:78,94, shift64_dead_half.c:112}`. Mark those
  passes `IR_PASS_REQUIRES_DU`.
  **Correctness note**: `tcc_ir_vreg_has_single_use` ignores the MLA
  accumulator operand, which `ir_opt_du_build_mode`
  (`analysis/du_chains.c:114-120`, comment cites fuzz seed 4274) handles
  correctly. Moving to `ctx->du` closes that hole; re-fuzz to confirm no
  behavior change was load-bearing.

- **2.2 — Two-line fix, large effect**: the four backward scans in
  `util/call_params.c:32,71,104,135` walk from `call_idx-1` all the way to 0
  even after leaving the contiguous FUNCPARAM run. `flat/scalar/const_call_fold.c:1061`
  shows the correct early `break`. `flat/scalar/self_copy.c:37-47` calls four of
  them per memcpy call.

- **2.3 — `flat/memory/dse.c` (1225 LOC, ~22 full-array scans per invocation).**
  Fuse the independent inventory scans (`:57,79,112,136,195,334,405,441,471,507,
  518,545,582,619,687,792,937,964,986,1131,1185`) — many collect disjoint facts
  over the same array — and convert the `while (prop_changed) { for i<n }`
  deadness fixpoint at `:1036-1041` to a worklist. Multiplied by 12 memory-group
  iterations this is the single hottest pass body.

- **2.4 — Named quadratic hot spots** (each independently fixable):
  `flat/dce/body_essential.c:401-418` (`ir_opt_backward_jump_has_cond_exit`,
  nests to O(n⁴), called per-instruction from `:514` — needs a def-index table +
  per-back-edge memoization); `flat/memory/local_copy_prop.c:158-190` (O(n³) —
  hoist the memset-call inventory out of the per-chain loop);
  `flat/scalar/call_result.c:137-166` (backward × forward nest);
  `flat/scalar/stack_bool_diamond.c:33,87,132` (two full scans per merge
  candidate, and the pass runs in **both** propagation and memory groups);
  `flat/fusion/lea_rmw_fold.c:51-215` (O(n²·SITES));
  `flat/fusion/lea_fold.c:109,224,264` (three full scans per LEA *despite having
  built a DU at* `:56`); `flat/memory/rmw_byte_clear.c:147-192`;
  `flat/memory/memmove_to_indexed_stores.c` (six whole-array walks per candidate
  call); `flat/scalar/const_aggregate.c:498-521`;
  `flat/loop/iv_analysis.c:319-354,467-484` (plus `getenv("TCC_DBG_MLAIV")`
  called from inside the per-instruction MLA scan at `:387`).

**Verify**: golden-IR diff = 0; per-pass µs drops; fuzz sweep clean (2.1 changes
a DU query's answer set).

### Phase 3 — One CFG for the loop cluster (compile time + ~350 LOC)

- **3.1 — A single `ssa_loop_for_each_outermost(ir, cfg, cb, ctx)` driver.**
  Replaces four verbatim ~60-LOC drivers (`ssa/loop/{loop_unroll.c:82-143,
  decrement_to_zero.c:62-124, iv_strength_reduction.c:82-143,
  loop_const_sim.c:95-157}`) and four verbatim ~40-LOC `try_candidate` prologues
  (`:19-68`, `:18-59`, `:18-70`, `:41-89`), plus the three other header-scan
  spellings (`loop_rotate.c:36-54`, `first_iter_exit.c:473-484`,
  `ptr_iv_exit_subst.c:532-543` — the last two differ by 3 lines). **~350 LOC.**

- **3.2 — Build the CFG + dominators once for the whole cluster.** The 9 loop
  passes at `ir/regalloc.c:4393-4483` currently rebuild up to 32 times. Hoist one
  CFG, pass it down, rebuild only when a pass reports a structural change.

- **3.3 — Compute loop nesting once per CFG build.** The "outermost only" filter
  is O(headers² × V × E): for each header, an inner loop over every other header
  calling `lcs_collect_header_members` (`ssa/loop/loop_cand.c:49-66`), which
  itself flood-fills the whole CFG *and* does a `tcc_malloc`/`tcc_free` per
  invocation. Nesting is a static property of one CFG — O(V+E) per header, once.

- **3.4 — Memoize stack-offset resolution.** `ssa_opt_resolve_lea_stackloc_ex`
  (`ssa/engine/stack_resolve.c:22-99`) is an uncached 64-hop def-chain walk
  called from 42 sites, several inside doubly-nested per-instruction loops
  (`ssa/dce/dead_overwrite_stores.c:208,303`,
  `ssa/memory/load_cse.c:596,622,878,962,988,1461`). A
  `{int32_t cached_stack_off; uint8_t cached_valid;}` pair on `vinfo` is a
  one-field change.

- **3.5 — Cache `has_static_chain` and reachability on `IRSSAOptCtx`.** The
  static-chain bail is a whole-IR scan duplicated 5× (`ssa/dce/{dead_var_stores.c:52,
  stackloc_stores.c:109, ret_path_frame_store.c:41}`, `ssa/scalar/{cprop.c:997,
  gvn.c:882}`), re-run per pass per driver iteration (driver max 5). Two
  independent reachability analyses exist over the same terminators
  (`ssa/dce/unreachable.c:17-93` instruction-level, `ssa/cfg/branch.c:104-187`
  block-level). Also: `ssa/dce/var_liveness.c:136` builds a *fresh* CFG inside a
  DCE sub-pass because `ctx->cfg` is stale — a ctx-level dirty flag fixes it.

- **3.6 — `flat/loop/licm.c` detects loops 3× and builds a CFG once more per
  invocation** (`:1168,1212,1223,1675`), then `ssa_opt_licm` (`:1142-1148`)
  throws the `IRLoops` away.

**Verify**: golden-IR diff = 0; loop-heavy IR tests unchanged; the loop cluster
now shows up in `TCC_PASS_TIMING` (Phase 0.3) with a lower total.

### Phase 4 — Mechanical helper dedup (low risk, ~1,200 LOC)

Pure hoists. Each is a delete-and-call-the-shared-one.

| # | Duplicated thing | Copies | Home | ~LOC |
|---|------------------|-------:|------|-----:|
| 4.1 | `operand_by_slot(q,k)` (src1/src2/MLA-accum dispatch) | 4 fns + 5 inline in flat; 20+ in SSA | `util/` iterator + `SSA_FOR_EACH_SRC` | 380 |
| 4.2 | `max_tmp`/`max_var` prescans that duplicate `ir->next_temporary_variable` / `next_local_variable` | 14 | delete outright | 200 |
| 4.3 | Address-taken VAR bitmap prescan | 5 | `ctx->var_addr_taken` | 150 |
| 4.4 | "instruction touches a volatile sym" | 6 | `util/purity.c` | 110 |
| 4.5 | "collapse body + reset codegen state" epilogue (canonical already at `flat/dce/body_essential.c:425`) | 6 | export it | 110 |
| 4.6 | Integer const-fold binop table | 3 (`const_prop_tmp.c:234`, `known_bits.c:452`, `value_tracking.c:816`) | `util/const_eval.c` | 210 |
| 4.7 | `int64 → IROperand` const materialization | 10 | `util/const_eval.c` | 70 |
| 4.8 | Byte-width-from-btype switch | 4 flat + 4 SSA | one helper, **divergent defaults today** | 80 |
| 4.9 | Condition-token inversion | 6 (`util/cond_util.c:146` is canonical) | delete copies | 60 |
| 4.10 | `SETIF cond-token → constant` fold | 4 | shared helper | 70 |
| 4.11 | LEA-alias map builder (`dead_addrvar_elim.c:34-120` vs `dead_trailing_addrvar_store.c:63-150`, back-to-back in the same group) | 2 | shared | 90 |
| 4.12 | Forward-only-CFG precondition scan (**verbatim**, comment-only diff) | 2 (`invariant_global_load_hoist.c:58` / `invariant_temp_deref_hoist.c:76`) | shared | 34 |
| 4.13 | `ssa_block_for_instr` open-coded, several sites missing the bounds guard | ~20 | export from `ssa/cfg/branch.c:40` | 40 |
| 4.14 | SSA use-list rebuild loop | 3 | export `ssa_opt_rebuild_uses` | 40 |
| 4.15 | "drop the folded CMP's use edges" | 4 | shared | 28 |

**Three of these are latent correctness issues, not just size:**
- 4.6: the three const-fold tables disagree — `const_prop_tmp.c:266` folds `SHL`
  with no `>= width` bail; `known_bits.c:499` returns 0.
- 4.8: `dce_common.c:32` defaults width 0, `stackloc_stores.c:42` defaults 4.
- 4.9: `loop_exit_analysis.c:372-381` handles 6 condition tokens where
  `:131-166` handles 10, silently declining unsigned loops the other accepts.

Also here: **`memset`/`memclr` callee-name recognition at 8 sites with divergent
variant sets** (some miss `__aeabi_memclr4/8`, some miss `__aeabi_memset4/8`) —
contrast the shared `ir_opt_is_memcpy_or_memmove_name` (`util/purity.c:16`).
Fold into a per-generation `ctx->call_kind[]` table, which also subsumes the 22
`get_tok_str` + strcmp cascades re-run every pass, every iteration.

### Phase 5 — Structural dedup (higher risk, ~2,000 LOC)

Ordered by payoff ÷ risk. Each needs its own design note.

- **5.1 — `flat/scalar/addrof_const_fold.c` PARAM/VAR twins.** ✅ Verified: two
  ~400-LOC functions (`:19-417`, `:420-866`) that differ, after normalizing
  identifier prefixes and the vreg-type constant, in **216 of 846 lines** — i.e.
  ~74% identical. Parameterize on vreg type + an optional `init_idx`. ~350 LOC.

- **5.2 — One pointer→(base, offset) resolver.** Seven independent
  implementations in `flat/memory/` alone (`byte_store_merge.c:107` *and* `:369`
  in the same file, `struct_copy_roundtrip_elim.c:57`, `dead_local_slot.c:30`,
  `rmw_byte_clear.c:62`, `dse.c:36`, `memmove_to_indexed_stores.c:116-200`,
  `dead_lea_store.c:108`), with **divergent depth caps** (4 / 32 / unbounded),
  plus 4 more in SSA (`ssa/engine/stack_resolve.c:22,102`,
  `ssa/dce/dead_global_stores.c:341`, `ssa/memory/diamond_store_fwd.c:198`).
  Natural home: `analysis/alias.c`. ~250 LOC + the depth-cap divergence.

- **5.3 — Unify the DSE family.** Four near-identical pending-store skeletons in
  SSA (`ssa/dce/{dead_overwrite_stores.c:157-320,dead_global_stores.c:194-321}`,
  `ssa/memory/{global_store_dse.c:43-127,ptr_store_dse.c:19-107}`) — the
  overlap-kill inner loop alone appears 8×. A shared `SsaStoreKillSet` takes
  ~620 LOC to ~250. The flat side (`flat/memory/dse.c` 1225 +
  `var_store_elim.c` 375 + `dead_static_store.c` 253) runs the same algorithm on
  the other side of the SSA boundary — that convergence belongs to the
  retirement plan, not here.

- **5.4 — `ssa/scalar/cprop.c` re-derives the DSL's own analysis.** `VarFacts` +
  `var_collect_facts` (`:790-876`) computes exactly what
  `opt_dsl_var_imm_state_build` (`framework/opt_dsl_var_const.h:46-140`)
  produces, and is set up identically at `:1005-1015`, `:1063-1072`, `:1157`.
  `ssa/scalar/var_imm_prop.c:51-82` is the 30-line proof the primitive suffices.
  ~250 LOC.

- **5.5 — `ssa/string/` onto `OPT_GEN_SSA`.** All 8 files key on
  `TCCIR_OP_FUNCCALLVAL`; `ssa/scalar/bitop_const_fold.c:111-135` already shows
  the pattern for the same opcode. Plus two shared emit tails
  (`strfold_emit_symref_off` / `strfold_emit_imm`) replacing 6 verbatim copies
  and 7 mechanical `can_fold`/`fold` wrapper pairs. ~200 of the ~800 LOC in the
  directory is boilerplate.

- **5.6 — `ssa/memory/load_cse.c` six per-kind table families.** Six copies of
  `find`/`track_vr`/`track_imm`/`remove_vr`/`invalidate_overlap` over fixed
  arrays (`:88-489`). One generic small-table with a key comparator: ~400 → ~120.
  Do last — largest single file, and its dominator walk
  (`:703-728`, `:1543-1571`) needs a `clone`/`free` hook that `OptSSADomWalk`
  does not yet expose (framework gap, not a pass bug).

- **5.7 — Pass fusion candidates** (fewer full IR walks per iteration, but each
  changes pass ordering — land only with golden-IR proof):
  `symref_prop` + `const_prop_tmp` (same gen-tracked map algorithm over disjoint
  value domains); `self_copy_elim` + `self_arith` (already `IROptGen` tables
  with non-overlapping op keys — concatenation is mechanically safe);
  `bool_simplify` into the shared fusion `run_gens` table;
  `pack64` + `pack64_implicit` + `pack64_from_stack_stores` + `shl32_or_chain`
  (`function_pipeline.c:248-251` — four consecutive passes, four DU rebuilds,
  three keyed on the same `OR`/`SHL #32` probe); and the big one,
  `known_bits` + `value_tracking` (1863 + 1559 LOC, overlapping fact tables over
  the same block structure, both in the 10-iteration loop).

### Phase 6 — Delete dead infrastructure (trivial)

- ✅ `engine/pass_registry.c` — `tcc_opt_dead_code_elimination` (`:24-35`),
  `tcc_opt_constant_folding` (`:37-48`), `tcc_opt_cse` (`:50-54`) are TODO stubs
  returning 0, registered into `builtin_passes` (`:70-99`) and dispatched by a
  second, vestigial pass driver `tcc_optimize_ir` (`:152-188`).
- ✅ `engine/fp_cache_shim.c` — six functions, each calling exactly one
  `fp_mat_cache.c` function with an identical signature. Whole file is deletable
  indirection.
- `ctx->changes` (`engine/ctx.c:33`) — initialized, never read or written.
- `tcc_ir_opt_licm` (`flat/loop/licm.c:1132-1138`) — no production caller (tests
  only), and its return value is `loops->num_loops`, not a change count.
- `IR_PASS_REQUIRES_DU_TMP_ONLY` latch bug (`engine/ctx.c:62-74`): a TMP_ONLY
  pass permanently downgrades every later `REQUIRES_DU` pass, silently returning
  `-1` for VAR/PARAM. Latent only because no table entry uses it today — fix or
  delete the flag.
- `flat/loop/reroll.c:34,37` re-declares `insert_instr_at` / `write_instr_at_nop`
  instead of including `include/opt_loop_utils.h:63,83`.

---

## 4. Explicitly out of scope

This plan does **not** re-propose work already planned elsewhere. Cross-check
before starting any item:

- **Flat→SSA pass retirement** — `docs/plan_legacy_flat_ir_ssa_retire.md`,
  `docs/plan_legacy_loop_ssa_replacement.md` (all 14 per-pass entries closed),
  `docs/plans/legacy_opt_porting_status.md`. The flat↔SSA pairs listed in the
  audit (DSE, `cmp_expr_fold`, `add_reassoc`, VAR-forwarding, `float_narrow`,
  `symaddr_cse`/`global_addr_hoist`) belong to that plan, not this one.
- **`ssa_opt_loop.c` consolidation and pass-invalidation churn** —
  `docs/plans/ssa_opt_loop_consolidation.md` already states the theme; Phase 3
  above supplies the file:line evidence and the CFG-rebuild count it lacked.
- **Output quality / instruction count** — the `codegen-gap-*` work. Nothing
  here should move an instruction count. If it does, that is a bug in the
  refactor, *except* where a plan item explicitly notes a latent correctness fix
  (4.6, 4.8, 4.9, 2.1, 1.6).

One open TODO this plan directly answers: `docs/plan_legacy_loop_ssa_replacement.md`
still has **`[ ] Define the SSA loop-analysis substrate needed by replacements`**
open — that is Phase 3.1, now with a measured size.

---

## 5. Verification protocol

Every phase, in order:

```bash
make clean && make cross -j$(nproc)      # make clean is mandatory after IR-pass edits
make test -j16                           # IR suite — primary gate
make test-golden-ir                      # requires CONFIG_TCC_DEBUG; diff must be 0
make test-gcc-torture-compile
python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu   # zero *new* divergences
```

Plus, for this plan specifically:

- **Compile time**: re-run the corpus O0/O2 wall-clock comparison from §1 and the
  `TCC_PASS_TIMING` aggregate. Record the delta in this file per phase.
- **Output neutrality**: corpus-wide static instruction count must be unchanged.
  A refactor phase that moves it has changed behavior.
- **LOC**: re-run the clone detector (Phase 0.5) and record the group count.

`make clean` before trusting suite results after iterative IR-pass edits — this
has bitten this project before (see the `loop_const_sim` extern-store bug).

---

## 6. Expected outcome

| Phase | Compile time | LOC | Risk |
|-------|-------------|-----|------|
| 0 Measurement | — | +small | none |
| 1 Dirty tracking | **large** — targets 27% pure-waste + 98% idle invocations | small | medium |
| 2 O(n²) removal | large | −200 | medium |
| 3 Loop-cluster CFG sharing | medium–large | −350 | medium |
| 4 Mechanical dedup | small | **−1,200** | low |
| 5 Structural dedup | small–medium | **−2,000** | high |
| 6 Dead code | — | −250 | none |

Phases 1–3 are the compile-time story; 4–6 are the maintainability story. They
are independent — 4 and 6 can land at any time and would make 1–3 easier to read.

---

## 7. Phase 0 results (landed 2026-07-19)

### What changed

| Item | Status | Where |
|------|--------|-------|
| 0.1 duplicate `__timed` wrappers | done — see design note below | `flat/{dce/dce.c,memory/{dse,sl_forward,var_store_elim}.c,scalar/{const_prop_tmp,const_var_prop,known_bits,value_tracking}.c,loop/{licm,reroll}.c}` |
| 0.2 `redundant_assign` / `redundant_var_assign` collision | done — one row under `redundant_assign`, the name used by the pass table, docs and `TCC_DISABLE_PASS` | `flat/memory/var_store_elim.c` |
| 0.3 SSA driver + regalloc loop cluster timed | done — all 25 `ssa:*` driver passes, the second (no-promotable) SSA list, and all 14 flat-region passes now appear | `ssa/engine/driver.c`, `ir/regalloc.c` |
| 0.4 productivity column | done — the driver records `(calls, productive_calls)`; `-vv` cross-referencing is no longer needed | `engine/pass_timing.c` |
| 0.5 scripts checked in | done | `scripts/opt_profile.py`, `scripts/find_clones.py` |

**Design note — why the wrappers were kept, not deleted.** The inner wrappers
are the only thing that sees a cascade-internal call (`kb_cascade` calls
`tcc_ir_opt_dce` directly, never through the pass table), so deleting them would
have re-created caveat #3 rather than fixing caveat #1. Instead
`tcc_pass_timing_begin/end` (`engine/pass_timing.c`) now maintains a frame stack
and:

- **suppresses a nested frame with a name already on the stack**, so a pass timed
  by both the pipeline driver and its own wrapper is counted exactly once; and
- reports **self time** (exclusive of nested timed passes) alongside inclusive
  time, which is what finally makes the cascade slots readable — `kb_cascade` is
  0.017 ms of its own work wrapping 0.117 ms of `dce`/`const_prop_tmp`/`sl_forward`.

`TCC_PASS_TIMED` short-circuits on `tcc_pass_timing_on == 0`, so with timing off
the instrumentation is one predictable branch per pass; corpus wall clock is
unchanged (0.90–0.92 s at `-O2`, inside run-to-run noise).

### Corrections to §1

Re-measured on the same 300-file corpus, **non-ASan** default build (the §1
numbers were ASan, so absolute µs are not comparable — call counts and
productivity are).

1. **Caveat #1 was wrong about `known_bits`.** Seven of the eight named passes
   were genuinely double-counted (`tcc_ir_opt_X_ex` forwards to
   `tcc_ir_opt_X`), but `tcc_ir_opt_known_bits_ex` (`known_bits.c:1838`) is a
   *separate* entry point into the engine that never goes through the timed
   wrapper. Its 22.2 ms / 1240 calls was accurate; the other seven were ~2×.
   Confirmed by the re-measurement: `value_tracking` 2458 → 1244 calls,
   `global_sl_fwd` 2838 → 1624, `known_bits` 1240 → 1240 (unchanged).
2. **Caveat #2 resolved**: 6.0 ms + 5.5 ms across two rows → 5.6 ms in one.
3. **Caveat #3 resolved**: `dce` is now measured at **3701 calls, 1317
   productive (35.6%)** — by far the most productive pass in the tree, and
   invisible to `-vv` before. `const_prop_tmp` is 1296 calls / 2.6%.
4. **Caveat #4 resolved**: the SSA driver and the loop cluster are instrumented.

### New baseline (`-O2`, 300 files, 293 compiled)

| Metric | §1 (flat only, ASan) | Phase 0 (flat + SSA + loops) |
|--------|---------------------:|-----------------------------:|
| Instrumented total | 191.6 ms | **190.3 ms** |
| Timed invocations | 52,373 | **101,733** |
| Productive | 1,139 (2.17%) | **2,180 (2.14%)** |
| Time in passes that never fired | 55.4 ms (27%) | **51.6 ms (27.1%)** |

The productivity rate is unchanged at ~2.1% now that it is measured directly
instead of inferred, which is the main thing Phase 0 had to establish before
Phase 1 can be trusted.

Top consumers (self µs / calls / productive):

| Pass | Self | Calls | Prod |
|------|-----:|------:|-----:|
| `known_bits` | 21.2 ms | 1240 | 18 (1.45%) |
| `uninit_dom_ret` | 20.8 ms | 1214 | **0** |
| `value_tracking` | 17.6 ms | 1244 | 6 (0.48%) |
| `uninit_ub` | 13.2 ms | 1214 | 9 (0.74%) |
| `ssa:dce` | 11.2 ms | 579 | 154 (26.6%) |
| `const_prop_tmp` | 8.0 ms | 1296 | 34 (2.62%) |
| `const_var_prop` | 6.6 ms | 1244 | 31 (2.49%) |
| `const_agg_fold` | 6.0 ms | 1214 | **0** |
| `redundant_assign` | 5.6 ms | 1808 | **0** |
| `symref_prop` | 5.4 ms | 1214 | **0** |

### New findings the SSA-side instrumentation exposed

**The flat-region loop cluster is ~5.7 ms for almost nothing.** Now visible for
the first time, and it sharpens Phase 3's case considerably:

| Pass | Self | Calls | Prod |
|------|-----:|------:|-----:|
| `ssa:reroll` | 2.81 ms | 392 | **0** |
| `ssa:licm` | 0.82 ms | 392 | 80 (20.4%) |
| `ssa:loop_rotate` | 0.66 ms | 392 | 1 (0.26%) |
| `ssa:stack_addr_simplify` | 0.46 ms | 392 | **0** |
| `ssa:or_bool_diamond` | 0.34 ms | 392 | **0** |
| `ssa:ptr_iv_exit_subst` | 0.27 ms | 392 | **0** |
| `ssa:iv_strength_reduction` | 0.27 ms | 392 | **0** |
| `ssa:first_iter_exit` | 0.26 ms | 392 | **0** |
| `ssa:loop_unroll` | 0.23 ms | 392 | **0** |
| `ssa:loop_const_sim` | 0.21 ms | 392 | **0** |
| `ssa:decrement_to_zero` | 0.17 ms | 392 | **0** |
| `ssa:mem_init` | 0.17 ms | 392 | **0** |
| `ssa:struct_copy_roundtrip` | 0.10 ms | 392 | **0** |

`ssa:reroll` alone costs more than the other twelve combined and has never
fired — it belongs on the Phase 1.7 list next to `uninit_dom_ret`. Note this is
a *compile-only* corpus (§1 caveat 5): the loop passes would score better on a
loop-heavy corpus, so treat "0 productive" here as "needs a cheap precondition
test", not "delete".

Zero-productivity SSA passes outside the loop cluster: `ssa:var_const_fold`
(0.62 ms), `ssa:diamond_store_fwd` (0.55 ms), `ssa_string_fold` (0.55 ms),
`ssa:var_imm_prop` (0.40 ms), `ssa:guard_collapse` (0.30 ms),
`ssa:bitop_const_fold` (0.27 ms), `ssa:const_string_fold` /
`ssa:setif_or_taut` (0.21 ms each), `ssa:cmp_offset_fold` (0.22 ms),
`ssa:dead_loop` (0.16 ms), `ssa:cmp_eq_prop` (0.14 ms).

`P:requirements` is **39,616 calls for 0.77 ms total** — the analysis caches are
cheap because they are almost never populated (RC2). That is *not* an argument
against 1.6: it means wiring up the dead caches should be close to free.

### Verification

- `make clean && make cross` clean.
- `make test -j16`: **13645 passed, 161 skipped, 1 xfailed**.
- `make test-gcc-torture-compile`: **5879 passed, 114 skipped**.
- `make test-golden-ir`: 9 failed / 16 passed — **identical set and identical
  diffs at HEAD with the changes stashed**, i.e. pre-existing and unrelated
  (`ssa:load_cse/repeated_load`, `branch_fold_2x/double_constant_diamond`,
  `esp_cleanup/inlined_struct_field_check`, `ssa:var_const_fold/simple`,
  `ssa:branch/branch_fold`, `ssa:cmp_eq_prop/simple`,
  `block_copy_init/clear_struct`, `ssa:phi_simplify/simple`,
  `ssa:dead_loop/simple`). **These must be fixed or re-baselined before
  Phase 1**, since golden-IR diff = 0 is the stated gate for the phases that
  actually change what runs.
- `scripts/diff_olevels.py --seeds 0-2000 --require-qemu`: 4 divergences
  (seeds 945, 1356, 1759, 1795), byte-identical checksums at HEAD — zero new.
- Corpus wall clock: `-O0` 0.40 s, `-O1` 0.87 s, `-O2` 0.92 s
  (HEAD: 0.40 / 0.86 / 0.91).

### Reproducing

```bash
ls tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/compile/*.c \
   | head -300 > corpus.txt
scripts/opt_profile.py --cc ./armv8m-tcc --corpus corpus.txt --levels 0,1,2
scripts/find_clones.py source/opt --window 10 --top 25
```

`find_clones.py` on `source/opt` at `--window 10` reports **52 groups /
~930 removable lines**, and its top hits independently reproduce items 4.9
(`util/cond_util.c:150` vs `flat/loop/loop_exit_analysis.c:28`) and 4.11
(`dead_addrvar_elim.c:35` vs `dead_trailing_addrvar_store.c:64`).

---

## 8. Phase 1 results, part 1 (landed 2026-07-19)

### What changed

**1.1 — cascade mutations now reach `ctx->generation`.** `CASCADE_END`
(`engine/pipeline_table.c`) is applied to `kb_cascade`,
`branch_cleanup_cascade`, `esp_cleanup` and the `memory` trigger. Their
`dce`/`compact_nops`/`eliminate_fallthrough` results are accumulated into a
`silent` counter kept *out* of the returned change count — adding them to the
count would have kept the outer group spinning and changed output — and the
cascade invalidates the ctx itself when it mutated but reported 0.

**1.2 — per-pass dirty tracking** in `tcc_ir_opt_run_group`
(`engine/pipeline_run.c`): `last_clean_gen[]` per group, skip when a pass
previously returned 0 and `ctx->generation` has not advanced. A skipped pass
still calls `tcc_ir_dump_after_pass`, so golden-IR sees exactly what running it
would have produced — the skip is *proved* output-neutral rather than assumed.

**`pipeline_apply_invalidations` now invalidates unconditionally.** Discovered
via `test_run_group_trigger_present_ignores_zero_round_changes_shortcut`, which
correctly failed: a pass that *reports* a change has mutated the IR whatever its
declared `invalidates` mask says, so keying the generation bump on that mask
made the dirty tracking depend on every table entry having an accurate one. The
four bits are runtime-identical anyway (RC2). This is a prerequisite for 1.4,
not an obstacle to it.

**1.7 (partial) — bail ordering in the two uninit-UB passes.** Every guard in
these passes is a pure predicate over unmodified IR, so they may run in any
order, and the passes fire on ~0% of real functions. Both were running their
most expensive guards first:

- `uninit_local_ub`: the decisive scan only ever examines the *entry block*, but
  sat behind two whole-function prescans. Extracted
  `udr_entry_block_reads_uninit(ir, addr_taken)`, called first with `NULL` for a
  superset pre-filter; only survivors pay for `udr_has_inline_asm_or_ijump` and
  `udr_prescan_addr_taken`.
- `uninit_dominates_return`: moved the decisive uninit-read scan ahead of the
  `has_return` walk, the inline-asm/IJUMP walk and
  `udr_has_observable_side_effects` (which does a per-instruction symbol lookup
  for its volatile test).

### Measured effect

| Metric | Phase 0 | Phase 1 (this increment) |
|--------|--------:|-------------------------:|
| Instrumented total | 190.3 ms | **179.2 ms (−5.8%)** |
| Timed invocations | 101,733 | 100,621 (−1.1%) |
| Productive | 2,180 (2.14%) | 2,180 (2.17%) |
| `uninit_ub` | 13.2 ms | **~2 ms** (off the top-20) |
| `uninit_dom_ret` | 20.8 ms | **16.8 ms** |

### Correction to the plan's sizing

**1.2 is a much smaller lever than §3 estimated, and the profile says why.**
The top passes show **~1210 calls against ~1214 functions in the corpus** — they
run *once per function*. The `propagation` group already converges in a single
round for almost every TU, so there are no repeat invocations to skip.
`ctx->generation` is also a *global* dirty bit: any change by any pass in a
round invalidates every other pass's clean record, so the skip only ever catches
the tail of the final round. Measured saving: **1.1% of invocations**, not the
"large majority" §3 projected.

1.2 is still worth keeping — it is sound, free, and becomes more effective once
1.4 makes invalidation bit-selective — but **it is not the compile-time story**.

**The real lever is per-call preconditions (1.5/1.7), not repeat-skipping.**
The 27% figure was never about passes running repeatedly; it is about a *single*
whole-function analysis per function that cannot fire. Reordering guards in two
passes recovered 11 ms (5.8%) by itself, without touching a single algorithm.
The same "always-full-scan guard placed ahead of a cheap decisive one" shape
should be checked in the remaining zero-productivity passes: `const_agg_fold`
(6.1 ms), `redundant_assign` (5.5 ms), `symref_prop` (5.3 ms).

The two biggest consumers are now `known_bits` (22.1 ms, 1.48% productive) and
`value_tracking` (17.5 ms, 0.50%) — **39.6 ms, 22% of instrumented time, between
them**. Neither has a cheap precondition to add; they are genuine whole-function
dataflow. That makes plan item **5.7's `known_bits` + `value_tracking` fusion the
single largest remaining compile-time item**, and it should be re-ranked out of
"Phase 5, high risk, small–medium payoff" accordingly.

### Verification

- `make test-golden-ir`: **25 passed** (the 9 pre-existing failures noted in §7
  were fixed separately before this work started).
- `make test -j16`: **13,645 passed**, 161 skipped, 1 xfailed.
- `make ut -j16`: clean (exit 0) — after fixing the invalidation invariant the
  orchestration test exposed.
- `make test-gcc-torture-compile`: **5,879 passed**, 114 skipped.
- `scripts/diff_olevels.py --seeds 0-2000 --require-qemu`: 4 divergences
  (945, 1356, 1759, 1795), checksums byte-identical to HEAD — zero new.
- **Output neutrality, directly proven**: all 300 corpus TUs compiled to `.o`
  at `-O2` with and without this change; `sha256sum` over the 293 objects that
  compile is **identical in every case (0 differing objects)**.
- Corpus wall clock unchanged at `-O0` 0.39 s / `-O1` 0.87 s / `-O2` 0.90 s:
  the optimizer is ~55% of `-O2` wall time and this is a 5.8% cut of that, i.e.
  ~3% of total — below the run-to-run noise floor of this harness. Per-pass µs
  is the honest metric for increments of this size.

### Remaining in Phase 1

1.3 (triggers for `propagation`/`late_cleanup`), 1.4 (bit-selective
invalidation), 1.5 (`ctx->fn_features`), and the rest of 1.7. Given the finding
above, 1.5 should be re-scoped: its value is not "13 passes × 12 iterations" (the
iterations do not happen) but "13 passes × 1 call per function", and the shared
bitmask has to be reachable from passes that take `TCCIRState *`, not
`IROptCtx *` — the `_ex` wrappers are the natural seam.
