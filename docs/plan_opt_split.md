# Plan: Split `ir/opt.c` Into Themed Modules

## Current State

`ir/opt.c` is **17,861 lines** (down from 28,973 after Phase 6.1 extracted `opt_loop.c` and `opt_memory.c`). It still contains **67 functions** spanning 6+ distinct optimization themes. The already-extracted modules total ~13,200 lines across 14 files — so the remaining monolith is still the single largest source file.

### Already extracted (for reference)

| File | Lines | Contents |
|------|-------|----------|
| `opt_loop_utils.c` | 3,498 | IV analysis, loop bounds, loop transforms |
| `opt_memory.c` | 3,259 | sl_forward, entry_store_prop, store_redundant, deref_fwd |
| `opt_loop.c` | 1,052 | Strength reduction, unroll, rotation, decrement-to-zero |
| `opt_utils.c` | 978 | Constant evaluators, BB/CFG helpers, purity tables |
| `opt_gens_fusion.c` | 818 | Engine-based fusion generators |
| `opt_gens_call_result.c` | 301 | Dead call result generators |
| `opt_jump_thread.c` | 203 | Jump threading + fallthrough elimination |
| `opt_gens_branch.c` | 176 | Branch folding generators |
| `opt_alias.c` | 127 | Stack-slot aliasing helpers |
| `opt_engine.c` | 100 | IROptCtx, IROptGen, tcc_ir_opt_run_gens |
| `opt_du.c` | 98 | Def-use build/query |
| `opt_hash.c` | 63 | Generic hash table for CSE |
| `opt_gens_bool.c` | 57 | Boolean simplification generators |
| `opt_xform.c` | 24 | Transform primitives |

---

## Proposed Split

Split the remaining 17,861 lines into **7 new themed files** + a slim residual `opt.c` (~1,600 lines).

---

### 1. `ir/opt_dce.c` — Dead Code & Cleanup (~2,200 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_dce` | 122 | 97–218 |
| `tcc_ir_opt_compact_nops` | 203 | 219–421 |
| `tcc_ir_opt_dead_var_store_elim` | 131 | 2985–3115 |
| `tcc_ir_opt_dead_addrvar_elim` | 330 | 3348–3677 |
| `tcc_ir_opt_redundant_var_assign` | 157 | 3678–3834 |
| `tcc_ir_opt_redundant_init_elim` | 156 | 14531–14686 |
| `tcc_ir_opt_dead_loop_elim` | 228 | 15500–15727 |
| `tcc_ir_opt_dse` | 1,269 | 1716–2984 |

**Rationale:** All these passes remove dead/redundant IR — NOPs, unreachable code, dead stores, dead variables. `dse` is the largest single pass (1,269 lines) and is purely elimination logic. Grouping gives a single file for "what can I safely delete."

**Internal dependencies:**
- `dse` uses `ir_opt_build_def_count` (shared static helper → move or expose via `opt_du.h`)
- All use `ir_xform_nop` (already in `opt_xform.h`)
- `dead_addrvar_elim` and `dse` use alias helpers (already in `opt_alias.h`)

---

### 2. `ir/opt_constprop.c` — Constant & Value Propagation (~4,100 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_const_var_prop` | 253 | 422–674 |
| `tcc_ir_opt_global_init_prop` | 137 | 675–811 |
| `tcc_ir_opt_complex_const_param_fold` | 177 | 812–988 |
| `tcc_ir_opt_const_prop` | 1,235 | 3835–5069 |
| `tcc_ir_opt_value_tracking` | 1,647 | 5070–6716 |
| `tcc_ir_opt_const_prop_tmp` | 368 | 7928–8295 |
| `tcc_ir_opt_add_reassoc` | 125 | 8330–8454 |
| `tcc_ir_opt_cmp_expr_fold` | 166 | 8455–8620 |
| `ir_opt_build_def_count` (static) | 34 | 8296–8329 |

**Rationale:** These are the "what values do I know at this point" passes. `const_prop` (1,235 lines) and `value_tracking` (1,647 lines) are the two biggest passes remaining in opt.c and they share constant-evaluation infrastructure. Together they form the core analysis engine.

**Internal dependencies:**
- `const_prop` and `value_tracking` share evaluation helpers from `opt_utils.h`
- `ir_opt_build_def_count` is used by `add_reassoc` and `copy_prop` → make non-static, expose from header
- `value_tracking` uses VRP slot helpers (`vrp_get_slot`, `vrp_fold_cmp`) — move with it

---

### 3. `ir/opt_copyprop.c` — Copy Propagation & CSE (~1,500 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_copy_prop` | 449 | 8621–9069 |
| `tcc_ir_opt_cse_global_load` | 214 | 9104–9317 |
| `tcc_ir_opt_globalsym_cse` | 133 | 9362–9494 |
| `gsym_cse_insert_before` (static) | 44 | 9318–9361 |
| `tcc_ir_opt_cse_param_add` | 194 | 9495–9688 |
| `tcc_ir_opt_local_load_cse` | 189 | 13737–13925 |
| `tcc_ir_opt_local_alu_cse` | 255 | 13926–14180 |
| `bool_cse_hash` / `bool_cse_eq` (statics) | 34 | 9070–9103 |

**Rationale:** All these passes identify redundant computations (copy chains, repeated loads, repeated ALU ops) and eliminate them via forwarding or CSE. They share the same flat-array or hash-table BB-scoped pattern.

**Internal dependencies:**
- Uses `IROptHashTable` from `opt_hash.h`
- `copy_prop` uses `ir_opt_build_def_count` (from opt_constprop.c or made public)
- `gsym_cse_insert_before` inserts instructions — unique to this group

---

### 4. `ir/opt_branch.c` — Branch & Boolean Optimization (~2,200 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_float_branch_fold` | 252 | 7178–7429 |
| `ir_opt_match_zero_test` (static) | 35 | 7143–7177 |
| `tcc_ir_opt_vrp` | 330 | 7430–7759 |
| `vrp_get_slot` / `vrp_fold_cmp` (statics) | 29 | 6717–6745 |
| `tcc_ir_opt_nonneg_branch_fold` | 365 | 9720–10084 |
| `nonneg_func_names` / `flag_cmp_funcs` (tables) | 31 | 9689–9719 |
| `tcc_ir_opt_branch_folding` | 30 | 12447–12476 |
| `tcc_ir_opt_stack_addr_nonnull_fold` | 423 | 12477–12899 |
| `tcc_ir_opt_setif_branch_fuse` | 39 | 12900–12938 |
| `tcc_ir_opt_stack_bool_diamond` | 268 | 12939–13206 |
| `tcc_ir_opt_or_bool_diamond` | 232 | 13207–13438 |
| `tcc_ir_opt_bool_cse` | 75 | 12324–12398 |

**Rationale:** All passes that reason about conditional branches, VRP (value-range propagation), boolean CSE, and control-flow diamonds. They share `JUMPIF`-triggered pattern matching and backward def-chain tracing. `vrp` and `nonneg_branch_fold` both use the VRP slot/fold helpers.

**Internal dependencies:**
- `vrp` range tables are self-contained
- `nonneg_branch_fold` uses `change_callee_sym` (shared with float_narrowing → move to opt_utils or keep in residual)
- Branch passes use `ir_opt_match_zero_test` → move together

---

### 5. `ir/opt_fusion.c` — Fusion & Addressing Mode (hand-written) (~2,050 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_add_deref_fold` | 232 | 3116–3347 |
| `tcc_ir_opt_postinc_fusion` | 278 | 10673–10950 |
| `tcc_ir_opt_loop_postinc_fusion` | 476 | 10951–11426 |
| `tcc_ir_barrel_shift_fusion` | 146 | 11427–11572 |
| `tcc_ir_opt_call_chain_rename` | 155 | 11573–11727 |
| `tcc_ir_opt_stackoff_addr_cse` | 176 | 11728–11903 |
| `tcc_ir_opt_lea_fold` | 420 | 11904–12323 |
| `tcc_ir_opt_assign_fuse` | 184 | 17486–17669 |

**Rationale:** Hand-written fusion passes that couldn't be converted to engine generators (they insert instructions, need loop structure, or use BB-scoped hash tables). These are the ARM addressing-mode optimization passes — `LOAD_INDEXED`, `LOAD_POSTINC`, barrel-shift folding, LEA elimination, displacement fusion. Distinct from `opt_gens_fusion.c` which holds the engine-compatible generators.

**Internal dependencies:**
- `loop_postinc_fusion` uses `IRLoops` from `opt_loop_utils.h`
- `lea_fold` uses def-use from `opt_du.h`
- `call_chain_rename` uses `change_callee_sym` helpers

---

### 6. `ir/opt_promote.c` — Variable-to-Temp Promotion & Forwarding (~1,600 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_var_tmp_fwd` | 298 | 13439–13736 |
| `tcc_ir_opt_var_to_tmp` | 350 | 14181–14530 |
| `tcc_ir_opt_select` | 410 | 14687–15096 |
| `tcc_ir_opt_postinc_assign_fold` | 145 | 15303–15447 |
| `tcc_ir_opt_returnvalue_merge` | 52 | 15448–15499 |
| `tcc_ir_opt_backedge_phi_hoist` | 205 | 15920–16124 |
| `tcc_ir_opt_redundant_loop_check` | 168 | 7760–7927 |

**Rationale:** These passes promote stack variables to temporaries, forward values through variable stores/loads, and select-ify simple if/else diamonds. They bridge the gap between flat variable-based IR (post-SSA destruction) and the register allocator which needs temporaries. `select` is the largest (410 lines) — it converts store-to-var-in-both-branches into a conditional move.

---

### 7. `ir/opt_constfold.c` — Constant String/Call/Addrof Folding (~1,800 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `ir_opt_eval_const_string_operand` (static) | 70 | 6746–6815 |
| `ir_opt_fold_strcmp_result` (static) | 13 | 6816–6828 |
| `ir_opt_fold_strncmp_result` (static) | 16 | 6829–6844 |
| `ir_opt_fold_memcmp_result` (static) | 15 | 6845–6859 |
| `ir_opt_fold_memchr_offset` (static) | 20 | 6860–6879 |
| `tcc_ir_opt_const_string_calls` | 263 | 6880–7142 |
| `tcc_ir_opt_const_call_replace` | 90 | 15830–15919 |
| `tcc_ir_detect_const_result` | 73 | 15728–15800 |
| `tcc_ir_cache_const_result` | 15 | 15801–15815 |
| `tcc_ir_lookup_const_result` | 14 | 15816–15829 |
| `tcc_ir_opt_param_addrof_const_fold` | 435 | 16125–16559 |
| `tcc_ir_opt_local_addrof_const_fold` | 471 | 16560–17030 |
| `tcc_ir_opt_float_narrowing` | 307 | 10151–10457 |
| `float_narrow_table` / `change_callee_sym*` | 66 | 10085–10150 |

**Rationale:** These passes evaluate calls and expressions at compile time when arguments are known constants — string library folding (`strcmp`, `strlen`, `memcmp`), memoized pure-function results, address-of-parameter constant propagation, and float type narrowing (e.g., `double→float` when precision allows). All share the "trace constant operands backward, fold result" pattern.

**Internal dependencies:**
- `change_callee_sym` / `change_callee_sym_keep_type` → used by both `float_narrowing` and `nonneg_branch_fold`. Move to this file (it's defined here at line 10106) or to `opt_utils.c` if needed by `opt_branch.c` too.

---

### 8. `ir/opt_pack64.c` — 64-bit Register Pair Optimization (~650 lines)

Functions to move:

| Function | Lines | Range |
|----------|-------|-------|
| `tcc_ir_opt_pack64` | 179 | 17031–17209 |
| `p64taut_trace_back` (static) | 51 | 17210–17260 |
| `tcc_ir_opt_pack64_tautology` | 225 | 17261–17485 |
| `tcc_ir_opt_cmp_narrow_64` | 192 | 17670–17861 |

**Rationale:** ARM-specific 64-bit register-pair tracking. These passes combine/split `PACK64` pseudo-ops and eliminate redundant 64→32→64 conversions. Self-contained logic with no significant shared state.

---

### 9. Residual `ir/opt.c` (~1,600 lines)

What stays:

| Function | Lines | Why stays |
|----------|-------|-----------|
| FP cache wrappers | 40 | Thin delegation layer, trivial |
| `tcc_ir_analyze_pure_via_sret` | 250 | Cross-cutting interprocedural analysis |
| FWS (func write summary) block | 400 | `fws_*` + `tcc_ir_compute_func_write_summary` — interprocedural, used by `dead_init_via_call` |
| `tcc_ir_opt_dead_init_via_call` | 116 | Depends on FWS, tight coupling |
| `tcc_ir_opt_stack_addr_cse` | 215 | Doesn't fit cleanly elsewhere (BB hash + stack aliasing hybrid) |
| `tcc_ir_opt_block_copy_init` | 206 | Memory/struct init hybrid |
| `tcc_ir_find_defining_instruction` | 18 | Small utility, widely used |
| `tcc_ir_vreg_has_single_use` | 30 | Small utility, widely used |
| Forward decls, includes, macros | ~50 | Boilerplate |

The residual `opt.c` becomes a "miscellaneous + interprocedural" file. As these grow, they can be split further (e.g., `opt_interproc.c` for FWS + sret analysis).

---

## Dependency Graph

```
opt.c (residual, 1.6K)
  ├── opt_dce.c (2.2K)         → opt_xform, opt_alias, opt_utils
  ├── opt_constprop.c (4.1K)   → opt_utils, opt_du
  ├── opt_copyprop.c (1.5K)    → opt_hash, opt_du, opt_utils
  ├── opt_branch.c (2.2K)      → opt_utils, opt_du
  ├── opt_fusion.c (2.0K)      → opt_du, opt_loop_utils, opt_alias
  ├── opt_promote.c (1.6K)     → opt_du, opt_utils
  ├── opt_constfold.c (1.8K)   → opt_utils
  └── opt_pack64.c (0.6K)      → (self-contained)
```

No circular dependencies. Each new file includes `ir.h` (which pulls in `tccir.h` + core types) plus the specific `opt_*.h` headers it needs.

---

## Shared Helpers To Expose

Before splitting, these currently-`static` helpers need to become non-static (add to appropriate header):

| Helper | Current location | Move to |
|--------|-----------------|---------|
| `ir_opt_build_def_count` | opt.c:8296 | `opt_du.h` / `opt_du.c` |
| `change_callee_sym` | opt.c:10106 | `opt_utils.h` / `opt_utils.c` |
| `change_callee_sym_keep_type` | opt.c:10133 | `opt_utils.h` / `opt_utils.c` |
| `vrp_get_slot` / `vrp_fold_cmp` | opt.c:6717 | `opt_branch.c` (file-local) |
| `ir_opt_match_zero_test` | opt.c:7143 | `opt_branch.c` (file-local) |
| `ir_opt_eval_const_string_operand` | opt.c:6746 | `opt_constfold.c` (file-local) |
| `ir_opt_fold_str*` / `ir_opt_fold_mem*` | opt.c:6816–6879 | `opt_constfold.c` (file-local) |
| `p64taut_trace_back` | opt.c:17210 | `opt_pack64.c` (file-local) |
| `gsym_cse_insert_before` | opt.c:9318 | `opt_copyprop.c` (file-local) |
| `bool_cse_hash` / `bool_cse_eq` | opt.c:9070 | `opt_copyprop.c` (file-local) |

---

## Execution Plan

### Step 1: Expose shared helpers (30 min)
- [ ] Move `ir_opt_build_def_count` → `opt_du.c` / `opt_du.h`
- [ ] Move `change_callee_sym` + `change_callee_sym_keep_type` → `opt_utils.c` / `opt_utils.h`
- [ ] Verify: `make cross && make test -j16`

### Step 2: Extract `opt_pack64.c` (30 min)
- [ ] Create `ir/opt_pack64.c` with `#define USING_GLOBALS` + `#include "ir.h"`
- [ ] Move `tcc_ir_opt_pack64`, `p64taut_trace_back`, `tcc_ir_opt_pack64_tautology`, `tcc_ir_opt_cmp_narrow_64`
- [ ] Add to `Makefile` `IR_FILES`
- [ ] Verify: `make cross && make test -j16`

### Step 3: Extract `opt_dce.c` (45 min)
- [ ] Create `ir/opt_dce.c`
- [ ] Move 8 functions: `dce`, `compact_nops`, `dead_var_store_elim`, `dead_addrvar_elim`, `redundant_var_assign`, `redundant_init_elim`, `dead_loop_elim`, `dse`
- [ ] Create `ir/opt_dce.h` with public declarations
- [ ] Verify: `make cross && make test -j16`

### Step 4: Extract `opt_constfold.c` (45 min)
- [ ] Create `ir/opt_constfold.c`
- [ ] Move 14 functions: string fold helpers, `const_string_calls`, `const_call_replace`, `detect_const_result`, `cache_const_result`, `lookup_const_result`, `param_addrof_const_fold`, `local_addrof_const_fold`, `float_narrowing`, `float_narrow_table`
- [ ] Verify: `make cross && make test -j16`

### Step 5: Extract `opt_branch.c` (45 min)
- [ ] Create `ir/opt_branch.c`
- [ ] Move 12 functions: `float_branch_fold`, `match_zero_test`, `vrp`, VRP statics, `nonneg_branch_fold`, name tables, `branch_folding`, `stack_addr_nonnull_fold`, `setif_branch_fuse`, `stack_bool_diamond`, `or_bool_diamond`, `bool_cse`
- [ ] Verify: `make cross && make test -j16`

### Step 6: Extract `opt_copyprop.c` (45 min)
- [ ] Create `ir/opt_copyprop.c`
- [ ] Move 8 functions: `copy_prop`, `cse_global_load`, `globalsym_cse`, `gsym_cse_insert_before`, `cse_param_add`, `local_load_cse`, `local_alu_cse`, `bool_cse_hash`/`bool_cse_eq`
- [ ] Verify: `make cross && make test -j16`

### Step 7: Extract `opt_fusion.c` (45 min)
- [ ] Create `ir/opt_fusion.c`
- [ ] Move 8 functions: `add_deref_fold`, `postinc_fusion`, `loop_postinc_fusion`, `barrel_shift_fusion`, `call_chain_rename`, `stackoff_addr_cse`, `lea_fold`, `assign_fuse`
- [ ] Verify: `make cross && make test -j16`

### Step 8: Extract `opt_promote.c` (30 min)
- [ ] Create `ir/opt_promote.c`
- [ ] Move 7 functions: `var_tmp_fwd`, `var_to_tmp`, `select`, `postinc_assign_fold`, `returnvalue_merge`, `backedge_phi_hoist`, `redundant_loop_check`
- [ ] Verify: `make cross && make test -j16`

### Step 9: Extract `opt_constprop.c` (45 min)
- [ ] Create `ir/opt_constprop.c`
- [ ] Move 9 functions: `const_var_prop`, `global_init_prop`, `complex_const_param_fold`, `const_prop`, `value_tracking`, `const_prop_tmp`, `add_reassoc`, `cmp_expr_fold`, `ir_opt_build_def_count`
- [ ] Verify: `make cross && make test -j16`

### Step 10: Final cleanup (30 min)
- [ ] Verify residual `opt.c` is ~1,600 lines
- [ ] Update `opt.h` — ensure all public function declarations reference correct headers
- [ ] Audit includes in each new file — remove unnecessary ones
- [ ] Final: `make cross && make test -j16 && make test-asm -j16`

---

## Result Summary

| File | Lines | Theme |
|------|-------|-------|
| `opt.c` (residual) | ~1,600 | Interprocedural (FWS, sret), misc |
| `opt_constprop.c` | ~4,100 | Constant/value propagation |
| `opt_dce.c` | ~2,200 | Dead code/store elimination |
| `opt_branch.c` | ~2,200 | Branch/VRP/boolean |
| `opt_fusion.c` | ~2,050 | Hand-written addressing-mode fusion |
| `opt_constfold.c` | ~1,800 | Compile-time call/string/addrof folding |
| `opt_promote.c` | ~1,600 | Variable→temp promotion |
| `opt_copyprop.c` | ~1,500 | Copy propagation & CSE |
| `opt_pack64.c` | ~650 | 64-bit register pair |

**Total estimated effort: ~6 hours** (mechanical moves, no logic changes).

**No flash savings** — this is purely a readability/maintainability refactor. The engine work (Phases 2–5 in the parent plan) is what saves flash.

---

## Risks & Mitigations

1. **Compilation unit boundaries change optimizer behavior.** Static functions that were previously inlinable across passes become extern calls. Mitigation: critical hot helpers stay `static inline` in headers (e.g., `ir_xform_nop` already is).

2. **Include order sensitivity.** `opt.c` currently relies on `#define USING_GLOBALS` at the top. Each new file needs this + `#include "ir.h"`. Verify with `-Werror` that no implicit declarations creep in.

3. **`change_callee_sym` used by 2 target files.** Moving it to `opt_utils.c` means both `opt_branch.c` and `opt_constfold.c` can call it. Alternative: duplicate in each file (worse) or keep in residual `opt.c` (limits extraction).

4. **Build time.** More `.o` files = more linker inputs but better incremental build (touching one pass doesn't recompile 17K lines). Net positive for development velocity.
