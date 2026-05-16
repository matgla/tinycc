# Pre-SSA Optimization: Engine + Modularization Plan

## Progress checklist

### Phase 0 — Delete dead code
- [x] Remove `tcc_ir_opt_run_by_name` stub (opt.c, opt.h)
- [x] Remove `tcc_ir_opt_run_all` stub (opt.c, opt.h)
- [x] Remove `tcc_ir_opt_return` stub + call site in tccgen.c
- [x] Remove `opt_return_value` flag (tcc.h, libtcc.c) — was the only consumer of the deleted stub

### Phase 1 — Extract shared analysis & primitives
- [x] **1.1** `ir/opt_du.{h,c}` — `IROptDU` + `ir_opt_du_build/idx/def/uses`
- [x] **1.2** `ir/opt_xform.{h,c}` — `ir_xform_nop` (inline), `ir_xform_same_block` (5/6 call sites migrated; 1 site keeps non-canonical NOP-boundary semantics)
- [x] **1.3** `ir/opt_utils.{h,c}` — constant evaluators, BB/CFG helpers, purity tables, expression equality, call-param helpers
- [x] **1.4** `ir/opt_alias.{h,c}` — stack-slot aliasing helpers
- [x] **1.5** `ir/opt_loop_utils.{h,c}` — IV analysis, loop bounds, loop transforms

### Phase 2 — Build the pre-SSA engine
- [x] **2.1** `ir/opt_engine.{h,c}` — `IROptCtx`, `IROptGen`, `tcc_ir_opt_run_gens`, lazy analysis cache
- [x] **2.2** Build-only verify (no rules wired yet)

### Phase 3 — Convert pass groups to generator tables
- [x] **3.1** Fusion group → `ir/opt_gens_fusion.c` (7 converted: rotate, mla, indexed_mem, deref_indexed, disp, indexed_chain, indexed_pair_reorder; hand-written: postinc, lea_fold, assign_fuse)
- [ ] **3.2** Branch-folding group → `ir/opt_gens_branch.c` (branch_folding, setif_branch_fuse, or_bool_diamond, parts of stack_addr_nonnull_fold) — deferred: called from 10+ pipeline locations, complex wiring
- [x] **3.3** Boolean simplification → `ir/opt_gens_bool.c` (bool_idempotent + bool_simplify + idempotent half of bool_pass)
- [ ] **3.4** BB-scoped hash CSE rewrites to use `IROptHashTable` (cse_global_load, globalsym_cse, cse_param_add, local_load_cse, local_alu_cse, stackoff_addr_cse, cse_bool)
- [x] **3.5** Call-result dead group → `ir/opt_gens_call_result.c` (dead_call_result_elim, dead_sret_call_elim, fold_call_result_store converted; dead_init_via_call stays in opt.c — FWS dependency)

### Phase 4 — Generic hash table
- [ ] **4.1** `ir/opt_hash.{h,c}` — `IROptHashTable`, bump-allocated entry pool, drop-in replacement for 3 of 4 hand-rolled CSE tables (skip `sl_forward`'s alias-aware table)

### Phase 5 — Collect-then-transform engine variant (optional)
- [ ] **5.1** `IROptCollectGen` 2-phase dispatch

### Phase 6 — Theme-based file split (optional, zero flash savings)
- [ ] **6.1** `opt_cleanup.c`, `opt_constprop.c`, `opt_memory.c`, `opt_loop.c`, `opt_promote.c`, `opt_peephole.c`

---

## Current State (2026-05)

`ir/opt.c` is **28,973 lines** containing **81 pass functions**. It is the single largest source file in the project. The SSA optimization engine (`ir/opt/`, 8,500 lines across 13 files) has been built and runs on SSA-renamed IR before SSA destruction — but it did **not** displace the pre-SSA monolith. Both layers exist in production and the pre-SSA layer keeps growing as new post-destruction peepholes are needed for address materialization, indexed-mode fusion, and stack-aware patterns.

### Why the monolith keeps growing

The expectation in the original plan — "as SSA passes mature, pre-SSA equivalents are removed" — has not held. The pre-SSA layer operates on flat IR after SSA destruction, where vregs are no longer single-assignment and stack/local layout is materialized. Several optimization classes only make sense at this layer:

- ARM addressing-mode fusion (`LOAD_INDEXED`, `LOAD_POSTINC`, `MLA`, displacement folding)
- Stack-slot aliasing and forwarding (`sl_forward`, `stack_addr_cse`)
- 64-bit register-pair tracking (`pack64`, `pack64_tautology`)
- Call-result lifetime analysis (`dead_call_result_elim`, `dead_init_via_call`, `dead_sret_call_elim`, `fold_call_result_store`)

Since the original plan was written, 21 new pre-SSA passes have been added (full list in the census below). The pre-SSA optimizer is **permanent infrastructure**, not a migration bridge.

### Two goals driving this rewrite

1. **Save flash memory.** The compiler ships on flash-constrained embedded targets. Each pass has ~30–50 lines of duplicated iteration boilerplate (forward loop, NOP skip, BB-boundary check, local DU-table build). Across 81 passes that's roughly **3,000–4,000 lines** of redundant code, plus 4 hand-rolled hash tables and 6+ inlined "same-block check" loops.
2. **Combine passes into single forward loops.** Many passes only differ in their trigger opcode and pattern body. Today the pipeline runs 7+ separate fusion forward-scans back-to-back (each rebuilding the DU table); they could all run in one scan.

The SSA engine has already proven the answer: a generator-based dispatch (`IRSSAOptGen` in [ir/opt/ssa_opt.h:62-66](ir/opt/ssa_opt.h#L62-L66), `ssa_opt_run_gens` in [ir/opt/ssa_opt.c:604-622](ir/opt/ssa_opt.c#L604-L622)) lets a single `O(n)` engine pass dispatch dozens of rules. The pre-SSA layer needs the same shape, with a context that survives the dispatch loop and caches analyses.

---

## Pass Census (current)

`opt.c` pass functions, grouped by pattern affinity:

### Cleanup / DCE
`dce`, `compact_nops`, `dead_var_store_elim`, `dead_addrvar_elim`, `redundant_var_assign`, `redundant_init_elim`, `dse`, `dead_loop_elim`, `dead_call_result_elim`, `dead_init_via_call`, `dead_sret_call_elim`

### Constant / value propagation
`const_var_prop`, `global_init_prop`, `const_prop`, `const_prop_tmp`, `value_tracking`, `complex_const_param_fold`, `param_addrof_const_fold`, `local_addrof_const_fold`, `add_reassoc`, `cmp_expr_fold`

### Memory
`sl_forward`, `entry_store_prop`, `store_redundant`, `block_copy_init`, `deref_fwd`, `fold_call_result_store`

### Fusion & addressing
`fusion_pass` (mla+indexed), `rotate_fusion`, `deref_indexed_fusion`, `disp_fusion`, `lea_fold`, `postinc_fusion`, `loop_postinc_fusion`, `indexed_chain`, `indexed_pair_reorder`, `add_deref_fold`, `stackoff_addr_cse`, `call_chain_rename`, `assign_fuse`

### CSE / copy propagation
`copy_prop`, `cse_global_load`, `globalsym_cse`, `cse_param_add`, `local_load_cse`, `local_alu_cse`, `stack_addr_cse`

### Branch / boolean
`branch_folding`, `setif_branch_fuse`, `stack_addr_nonnull_fold`, `stack_bool_diamond`, `or_bool_diamond`, `nonneg_branch_fold`, `float_branch_fold`, `bool_idempotent`, `bool_simplify`, `bool_pass`

### Loop
`loop_unroll`, `loop_rotation`, `loop_bound_remat`, `iv_strength_reduction`, `iv_strength_reduction_with_loops`, `decrement_to_zero`, `redundant_loop_check`, `backedge_phi_hoist`

### Other / peephole
`vrp`, `var_tmp_fwd`, `var_to_tmp`, `float_narrowing`, `strength_reduction`, `select`, `postinc_assign_fold`, `returnvalue_merge`, `const_string_calls`, `const_call_replace`, `pack64`, `pack64_tautology`, `fp_cache_*`

### Stubs (delete in Phase 0)
`tcc_ir_opt_return`, `tcc_ir_opt_run_by_name`

The original plan's `tcc_ir_opt_run_all` is already gone. `opt_jump_thread.c` already lives outside `opt.c` and provides `tcc_ir_opt_jump_threading` + `tcc_ir_opt_eliminate_fallthrough`.

---

## Architecture: mirror the SSA engine for pre-SSA

```
┌──────────────────────────── Pipeline (tccgen.c) ─────────────────────────────┐
│                                                                              │
│  SSA layer:  IRSSAOptCtx + IRSSAOptGen + ssa_opt_run_gens()                  │
│              ✓ shipped: 13 passes, generator-based dispatch                  │
│                                                                              │
│  Pre-SSA layer (this plan):                                                  │
│              IROptCtx    + IROptGen    + tcc_ir_opt_run_gens()               │
│              one engine, ~25 fusion/branch/bool peepholes registered as gens │
│              ~55 remaining passes call into shared infra but stay bespoke    │
│                                                                              │
├─────────────────────────── Shared analysis cache ────────────────────────────┤
│   IROptCtx { du, bb_starts, pred_count, merge_bitmap } — lazy, generational  │
├─────────────────────────────── Libraries ────────────────────────────────────┤
│   opt_du   opt_utils   opt_alias   opt_loop_utils   opt_hash   opt_xform     │
├──────────────────────────────── IR core ─────────────────────────────────────┤
│           core.c  ir.h  cfg.c  ssa.c  vreg.c  pool.c  machine_op.c           │
└──────────────────────────────────────────────────────────────────────────────┘
```

The pre-SSA engine deliberately mirrors the SSA engine's type and function naming:

| SSA layer                | Pre-SSA mirror             |
|--------------------------|----------------------------|
| `IRSSAOptCtx`            | `IROptCtx`                 |
| `IRSSAOptGen`            | `IROptGen`                 |
| `ssa_opt_run_gens()`     | `tcc_ir_opt_run_gens()`    |
| `ssa_gen_*` functions    | `ir_gen_*` functions       |
| `ssa_opt_<pass>()`       | `tcc_ir_opt_<pass>()`      |

Contributors who know one layer learn the other for free, and one implementation informs the other.

---

## Flash savings estimate

| Source of saving                                                        | Approx. lines removed |
|-------------------------------------------------------------------------|-----------------------|
| Iteration-loop boilerplate deduplicated across ~25 peephole passes      | ~2,500                |
| DU-table builds: 20+ inline `ir_opt_du_build` call-sites → cache lookup | ~300                  |
| Same-block check: 6+ inlined `for (j=...) if (JUMP/JUMPIF)` loops       | ~200                  |
| Pool-slot grow loops in fusion passes (`while (count <= n) pool_add`)   | ~100                  |
| `IROptHashTable` collapsing 4 hand-rolled CSE hash tables               | ~400                  |
| Constants in 2 idempotent/simplify boolean passes merged into one scan  | ~150                  |
| Branch-folding family (5 JUMPIF-triggered passes) merged into one scan  | ~400                  |
| **Total estimate**                                                      | **~4,000 lines (~14% of opt.c)** |

Conservative because it counts only what duplication clearly costs; the engine creates new abstraction surface (~600 lines) that must be subtracted. **Net ~3,400 lines / ~12%.**

The other win — not visible in line count — is **fewer O(n) scans** through the IR. The fusion group alone goes from 7+ separate forward scans (each rebuilding DU) to 1 scan with 1 DU build. For a function with 10,000 instructions that's 60,000–70,000 fewer dispatch-loop iterations per compile.

---

## Migration phases

The phase order has changed from the original plan. **Engine work goes first** because it produces all the flash savings; theme-based file splitting goes last because it produces zero flash savings (only readability).

### Phase 0 — Delete dead code (15 min)

1. Remove `tcc_ir_opt_run_by_name` ([opt.c:15131](ir/opt.c#L15131)) — empty stub.
2. Remove `tcc_ir_opt_return` ([opt.c:11202](ir/opt.c#L11202)) — 5-line stub never called from any pipeline path that needs it.
3. Delete `ir/opt_embedded_deref.c` if still present on disk (orphaned, not in `Makefile`).
4. Remove matching declarations from `ir/opt.h`.

**Verify:** `make cross && make test -j16`.

---

### Phase 1 — Extract shared analysis & primitives (4–6 h)

This is the highest-leverage phase for flash savings. All subsequent phases depend on the libraries created here.

#### 1.1 `ir/opt_du.h` + `ir/opt_du.c` (~200 lines)
- Move `IROptDU`, `ir_opt_du_build/def/uses/idx` from `opt.c`.
- Used by 20+ pass sites today; each currently writes its own `IROptDU du; ir_opt_du_build(ir, &du); …; tcc_free(du.def)` block (~10–15 lines per site).
- After extraction these collapse to `const IROptDU *du = ir_opt_ctx_require_du(&ctx);`.

#### 1.2 `ir/opt_xform.h` + `ir/opt_xform.c` (~150 lines)
Six primitives, mirrors the most-duplicated patterns:
```c
static inline void ir_xform_nop(TCCIRState *ir, int idx);          /* 81 sites */
void ir_xform_replace_with_assign(TCCIRState *ir, int idx, IROperand src); /* ~40 sites */
void ir_xform_replace_with_imm(TCCIRState *ir, int idx, int64_t v, int btype);
int  ir_xform_same_block(TCCIRState *ir, int from, int to);        /* 6+ sites */
int  ir_xform_alloc_pool(TCCIRState *ir, int n_slots);             /* every fusion pass */
void ir_xform_nop_with_du(TCCIRState *ir, int idx, IROptDU *du);
```

#### 1.3 `ir/opt_utils.h` + `ir/opt_utils.c` (~1,500 lines)
Extract from `opt.c`:
- Constant evaluators: `ir_opt_eval_const_u64`, `ir_opt_eval_const_string`, `evaluate_compare_condition`, `is_power_of_2`, condition-token helpers (`invert_cond_token`, `vrp_swap_cmp_tok`, `vrp_negate_cmp_tok`).
- BB / CFG helpers: `ir_opt_build_merge_bitmap`, `ir_opt_mark_block_starts`, `ir_opt_next_non_nop`, `ir_skip_nops_forward`, `ir_has_other_jump_to`, `ir_negate_condition`, `invert_condition`.
- Purity tables: `ir_opt_is_pure_helper_name`, `ir_opt_is_flag_cmp_helper_name`, `ir_opt_is_pure_fallthrough_instruction`, `tcc_ir_is_pure_aeabi`.
- Expression equality: `ir_opt_pure_expr_equal`, `ir_opt_pure_def_equal`, `ir_opt_nonvreg_expr_equal`.
- Call-param helpers: `ir_opt_get_call_param_operand` (27 sites), `ir_opt_nop_call_params` (15 sites), `ir_opt_nop_call_param`, `ir_opt_change_call_argc`.

#### 1.4 `ir/opt_alias.h` + `ir/opt_alias.c` (~600 lines)
- `ir_opt_store_btype_size_bytes`, `ir_opt_stack_slot_range_for_offset`, `stackoff_same_slot`, `operand_references_slot`, `is_stack_address_operand`, `find_deref_use_operand`.

#### 1.5 `ir/opt_loop_utils.h` + `ir/opt_loop_utils.c` (~1,800 lines)
- IV analysis (`find_induction_vars_ex`, `find_derived_ivs`, `transform_derived_iv`, `iv_strength_reduction_core`).
- Loop bounds (`find_loop_exit_condition`, `compute_trip_count`, `collect_body_instructions`).
- Loop transforms (`try_eliminate_loop`, `try_unroll_loop`, `try_rotate_loop`).
- Structs `InductionVar`, `DerivedIV`.

**At end of Phase 1:** `opt.c` shrinks from 28,973 to ~24,000 lines. No pass logic moves yet; only their shared helpers. `static` → `extern` for everything pulled out. Build is verified after each step.

---

### Phase 2 — Build the engine (3–4 h)

#### 2.1 `ir/opt_engine.h` + `ir/opt_engine.c`

Mirror the SSA engine's shape:

```c
typedef struct IROptCtx {
    TCCIRState *ir;
    int n;                  /* cached ir->next_instruction_index */
    uint32_t generation;    /* bumped on invalidation */

    /* Lazy-built analyses — accessor builds on first use */
    IROptDU du;
    uint32_t du_gen;

    int *pred_count;
    uint32_t pred_gen;

    uint8_t *merge_bitmap;
    uint32_t merge_gen;

    int changes;
} IROptCtx;

typedef int (*ir_opt_gen_fn)(IROptCtx *ctx, int instr_idx);

typedef struct IROptGen {
    int op;                 /* trigger opcode; -1 = match any */
    ir_opt_gen_fn fn;
    const char *name;
    uint8_t needs_du;       /* engine builds DU before dispatch if any gen requires */
    uint8_t same_block;     /* engine wraps fn with same-BB check */
} IROptGen;

/* Lifecycle */
void tcc_ir_opt_ctx_init(IROptCtx *ctx, TCCIRState *ir);
void tcc_ir_opt_ctx_free(IROptCtx *ctx);
void tcc_ir_opt_ctx_invalidate(IROptCtx *ctx);

/* Lazy analysis accessors */
const IROptDU *tcc_ir_opt_ctx_require_du(IROptCtx *ctx);
const int     *tcc_ir_opt_ctx_require_pred(IROptCtx *ctx);
const uint8_t *tcc_ir_opt_ctx_require_merge(IROptCtx *ctx);

/* Run a table of generators in a single forward pass */
int tcc_ir_opt_run_gens(IROptCtx *ctx, const IROptGen *gens, int count);
```

Engine loop (mirrors `ssa_opt_run_gens` shape):
```c
int tcc_ir_opt_run_gens(IROptCtx *ctx, const IROptGen *gens, int count)
{
    TCCIRState *ir = ctx->ir;
    int changes = 0;

    /* Ensure analyses are built once if any rule needs them */
    int any_du = 0;
    for (int g = 0; g < count; g++) if (gens[g].needs_du) { any_du = 1; break; }
    if (any_du) tcc_ir_opt_ctx_require_du(ctx);

    for (int i = 0; i < ir->next_instruction_index; i++) {
        int op = ir->compact_instructions[i].op;
        if (op == TCCIR_OP_NOP) continue;
        for (int g = 0; g < count; g++) {
            if (gens[g].op >= 0 && gens[g].op != op) continue;
            int d = gens[g].fn(ctx, i);
            if (d > 0) { changes += d; break; }   /* first-match-wins */
        }
    }
    return changes;
}
```

**Same-block check:** When `gens[g].same_block` is set, the generator is wrapped by a helper that calls the user's `fn`, captures the matched instruction range, and calls `ir_xform_same_block` before allowing the transform. The cleanest place to put this check is inside the generator (it knows which range to test); a helper macro `IR_OPT_REQUIRE_SAME_BLOCK(ctx, from, to)` makes it one line.

#### 2.2 Verify
Build only — no rules yet. Add `opt_engine.c`/`opt_du.c`/`opt_xform.c` to `Makefile` `IR_FILES`. Both engines coexist; pre-SSA passes still call the old way.

---

### Phase 3 — Convert pass groups to generator tables

Order is by **density of duplication** (highest payoff first), not by file location.

#### 3.1 Fusion group → `ir/opt_gens_fusion.c` (4–6 h)

Convert 7+ fusion passes into generators sharing one engine run. Current passes:

| Pass                      | Trigger              | Today's lines | After (match+transform) |
|---------------------------|----------------------|---------------|-------------------------|
| `fusion_pass` (mla+indexed) | `ADD`, `LOAD`, `STORE` | ~300 | ~120 |
| `rotate_fusion`           | `ADD`/`OR` patterns  | ~260 | ~100 |
| `deref_indexed_fusion`    | ALU with deref       | ~215 | ~100 |
| `disp_fusion`             | `LOAD`/`STORE`/`ASSIGN` | ~260 | ~90 |
| `postinc_fusion`          | `LOAD`/`STORE`       | ~280 | ~90 |
| `lea_fold`                | any deref source     | ~420 | ~120 |
| `indexed_chain`           | `LOAD_INDEXED`/`STORE_INDEXED` | ~150 | ~60 |
| `indexed_pair_reorder`    | `LOAD_INDEXED` pairs | ~200 | ~70 |
| `assign_fuse`             | `ASSIGN` chain       | ~190 | ~70 |

Hand-written exceptions:
- `add_deref_fold` (inserts new instructions, can't fit a same-index forward engine).
- `loop_postinc_fusion` (needs loop structure from `IRLoops`).
- `stackoff_addr_cse`, `call_chain_rename` (BB-scoped hash, see Phase 3.4).

**Pipeline integration:**
```c
/* Before — 8 separate forward scans, 8 DU builds */
tcc_ir_opt_rotate_fusion(ir);
tcc_ir_opt_fusion_pass(ir, opt_mla, opt_indexed);
tcc_ir_opt_deref_indexed_fusion(ir);
tcc_ir_opt_disp_fusion(ir);
tcc_ir_opt_indexed_chain(ir);
tcc_ir_opt_indexed_pair_reorder(ir);
tcc_ir_opt_assign_fuse(ir);
tcc_ir_opt_lea_fold(ir);
tcc_ir_opt_postinc_fusion(ir);

/* After — 1 scan, 1 DU build */
IROptCtx ctx;
tcc_ir_opt_ctx_init(&ctx, ir);
tcc_ir_opt_run_gens(&ctx, fusion_gens, FUSION_GENS_COUNT);
tcc_ir_opt_ctx_free(&ctx);

tcc_ir_opt_add_deref_fold(ir);     /* inserts → hand-written */
tcc_ir_opt_loop_postinc_fusion(ir); /* needs IRLoops → hand-written */
```

Convert one generator at a time, run `make test -j16` after each. Use existing IR tests (`tests/ir_tests/`) that exercise each pattern to catch ordering regressions.

#### 3.2 Branch-folding group → `ir/opt_gens_branch.c` (3–4 h)

All these trigger on `JUMPIF` and inspect the backward def chain. Currently 5 separate forward scans:

| Pass                      | Trigger     | Today | After |
|---------------------------|-------------|-------|-------|
| `branch_folding`          | `JUMPIF`    | ~160  | ~55   |
| `setif_branch_fuse`       | `JUMPIF`    | ~130  | ~65   |
| `stack_addr_nonnull_fold` | `JUMPIF`    | ~470  | keep hand-written *or* split simple cases (~120) into generator and leave deep def-chain tracing (~350) in a helper |
| `or_bool_diamond`         | `JUMPIF`    | ~230  | ~80 |
| `stack_bool_diamond`      | CFG diamond | ~270  | keep hand-written (4-instruction CFG pattern doesn't fit single-trigger dispatch) |

Hand-written exceptions: `nonneg_branch_fold`, `float_branch_fold` (need merge-bitmap value tracking that doesn't fit per-instruction dispatch).

#### 3.3 Boolean simplification → `ir/opt_gens_bool.c` (1–2 h)

`bool_idempotent` + `bool_simplify` + the idempotent half of `bool_pass` collapse into 2–3 generators triggered on `BOOL_AND`/`BOOL_OR`. CSE half of `bool_pass` keeps its hash table and uses the new generic `IROptHashTable` from Phase 4.

#### 3.4 BB-scoped hash CSE → use `opt_hash` (3–4 h)

`cse_global_load`, `globalsym_cse`, `cse_param_add`, `local_load_cse`, `local_alu_cse`, `stackoff_addr_cse`, `cse_bool` all maintain a hash table that resets at BB boundaries. They are too varied for a single engine but they all reinvent the same hash-table lifecycle.

**Phase 4 builds a shared `IROptHashTable`** (see below) — these passes are then rewritten to use it. Body logic stays per-pass; only the hash-table alloc/lookup/insert/clear/free becomes shared. ~400 lines saved across the 7 passes.

#### 3.5 Call-result dead group → `ir/opt_gens_call_result.c` (2 h)

`dead_call_result_elim`, `dead_init_via_call`, `dead_sret_call_elim`, `fold_call_result_store` all trigger on `FUNCCALLVAL` / `RETURNVALUE` and inspect the result's use chain. Collect-then-transform pattern fits the engine if a 2-phase variant is added (see Phase 5).

---

### Phase 4 — Generic hash table (3–4 h)

`ir/opt_hash.h` + `ir/opt_hash.c` (~200 lines) providing a bump-allocated CSE hash table. Drop-in replacement for 4 hand-rolled tables in `opt.c`:

| Pass                | Local struct        | Buckets |
|---------------------|--------------------|---------|
| `cse_arith` (in `local_alu_cse`) | `ArithCSEEntry`    | 256 |
| `cse_bool` (in `bool_pass`)      | `BoolCSEEntry`     | 64  |
| `sl_forward`        | `StoreEntry`       | 128 |
| `globalsym_cse`     | `GSymCSEEntry`     | linear-16 |

API mirrors what `ssa_opt_load_cse` uses internally:

```c
typedef struct IROptHashEntry {
    uint32_t hash;
    int instruction_idx;
    int32_t result_vr;
    int extra[4];                 /* pass-specific payload */
    struct IROptHashEntry *next;
} IROptHashEntry;

typedef struct IROptHashTable {
    IROptHashEntry **buckets;
    int n_buckets;
    IROptHashEntry *pool;         /* bump-allocated */
    int pool_count;
} IROptHashTable;

void ir_opt_hash_init(IROptHashTable *, int n_buckets, int max_entries);
void ir_opt_hash_clear(IROptHashTable *);   /* O(n_buckets), not O(entries) */
void ir_opt_hash_free(IROptHashTable *);
IROptHashEntry *ir_opt_hash_lookup(IROptHashTable *, uint32_t hash,
                                   int (*eq)(const IROptHashEntry *, const void *),
                                   const void *key);
IROptHashEntry *ir_opt_hash_insert(IROptHashTable *, uint32_t hash);
```

`sl_forward`'s store-entry table has alias semantics that don't fit; **don't** touch it. The other 3 are straight rewrites.

---

### Phase 5 — Collect-then-transform engine variant (optional, 2–3 h)

Several passes (`const_var_prop`, `dead_call_result_elim`, `redundant_var_assign`, `dead_var_store_elim`) follow the pattern: forward pass to collect metadata, finalize, forward pass to transform. A 2-phase engine collapses their boilerplate:

```c
typedef struct IROptCollectGen {
    const char *name;
    int op;
    int (*collect)(IROptCtx *, int idx);   /* phase 1 */
    int (*transform)(IROptCtx *, int idx); /* phase 2 */
} IROptCollectGen;

int tcc_ir_opt_run_collect_gens(IROptCtx *, const IROptCollectGen *, int n);
```

This is **optional** and should only be done after Phase 3 if the collect-transform passes still show significant boilerplate. If they don't, keep them hand-written and skip this phase.

---

### Phase 6 — Theme-based file split (3–5 h, optional, zero flash savings)

After Phases 0–5 the pre-SSA layer is:
- `opt.c` core (~16,000 lines of hand-written passes that don't fit any engine variant)
- `opt_engine.c`, `opt_du.c`, `opt_xform.c`, `opt_utils.c`, `opt_alias.c`, `opt_loop_utils.c`, `opt_hash.c`
- `opt_gens_fusion.c`, `opt_gens_branch.c`, `opt_gens_bool.c`, `opt_gens_call_result.c`

Splitting the remaining `opt.c` by theme (cleanup / constprop / memory / loop / promote / peephole) is a pure-readability change and produces **zero flash savings**. It is worth doing once everything else is stable, mostly to make merge conflicts less painful. Don't block any of the earlier phases on this.

---

## Pipeline driver changes

The optimization driver lives in `tccgen.c` (~lines 25227–26230). Most changes are local one-block replacements where 7 sequential pass calls become 1 engine call:

- Fusion section (~25446–25478): 9 calls → 1 engine call + 2 hand-written holdouts.
- Branch section (~25277–25291 and ~25535–25589 inside iterative loop): 3–4 calls → 1 engine call.
- Boolean section (~25480–25484): 2 calls → 1 engine call + 1 hand-written CSE.

Inside the iterative `do { changes += … } while (changes)` loop, each engine invocation creates and destroys its own `IROptCtx` — the analysis cache must not span iterations because `compact_nops` and `dce` between iterations renumber instructions.

---

## Risks

- **Generator function-pointer dispatch overhead.** With ~10 fusion gens and 20K instructions, that's up to 200K indirect calls per engine run. Trigger-op filtering skips ~90% of gens per instruction. If profiling shows >5% overhead, switch to a `switch (op)` dispatch table generated at compile time. Mitigation already proven by `ssa_opt_run_gens` running in production with 14+ gens in `fold` alone.
- **Ordering changes when batching.** Today MLA fusion finishes the entire IR before disp fusion starts. After batching they run at the same instruction. First-match-wins + rule ordering (MLA before disp, indexed before plain disp, etc.) handles this, but every conversion needs a test verifying IR-dump equivalence on a representative input.
- **DU-table invalidation mid-pass.** When a generator changes `MUL→MLA` or `LOAD→LOAD_INDEXED`, the set of defined/used vregs around that index changes. NOP-only transforms preserve DU. Each generator must declare whether it changes opcodes; the engine refreshes DU between gens that need it. The SSA engine handles this via `tcc_ir_ssa_opt_rebuild` — borrow the same approach.
- **Pre-SSA passes that insert instructions.** `add_deref_fold` is the canonical example. Inserting shifts subsequent indices, invalidating the engine's loop counter. These stay hand-written and run **outside** the engine call. Document the rule: "generators must not change instruction count."

---

## Estimated effort

| Phase | What                                              | Time     | Net lines removed |
|------:|---------------------------------------------------|----------|-------------------|
| 0     | Delete dead stubs                                 | 15 min   | ~30               |
| 1     | Libraries: opt_du / opt_xform / opt_utils / opt_alias / opt_loop_utils | 4–6 h | ~500 (dedup) |
| 2     | Engine: opt_engine.c                              | 3–4 h    | -600 (added)      |
| 3.1   | Fusion gens                                       | 4–6 h    | ~1,400            |
| 3.2   | Branch gens                                       | 3–4 h    | ~500              |
| 3.3   | Bool gens                                         | 1–2 h    | ~200              |
| 3.4   | BB hash CSE rewrites                              | 3–4 h    | ~400              |
| 3.5   | Call-result gens                                  | 2 h      | ~300              |
| 4     | Generic IROptHashTable                            | 3–4 h    | (counted in 3.4)  |
| 5     | Collect-transform engine variant (optional)       | 2–3 h    | ~250              |
| 6     | Theme-based split of remaining opt.c (optional)   | 3–5 h    | 0                 |
| **Total (phases 0–4)**                                    | **~20–28 h** | **~3,400 (~12%)** |

Each phase produces a working build. Each can ship independently. If the project ships at any intermediate state, the result is strictly better than today.

---

## Why this rewrite is different from the original plan

| Original plan said…                                | This plan says…                                              |
|---------------------------------------------------|--------------------------------------------------------------|
| opt.c is 22,712 lines, ~60 passes                  | opt.c is 28,973 lines, 81 passes (and growing)               |
| Pre-SSA is a migration bridge — passes die as SSA matures | Pre-SSA is permanent infrastructure for post-destruction IR  |
| Phase 4 (engine) is optional contingency           | Phase 2 (engine) is the **primary** flash-saving mechanism   |
| Phases 2 (theme split) first, then engine          | Engine first; theme split last (or skip entirely)            |
| Invent a fresh `IRPeepholeRule` API                | **Mirror** the proven `IRSSAOptGen` / `ssa_opt_run_gens` API |
| Pass conversion is a 4–6 h side project            | Pass conversion is **the whole point** — most of the work    |