# opt.c Analysis & Modularization Plan

## Current State

`ir/opt.c` is **22,712 lines** — 65% of all IR code (34,684 lines total across all `ir/*.c` files). It contains **~60 optimization pass functions** plus shared infrastructure. The next-largest IR files are `licm.c` (2,483 lines) and `codegen.c` (2,967 lines).

One pass was already extracted:
- `opt_jump_thread.c` (203 lines) — in `Makefile` `IR_FILES`, actively used.

Dead code to clean up:
- `opt_embedded_deref.c` (214 lines) — exists on disk but is **not in `Makefile`** and its sole function `tcc_ir_opt_extract_embedded_deref()` is **never called** from anywhere. This is orphaned dead code, not an extracted pass.
- `tcc_ir_opt_run_all()` and `tcc_ir_opt_run_by_name()` in `opt.c` (lines 13074-13087) — empty stubs with `TODO` comments, never called.
- `tcc_ir_opt_return()` (line 9398) — stub, 5 lines.

The optimization pipeline driver lives in `tccgen.c` (lines ~25227-26230), not in `opt.c`.

---

## Section Map of opt.c

| Lines  | Section | Pass Function(s) | Theme |
|--------|---------|------------------|-------|
| 34-63 | FP Cache Wrappers | `tcc_ir_opt_fp_cache_*` | Backend helpers |
| 129-147 | CSE Hash Helpers | — | Shared data structure |
| 149-270 | Dead Code Elimination | `tcc_ir_opt_dce` | Cleanup |
| 271-473 | NOP Compaction | `tcc_ir_opt_compact_nops` | Cleanup |
| 474-726 | Constant VAR Propagation | `tcc_ir_opt_const_var_prop` | Constant propagation |
| 727-851 | Global Init Propagation | `tcc_ir_opt_global_init_prop` | Constant propagation |
| 852-2117 | Dead Store Elimination | `tcc_ir_opt_dse` | Cleanup |
| 2118-2246 | Dead VAR Store Elimination | `tcc_ir_opt_dead_var_store_elim` | Cleanup |
| 2248-2422 | ADD Deref Fold | `tcc_ir_opt_add_deref_fold` | Peephole |
| 2424-2752 | Dead AddrVar Elimination | `tcc_ir_opt_dead_addrvar_elim` | Cleanup |
| 2754-2907 | Redundant VAR Assign | `tcc_ir_opt_redundant_var_assign` | Cleanup |
| 2909-4017 | Constant Propagation | `tcc_ir_opt_const_prop` | Constant propagation |
| 4018-5473 | Value Tracking (arithmetic) | `tcc_ir_opt_value_tracking` | Constant propagation |
| 5475-6256 | VRP Infrastructure | (structs, helpers for VRP pass below) | Value range analysis |
| 6258-6765 | Const String Calls | `tcc_ir_opt_const_string_calls` | Library call folding |
| 6767-7017 | Float Branch Fold | `tcc_ir_opt_float_branch_fold` | Branch folding |
| 7019-7347 | VRP Pass | `tcc_ir_opt_vrp` | Value range analysis |
| 7349-7515 | Redundant Loop Check | `tcc_ir_opt_redundant_loop_check` | Loop opt |
| 7517-7867 | Const Prop TMP | `tcc_ir_opt_const_prop_tmp` | Constant propagation |
| 7869-8356 | Copy Propagation | `tcc_ir_opt_copy_prop` | Copy propagation |
| 8358-8435 | CSE Bool | `tcc_ir_opt_cse_bool` | CSE |
| 8437-8503 | Bool Idempotent | `tcc_ir_opt_bool_idempotent` | Boolean simplification |
| 8505-8584 | Bool Simplify | `tcc_ir_opt_bool_simplify` | Boolean simplification |
| 8586-8783 | Global LOAD CSE | `tcc_ir_opt_cse_global_load` | CSE |
| 8827-8955 | GlobalSym CSE | `tcc_ir_opt_globalsym_cse` | CSE |
| 8957-9121 | CSE Param Add | `tcc_ir_opt_cse_param_add` | CSE |
| 9123-9396 | CSE Arith | `tcc_ir_opt_cse_arith` | CSE |
| 9398-9426 | Return Opt (stub) | `tcc_ir_opt_return` | Dead code (delete) |
| 9427-10174 | Entry Store Propagation | `tcc_ir_opt_entry_store_prop` | Memory opt |
| 10177-12169 | Store-Load Forwarding | `tcc_ir_opt_sl_forward` | Memory opt |
| 12171-12379 | Redundant Store Elimination | `tcc_ir_opt_store_redundant` | Memory opt |
| 12381-12694 | Non-Negative Branch Fold | `tcc_ir_opt_nonneg_branch_fold` | Branch folding |
| 12812-13073 | Float Narrowing | `tcc_ir_opt_float_narrowing` | Type narrowing |
| 13074-13087 | Dead Stubs | `tcc_ir_opt_run_all`, `tcc_ir_opt_run_by_name` | Dead code (delete) |
| 13134-13327 | Stack Address CSE | `tcc_ir_opt_stack_addr_cse` | CSE |
| 13329-13443 | Def-Use Table (IROptDU) | `ir_opt_du_*` | Shared infrastructure |
| 13467-13716 | MLA Fusion | `tcc_ir_opt_mla_fusion` | Instruction fusion |
| 13717-13990 | Indexed Memory Fusion | `tcc_ir_opt_indexed_memory_fusion` | Instruction fusion |
| 13991-14265 | Post-Inc Fusion | `tcc_ir_opt_postinc_fusion` | Instruction fusion |
| 14266-14724 | Loop Post-Inc Fusion | `tcc_ir_opt_loop_postinc_fusion` | Instruction fusion |
| 14725-14993 | Combined Fusion Pass | `tcc_ir_opt_fusion_pass` | Instruction fusion |
| 14994-15205 | Deref-Indexed Fusion | `tcc_ir_opt_deref_indexed_fusion` | Instruction fusion |
| 15206-15475 | Displacement Fusion | `tcc_ir_opt_disp_fusion` | Instruction fusion |
| 15476-15796 | LEA Fold | `tcc_ir_opt_lea_fold` | Peephole |
| 15797-16048 | Bool Pass (combined) | `tcc_ir_opt_bool_pass` | Boolean simplification |
| 16049-16206 | Branch Folding | `tcc_ir_opt_branch_folding` | Branch folding |
| 16207-16628 | Stack Addr NonNull Fold | `tcc_ir_opt_stack_addr_nonnull_fold` | Branch folding |
| 16629-16755 | SETIF Branch Fuse | `tcc_ir_opt_setif_branch_fuse` | Branch folding |
| 16756-16946 | Stack Bool Diamond | `tcc_ir_opt_stack_bool_diamond` | Branch folding |
| 16947-17209 | VAR→TMP Forwarding | `tcc_ir_opt_var_tmp_fwd` | Promotion |
| 17210-17657 | VAR→TMP Promotion | `tcc_ir_opt_var_to_tmp` | Promotion |
| 17658-18929 | Strength Reduction (mul) | `tcc_ir_opt_strength_reduction` | Arithmetic opt |
| 18931-18953 | IV Strength Reduction | `tcc_ir_opt_iv_strength_reduction` | Loop opt |
| 18954-19014 | IV SR with loops | `tcc_ir_opt_iv_strength_reduction_with_loops` | Loop opt |
| 19015-19960 | Loop Bound Remat | `tcc_ir_opt_loop_bound_remat` | Loop opt |
| 19961-20575 | Loop Unrolling | `tcc_ir_opt_loop_unroll` | Loop opt |
| 20576-21037 | Loop Rotation | `tcc_ir_opt_loop_rotation` | Loop opt |
| 21038-21283 | Global CSE | `tcc_ir_opt_cse_global` | CSE |
| 21284-21448 | Redundant Init Elim | `tcc_ir_opt_redundant_init_elim` | Cleanup |
| 21449-21731 | Decrement-to-Zero | `tcc_ir_opt_decrement_to_zero` | Loop opt |
| 21732-21981 | Select (ITE) full | `tcc_ir_opt_select` | Select |
| 21994-22192 | Block Copy Init | `tcc_ir_opt_block_copy_init` | Memory opt |
| 22194-22321 | Post-Inc Assign Fold | `tcc_ir_opt_postinc_assign_fold` | Peephole |
| 22323-22542 | Dead Loop Elimination | `tcc_ir_opt_dead_loop_elim` | Loop opt |
| 22544-22648 | Const Call Replace | `tcc_ir_opt_const_call_replace` | Interprocedural |

---

## Shared Infrastructure Inside opt.c

### 1. **Value Tracking / Constant Evaluation** (scattered, primarily lines ~64-148, ~5624-6765)
- `ir_opt_build_merge_bitmap()` — bitmap of merge points (used by VRP, float_branch_fold, etc.)
- `ir_opt_mark_block_starts()` — block start marking
- `ir_opt_next_non_nop()` — skip NOPs
- `ir_opt_is_pure_helper_name()` / `ir_opt_is_flag_cmp_helper_name()` — purity tables
- `ir_opt_get_call_param_operand()` / `ir_opt_nop_call_params()` — call parameter helpers
- `ir_opt_vreg_address_taken_between()` — alias analysis helper
- `ir_opt_eval_const_u64()` — recursive constant evaluator (used by value_tracking, const_prop, const_string_calls)
- `ir_opt_eval_const_string()` / `ir_opt_fold_strcmp_result()` / etc. — string constant folding
- `ir_opt_pure_expr_equal()` / `ir_opt_pure_def_equal()` — expression equivalence for CSE
- `ir_opt_is_pure_fallthrough_instruction()` — purity check for basic blocks
- `ir_opt_match_zero_test()` — pattern matcher

### 2. **Def-Use Table (IROptDU)** (lines 13329-13443)
- `ir_opt_du_idx()`, `ir_opt_du_build()`, `ir_opt_du_def()`, `ir_opt_du_uses()`
- Used by: MLA fusion, indexed fusion, post-inc fusion, combined fusion, disp fusion
- **This is the most valuable shared abstraction to extract first.**

### 3. **Store-Load Analysis Helpers** (lines ~10113-10195)
- `ir_opt_store_btype_size_bytes()` — byte width for store types
- `ir_opt_stack_slot_range_for_offset()` — stack slot aliasing
- Various `StoreEntry` / `AliasInfo` structures for SL forward

### 4. **Loop Infrastructure**
- `find_induction_vars_ex()` / `find_derived_ivs()` — IV analysis
- `insert_instr_at()` — instruction insertion helper
- `transform_derived_iv()` / `try_eliminate_iv_counter()` / `iv_strength_reduction_core()`
- `find_loop_exit_condition()` / `compute_trip_count()` / `collect_body_instructions()`
- `try_eliminate_loop()Here's the approach:

IR ≤ 8: always inline (existing func_auto_inline)
8 < IR ≤ threshold: inline only when all call-site args are constants (func_eval_only_inline)
IR > threshold: don't inline` / `try_unroll_loop()` / `try_rotate_loop()`
- `invert_condition()` / `loop_size_cmp()`

### 5. **Branch Folding Helpers**
- `evaluate_compare_condition()` — constant comparison evaluator
- `tcc_ir_vreg_has_single_def()` — single-def check (static, defined in opt.c; the related `tcc_ir_find_defining_instruction` and `tcc_ir_vreg_has_single_use` are extern, defined in vreg.c)
- `is_stack_address_operand()` — stack address detection
- `invert_cond_token()` — condition inversion

### 6. **Misc Helpers**
- `change_callee_sym()` / `change_callee_sym_keep_type()` — call rewriting
- `gsym_cse_insert_before()` — instruction insertion with jump patching
- `stackoff_same_slot()` / `operand_references_slot()` — stack slot comparison
- `ir_skip_nops_forward()` / `ir_negate_condition()` / `ir_has_other_jump_to()`
- `is_power_of_2()`

### 7. **Data Structures Defined Inside Passes**
- `CSEHashEntry` (line 134) — boolean CSE hash table
- `VarConstInfo` (line 2912) — constant variable tracking
- `VTState` / `VTEntry` (line 3964) — value tracking state
- `VRPRange` (line 5499) — VRP range info
- `TmpConstState` (line 7519) — temp constant propagation state
- `CopyPropFrame` (line 7879) — copy propagation stack frames
- `BoolCSEEntry` (line 8289) — boolean CSE entries
- `GSymCSEEntry` (line 8834) — global symbol CSE
- `ArithCSEEntry` (line 9125) — arithmetic CSE
- `EntryStoreInfo` (line 9700) — entry store propagation
- `StoreEntry` / `AliasInfo` (line 10177) — SL forwarding
- `StackAddrSeq` (line 13124) — stack address CSE
- `IROptDU` (line 13348) — def-use table
- `InductionVar` / `DerivedIV` (line 17721) — IV analysis
- `GCSEExpr` / `GCSEBlock` (line 20667) — global CSE

---

## Proposed Architecture: Optimization Engine with Libraries

There are now two optimization layers: **SSA-form passes** (run on SSA-renamed IR before destruction) and **pre-SSA passes** (run on flat IR after SSA destruction). Both share the IR core but have distinct infrastructure.

```
┌──────────────────────────────────────────────────────────┐
│          Optimization Driver (tccgen.c)                  │
├────────────────────────┬─────────────────────────────────┤
│  SSA Optimization      │  Pre-SSA Optimization           │
│  Engine (ir/opt/)      │  Passes (ir/opt.c → opt_*.c)    │
│  ✓ Implemented         │  Existing monolith              │
│                        │                                 │
│  cprop, dce            │  ~60 passes: const_prop, cse,   │
│  + target generators   │  fusion, loop opts, DSE, VRP... │
├────────────────────────┤                                 │
│  Target Generators     │                                 │
│  (arch/arm/)           │                                 │
│  ✓ Implemented         │                                 │
├────────────────────────┴─────────────────────────────────┤
│  Shared Libraries (opt_utils, opt_du, opt_alias,         │
│    opt_loop_utils) — to be extracted from opt.c          │
├──────────────────────────────────────────────────────────┤
│         IR Core (core.c, ir.h, cfg.c, ssa.c)             │
└──────────────────────────────────────────────────────────┘
```

**SSA engine** (`ir/opt/`) — already implemented:
- Generator-based dispatch: each rewrite rule is an explicit named function (like `thop_*`)
- Use-def chains built from SSA form (each TEMP vreg has exactly one def)
- Target-specific generators registered by backend via `tcc_ir_ssa_opt_register_target()`
- Generic code has no knowledge of the target architecture

**Pre-SSA passes** (`ir/opt.c`) — the monolith below targets splitting into thematic files.
As SSA passes mature (SCCP, GVN, etc.), more pre-SSA passes become redundant and can be removed rather than split.

### Library Layer (extract first)

These are the shared utilities that many passes depend on. Extracting them first eliminates the main obstacle to splitting passes.

#### `ir/opt_utils.h` + `ir/opt_utils.c` (~2000-3000 lines)
**Constant folding & expression evaluation helpers:**
- `ir_opt_eval_const_u64()`
- `ir_opt_eval_const_string()` / `ir_opt_eval_const_string_operand()`
- `ir_opt_fold_strcmp_result()` / `ir_opt_fold_strncmp_result()` / `ir_opt_fold_memcmp_result()` / `ir_opt_fold_memchr_offset()`
- `evaluate_compare_condition()`
- `invert_cond_token()` / `vrp_swap_cmp_tok()` / `vrp_negate_cmp_tok()`
- `is_power_of_2()`

**Basic block & control flow helpers:**
- `ir_opt_build_merge_bitmap()` / `ir_opt_mark_block_starts()`
- `ir_opt_next_non_nop()` / `ir_skip_nops_forward()`
- `ir_has_other_jump_to()`
- `ir_negate_condition()` / `invert_condition()`

**Purity & alias analysis:**
- `ir_opt_is_pure_helper_name()` / `ir_opt_is_flag_cmp_helper_name()`
- `ir_opt_is_pure_fallthrough_instruction()`
- `ir_opt_vreg_address_taken_between()`
- `tcc_ir_is_pure_aeabi()`

**Expression equivalence:**
- `ir_opt_pure_expr_equal()` / `ir_opt_pure_def_equal()` / `ir_opt_nonvreg_expr_equal()`

**Call parameter helpers:**
- `ir_opt_get_call_param_operand()` / `ir_opt_nop_call_params()` / `ir_opt_nop_call_param()` / `ir_opt_change_call_argc()`

**Instruction insertion:**
- `insert_instr_at()`
- `gsym_cse_insert_before()`

**VReg queries:**
- `tcc_ir_vreg_has_single_def()` — static in opt.c, move to vreg.c alongside the existing `tcc_ir_find_defining_instruction` and `tcc_ir_vreg_has_single_use`

#### `ir/opt_du.h` + `ir/opt_du.c` (~150 lines)
**Def-Use table (the #1 reusable abstraction):**
- `IROptDU` struct
- `ir_opt_du_idx()` / `ir_opt_du_build()` / `ir_opt_du_def()` / `ir_opt_du_uses()`
- Already self-contained; used by 6+ passes.

#### `ir/opt_alias.h` + `ir/opt_alias.c` (~500-800 lines)
**Memory alias / stack slot analysis:**
- `ir_opt_store_btype_size_bytes()`
- `ir_opt_stack_slot_range_for_offset()`
- `stackoff_same_slot()` / `operand_references_slot()`
- `is_stack_address_operand()`
- `find_deref_use_operand()`
- Store-entry hashing helpers from SL forward

#### `ir/opt_loop_utils.h` + `ir/opt_loop_utils.c` (~1500-2000 lines)
**Loop analysis & transformation helpers:**
- `find_induction_vars_ex()` / `find_derived_ivs()`
- `transform_derived_iv()` / `try_eliminate_iv_counter()`
- `iv_strength_reduction_core()`
- `find_loop_exit_condition()` / `compute_trip_count()` / `collect_body_instructions()`
- `try_eliminate_loop()` / `try_unroll_loop()` / `try_rotate_loop()`
- `signed_to_unsigned_cond()` / `loop_size_cmp()`
- `InductionVar` / `DerivedIV` structs

---

### Pass Layer (split into thematic files)

After the libraries are extracted, the passes themselves have minimal cross-dependencies and can be grouped by theme.

| File | Passes | Est. Lines | Dependencies |
|------|--------|------------|--------------|
| `opt_cleanup.c` | DCE, NOP compact, DSE, dead_var_store, dead_addrvar, redundant_var_assign, redundant_init_elim | ~3000 | opt_du (optional) |
| `opt_constprop.c` | const_prop, const_prop_tmp, const_var_prop, global_init_prop, value_tracking | ~4500 | opt_utils |
| `opt_branch.c` | branch_folding, stack_addr_nonnull_fold, setif_branch_fuse, stack_bool_diamond, nonneg_branch_fold, float_branch_fold | ~3000 | opt_utils |
| `opt_cse.c` | cse_bool, cse_arith, cse_global_load, globalsym_cse, cse_param_add, bool_idempotent, bool_simplify, bool_pass, global_cse | ~3500 | opt_utils, opt_du |
| `opt_memory.c` | sl_forward, store_redundant, entry_store_prop, block_copy_init, lea_fold, add_deref_fold | ~3500 | opt_utils, opt_alias |
| `opt_fusion.c` | mla_fusion, indexed_memory_fusion, postinc_fusion, loop_postinc_fusion, fusion_pass, deref_indexed_fusion, disp_fusion | ~2500 | opt_du |
| `opt_loop.c` | iv_strength_reduction, iv_sr_with_loops, loop_bound_remat, loop_unroll, loop_rotation, decrement_to_zero, dead_loop_elim, redundant_loop_check | ~4500 | opt_utils, opt_loop_utils, opt_du |
| `opt_promote.c` | copy_prop, var_tmp_fwd, var_to_tmp, stack_addr_cse, float_narrowing | ~2500 | opt_utils, opt_du |
| `opt_peephole.c` | postinc_assign_fold, select, strength_reduction, const_string_calls, const_call_replace | ~1800 | opt_utils |
| `opt_fp_cache.c` | fp_cache wrappers (already thin) | ~50 | — |

**Total after split:** ~22,700 lines distributed across ~14 files (4 library + 10 pass files), none larger than ~4,500 lines. The line count stays the same — this is a reorganization, not an expansion.

The existing `opt_jump_thread.c` (203 lines) can stay as-is or be merged into `opt_branch.c`.

---

## Why This Architecture Helps

1. **Libraries enable new passes:** Want to write a new fusion? You already have `opt_du` (O(1) def/use) and `opt_alias` (stack slot analysis). Want to write a new constant fold? `opt_utils` gives you `eval_const_u64()`.

2. **Faster compilation:** Changing one pass no longer recompiles 22K lines. The linker does the work.

3. **Testability:** Each file has a coherent theme. You can unit-test `opt_du.c` independently of `opt_loop.c`.

4. **Clearer ownership:** `opt_loop.c` owns all loop transforms; `opt_fusion.c` owns all instruction fusions.

---

## Concrete Migration Plan (Step-by-Step)

### Phase 0: Delete Dead Code

**Step 0.1:** Delete `ir/opt_embedded_deref.c`
- This file is not in `Makefile` `IR_FILES` and its sole function `tcc_ir_opt_extract_embedded_deref()` is never called. It is orphaned dead code.

**Step 0.2:** Delete dead stubs in `opt.c`
- Remove `tcc_ir_opt_run_all()` (line 13074) and `tcc_ir_opt_run_by_name()` (line 13081) — empty stubs with `TODO` comments, never called.
- Remove `tcc_ir_opt_return()` (line 9398) — 5-line stub.
- Remove corresponding declarations from `ir/opt.h`.
- **Verify:** Build passes, tests green.

### Phase 1: Extract Shared Libraries (no pass logic moves yet)

**Step 1.1:** Create `ir/opt_du.h` and `ir/opt_du.c`
- Move `IROptDU` struct and `ir_opt_du_*` functions.
- Update `opt.c` to `#include "opt_du.h"`.
- Add `opt_du.c` to `Makefile` (`IR_FILES`).
- **Verify:** Build passes, tests green.

**Step 1.2:** Create `ir/opt_utils.h` and `ir/opt_utils.c`
- Move the constant evaluators, purity tables, BB helpers, expression equivalence, and call param helpers.
- These functions have the most call-sites in different passes (top 5: `ir_opt_get_call_param_operand` 27 sites, `ir_opt_nop_call_params` 15, `ir_opt_pure_expr_equal` 13, `ir_opt_eval_const_string` 10, `ir_opt_next_non_nop` 9), so extracting them unblocks everything else.
- **Verify:** Build passes, tests green.

**Step 1.3:** Create `ir/opt_alias.h` and `ir/opt_alias.c`
- Move stack slot analysis helpers.
- **Verify:** Build passes, tests green.

**Step 1.4:** Create `ir/opt_loop_utils.h` and `ir/opt_loop_utils.c`
- Move IV analysis helpers and loop transformation utilities.
- Named `opt_loop_utils` to avoid collision with `opt_loop.c` (the pass file created in Phase 2).
- **Verify:** Build passes, tests green.

At the end of Phase 1, `opt.c` still contains all pass bodies, but all shared `static` helpers are gone. The file shrinks from ~22,700 to ~16,000 lines.

### Phase 2: Split Passes by Theme

**Step 2.1:** Create `ir/opt_cleanup.c`
- Move DCE, NOP compact, DSE, dead_var_store_elim, dead_addrvar_elim, redundant_var_assign, redundant_init_elim.
- These passes share no private helpers (after Phase 1).
- Remove from `opt.c`.
- **Verify:** Build + tests.

**Step 2.2:** Create `ir/opt_constprop.c`
- Move const_prop, const_prop_tmp, const_var_prop, global_init_prop, value_tracking.
- **Verify:** Build + tests.

**Step 2.3:** Create `ir/opt_branch.c`
- Move branch_folding, stack_addr_nonnull_fold, setif_branch_fuse, stack_bool_diamond, nonneg_branch_fold, float_branch_fold.
- **Verify:** Build + tests.

**Step 2.4:** Create `ir/opt_cse.c`
- Move cse_bool, cse_arith, cse_global_load, globalsym_cse, cse_param_add, bool_idempotent, bool_simplify, bool_pass, global_cse.
- **Verify:** Build + tests.

**Step 2.5:** Create `ir/opt_memory.c`
- Move sl_forward, store_redundant, entry_store_prop, block_copy_init, lea_fold, add_deref_fold.
- **Verify:** Build + tests.

**Step 2.6:** Create `ir/opt_fusion.c`
- Move mla_fusion, indexed_memory_fusion, postinc_fusion, loop_postinc_fusion, fusion_pass, deref_indexed_fusion, disp_fusion.
- **Verify:** Build + tests.

**Step 2.7:** Create `ir/opt_loop.c`
- Move iv_strength_reduction, iv_sr_with_loops, loop_bound_remat, loop_unroll, loop_rotation, decrement_to_zero, dead_loop_elim, redundant_loop_check.
- **Verify:** Build + tests.

**Step 2.8:** Create `ir/opt_promote.c`
- Move copy_prop, var_tmp_fwd, var_to_tmp, stack_addr_cse, float_narrowing.
- **Verify:** Build + tests.

**Step 2.9:** Create `ir/opt_peephole.c`
- Move postinc_assign_fold, select, strength_reduction, const_string_calls, const_call_replace.
- **Verify:** Build + tests.

At the end of Phase 2, `opt.c` contains only:
- FP cache wrappers (~50 lines — can stay or move to `opt_fp_cache.c`)
- Any leftover forward declarations

`opt.c` becomes ~100 lines and can be deleted (FP cache moves to a pass file or its own file).

### Phase 3: Clean Up

**Step 3.1:** Update `ir/opt.h`
- Split declarations into thematic headers (`opt_cleanup.h`, `opt_constprop.h`, etc.) or keep them in the monolithic `opt.h` for simplicity. The existing `opt.h` is already well-organized; keeping it as the public API header is fine.

**Step 3.2:** Update `Makefile`
- Replace `ir/opt.c` with the new files in `IR_FILES`.

**Step 3.3:** Delete or rename the hollowed-out `opt.c`.

**Step 3.4:** Run full test suite.

---

## Risk Mitigation

- **Circular dependencies:** The library layer (`opt_utils`, `opt_du`, `opt_alias`, `opt_loop_utils`) must not depend on any pass layer files. They can depend on `ir.h`, `pool.h`, `vreg.h`, `cfg.h`, `live.h`, `licm.h`.
- **Hidden shared state:** Most helpers are `static` inside `opt.c`. After extraction they become `extern` in the library headers. No global mutable state is introduced.
- **Build breakage:** After each step, verify `make` compiles. The Makefile uses `$(IR_FILES)` so adding a new `.c` file is one-line change.
- **Test regression:** The project has a comprehensive test suite (`tests/`, `tests2/`, `ir_tests/`). Run `make test` after each phase.
- **Rollback:** Commit after each step. If a step breaks tests, `git revert` the single commit. Each step is designed to be independently revertible.
- **Incremental delivery:** Each phase produces a working build. The project can ship at any intermediate state — partially split is strictly better than monolithic.

---

## Estimated Effort

| Phase | Steps | Est. Time |
|-------|-------|-----------|
| Phase 0 | Dead code deletion | 15-30 min |
| Phase 1 | 4 library extractions | 2-3 hours |
| Phase 2 | 9 pass file extractions | 4-6 hours |
| Phase 3 | Cleanup & testing | 1-2 hours |
| **Total** | | **7-12 hours** |

Each step is mechanical: move code, update `#include`s, change `static` to `extern`, fix `Makefile`, build, test.

---

## Phase 4: Pre-SSA Optimization Engine — Building Passes from Blocks

### Relationship to SSA Optimization Engine

The SSA optimization engine (`ir/opt/`) is already implemented and runs on SSA-renamed IR *before* SSA destruction. It uses a generator-based dispatch pattern inspired by `thop_*` instruction builders, with target-specific generators registered from `arch/arm/`.

This Phase 4 covers the **pre-SSA** optimization engine for the ~60 passes in `ir/opt.c` that run *after* SSA destruction on flat IR. As SSA passes mature (SCCP replaces const_prop, GVN replaces cse_arith, SSA-DCE replaces dce, SSA-fusion replaces mla_fusion/indexed_fusion), pre-SSA passes become redundant and are removed rather than converted. The pre-SSA engine is therefore a **migration bridge** — it makes the remaining pre-SSA passes easier to maintain while SSA equivalents are developed.

### Motivation

The current 60 passes share massive structural repetition. Studying representative passes reveals that most fall into one of **4 iteration patterns**, all using the same handful of **analysis blocks** and **transformation primitives**. The engine eliminates boilerplate, lets new optimizations be defined declaratively, and enables the runtime to share analysis across passes that run in sequence.

### Pattern Census

Classifying all ~60 passes by their iteration strategy:

| Pattern | Count | Example Passes |
|---------|-------|----------------|
| **Forward peephole** (match window, transform) | ~25 | MLA fusion, postinc fusion, disp fusion, setif_branch_fuse, bool_idempotent, add_deref_fold |
| **BB-scoped hash** (hash table, reset at BB boundary) | ~8 | cse_arith, cse_bool, globalsym_cse, cse_global_load, cse_param_add, copy_prop |
| **Collect-then-transform** (scan metadata, then apply) | ~12 | const_var_prop, const_prop, value_tracking, dse, dead_var_store, dead_addrvar, redundant_var_assign |
| **CFG/loop-driven** (operate on loop/CFG structure) | ~8 | loop_unroll, loop_rotation, iv_strength_reduction, dead_loop_elim, dce, loop_bound_remat |
| **Stateful forward** (forward with complex carried state) | ~5 | sl_forward, entry_store_prop, vrp, store_redundant |

The peephole pattern (~25 passes) has the most uniform structure and benefits most from an engine. BB-scoped hash (~8 passes) is worth a shared hash table but the passes vary enough that a full rule abstraction adds complexity without proportional benefit. Collect-then-transform (~12 passes) is too diverse for a single engine — DSE alone has 4+ sub-phases with worklists and fixed-point iteration. These stay hand-written but use the shared analysis cache and transform primitives.

**Engine scope: ~25 peephole passes.** Everything else uses shared infrastructure but keeps hand-written iteration.

### Architecture

```
┌───────────────────────────────────────────────────────────────────────────┐
│                        Pipeline Driver (tccgen.c)                        │
├──────────────────────────────┬────────────────────────────────────────────┤
│  SSA Opt Engine (ir/opt/)    │  Pre-SSA Opt Engine (ir/opt_engine.c)     │
│  ✓ Implemented               │  Planned (this phase)                     │
│                              │                                           │
│  Generators + use-def chains │  Peephole rules + analysis cache          │
│  Target gens in arch/arm/    │  Forward-scan loop, trigger_op dispatch   │
│  Runs on SSA form            │  Runs on flat IR after SSA destruction    │
├──────────────────────────────┼────────────────┬──────────────────────────┤
│                              │  Peephole Rules │  Hand-written passes    │
│                              │  (opt_rules_*) │  (CSE, const prop, DSE, │
│                              │  ~25 passes    │  VRP, loop opts) ~35    │
├──────────────────────────────┴────────────────┴──────────────────────────┤
│                     Analysis Cache (opt_analysis.c)                      │
│  def-use (IROptDU), BB boundaries, pred_count                            │
├─────────────────────────────────────────────────────────────────────────-─┤
│             Shared Infra (opt_utils, opt_hash, opt_du)                   │
│  transform primitives, generic hash table, NOP helpers                   │
├──────────────────────────────────────────────────────────────────────────┤
│                          IR Core (ir.h)                                  │
└──────────────────────────────────────────────────────────────────────────┘
```

**Migration strategy:** As SSA optimization passes mature, pre-SSA equivalents are removed:
- SSA `ssa_opt_dce` → replaces pre-SSA `tcc_ir_opt_dce`
- SSA `ssa_opt_cprop` → replaces pre-SSA `tcc_ir_opt_copy_prop`
- SSA `ssa_gen_arm_fuse_mul_add_to_mla` → replaces pre-SSA `tcc_ir_opt_mla_fusion`
- Future SSA SCCP → replaces pre-SSA `tcc_ir_opt_const_prop` + `const_prop_tmp` + `value_tracking`
- Future SSA GVN → replaces pre-SSA `tcc_ir_opt_cse_arith` + `cse_global_load`

### Block 1: Analysis Cache

The key insight from studying the pipeline: the same analyses are rebuilt dozens of times. `ir_opt_du_build()` is called in 7+ passes independently. `is_jump_target` is recomputed by every BB-aware pass. The analysis cache builds each analysis **once per engine run** and invalidates lazily.

```c
/* Shared analysis state — built on demand, invalidated when IR changes */
typedef struct IROptAnalysis {
    TCCIRState *ir;
    uint32_t    generation;     /* incremented on invalidation */

    /* Def-use chains */
    IROptDU     du;
    uint32_t    du_gen;         /* generation when du was built */

    /* Basic block boundaries */
    uint8_t    *is_bb_start;    /* [n] — 1 if instruction starts a BB */
    int        *pred_count;     /* [n] — predecessor count per instruction */
    uint32_t    bb_gen;

    /* Merge bitmap (for VRP, float_branch_fold) */
    uint32_t   *merge_bitmap;
    uint32_t    merge_gen;
} IROptAnalysis;

/* Lazy accessors — build on first use, return cached on subsequent calls */
const IROptDU *ir_opt_require_du(IROptAnalysis *a);
const uint8_t *ir_opt_require_bb(IROptAnalysis *a);
const int     *ir_opt_require_pred(IROptAnalysis *a);

/* Call after any transformation that changes the instruction array */
void ir_opt_invalidate(IROptAnalysis *a);

/* Call after NOP-only changes (most peepholes) — du stays valid */
void ir_opt_invalidate_bb(IROptAnalysis *a);
```

This eliminates redundant O(n) analysis builds when multiple passes run back-to-back without structural changes.

### Block 2: Peephole Rule Engine

Covers ~25 passes. A rule is: **match a multi-instruction pattern via def-use chains, apply a transformation**.

```c
typedef struct IRPeepholeMatch {
    int instr[4];           /* matched instruction indices */
    int n_matched;          /* how many instructions in the match */
    IROperand captured[8];  /* captured operands for the transform */
    int n_captured;
} IRPeepholeMatch;

typedef struct IRPeepholeRule {
    const char *name;                /* for logging/debugging */
    TccIrOp     trigger_op;          /* opcode that triggers matching */
    int         needs_du;            /* 1 if match uses def-use chains */
    int         same_block;          /* 1 to enforce same-BB constraint */
    int         max_window;          /* max lookahead (0 = def-use only) */

    /* Match: return 1 if pattern found, fill out match struct */
    int (*match)(const IROptAnalysis *a, int idx, IRPeepholeMatch *m);

    /* Transform: apply the rewrite, return count of changes */
    int (*transform)(TCCIRState *ir, const IRPeepholeMatch *m);
} IRPeepholeRule;

/* Engine: single forward pass, tries all rules at each instruction */
int ir_opt_run_peephole(TCCIRState *ir, IROptAnalysis *a,
                        const IRPeepholeRule *rules, int nrules);
```

**Example — MLA fusion as a rule:**

```c
static int match_mla(const IROptAnalysis *a, int idx, IRPeepholeMatch *m)
{
    TCCIRState *ir = a->ir;
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op != TCCIR_OP_ADD) return 0;

    const IROptDU *du = ir_opt_require_du((IROptAnalysis *)a);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    /* Try src2 as MUL result, then src1 */
    for (int side = 0; side < 2; side++) {
        IROperand mul_src = side ? s1 : s2;
        IROperand accum   = side ? s2 : s1;
        if (!irop_has_vreg(mul_src)) continue;
        int32_t vr = irop_get_vreg(mul_src);
        int def = ir_opt_du_def(du, vr, idx);
        if (def < 0 || ir->compact_instructions[def].op != TCCIR_OP_MUL)
            continue;
        if (ir_opt_du_uses(du, vr) != 1) continue;
        /* Reject SYMREF, non-lval stack addresses, immediate MUL operands */
        /* ... constraint checks ... */
        m->instr[0] = def;   /* MUL */
        m->instr[1] = idx;   /* ADD */
        m->n_matched = 2;
        m->captured[0] = accum;
        m->n_captured = 1;
        return 1;
    }
    return 0;
}

static int transform_mla(TCCIRState *ir, const IRPeepholeMatch *m)
{
    int mul_idx = m->instr[0], add_idx = m->instr[1];
    IRQuadCompact *mul_q = &ir->compact_instructions[mul_idx];
    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    /* MUL → MLA, copy ADD dest to MUL dest, store accumulator at slot 3 */
    mul_q->op = TCCIR_OP_MLA;
    ir->iroperand_pool[mul_q->operand_base] =
        ir->iroperand_pool[add_q->operand_base]; /* dest */
    int accum_idx = mul_q->operand_base + 3;
    while (ir->iroperand_pool_count <= accum_idx)
        tcc_ir_pool_add(ir, IROP_NONE);
    ir->iroperand_pool[accum_idx] = m->captured[0]; /* accumulator */
    add_q->op = TCCIR_OP_NOP;
    return 1;
}

static const IRPeepholeRule mla_rule = {
    .name       = "mla_fusion",
    .trigger_op = TCCIR_OP_ADD,
    .needs_du   = 1,
    .same_block = 1,
    .match      = match_mla,
    .transform  = transform_mla,
};
```

**Key design choices:**

- **`trigger_op`** avoids calling `match` on irrelevant opcodes — the engine only calls match when the current instruction's opcode matches. For rules that trigger on multiple opcodes (e.g., LOAD or STORE), use `trigger_op = -1` and check inside `match`.
- **`same_block` flag** tells the engine to do the BB-boundary check generically (loop from `def_idx+1..idx` checking for JUMP/JUMPIF), so every rule doesn't reimplement it.
- **`needs_du`** tells the engine to ensure the def-use analysis is built before calling this rule.

**Batching multiple rules in one pass:**

```c
/* Instead of 7 separate forward passes for fusion, one pass tries all rules */
const IRPeepholeRule fusion_rules[] = {
    mla_rule,
    indexed_memory_rule,
    deref_indexed_rule,
    disp_fusion_rule,
    postinc_rule,
    add_deref_fold_rule,
    lea_fold_rule,
};
ir_opt_run_peephole(ir, &analysis, fusion_rules, 7);
```

This replaces 7 separate O(n) passes with 1 pass. Each instruction is visited once; the engine tries rules whose `trigger_op` matches.

### Block 3: BB-Scoped Hash Engine

Covers ~8 CSE/copy-prop passes. Pattern: maintain a hash table that resets at BB boundaries.

```c
typedef struct IRHashRule {
    const char *name;

    /* Which opcodes participate */
    int (*is_eligible)(TccIrOp op);

    /* Hash an instruction's expression (opcode + operands) */
    uint32_t (*hash_expr)(TCCIRState *ir, int idx);

    /* Compare two instructions for expression equivalence */
    int (*exprs_equal)(TCCIRState *ir, int idx1, int idx2);

    /* Which opcodes force a table reset (calls, jumps, etc.) */
    int (*is_boundary)(TccIrOp op);
} IRHashRule;

/* Engine: forward pass with hash table, reset at boundaries */
int ir_opt_run_bb_hash(TCCIRState *ir, IROptAnalysis *a,
                       const IRHashRule *rule);
```

The engine manages the hash table lifecycle — allocation, insertion, lookup, reset at boundaries, cleanup. Rules only supply the expression-specific logic.

**Example — arith CSE as a rule:**

```c
static int arith_eligible(TccIrOp op)
{
    return op == TCCIR_OP_ADD || op == TCCIR_OP_SUB ||
           op == TCCIR_OP_MUL || op == TCCIR_OP_AND ||
           op == TCCIR_OP_OR  || op == TCCIR_OP_XOR ||
           op == TCCIR_OP_SHL || op == TCCIR_OP_SHR ||
           op == TCCIR_OP_SAR;
}

static uint32_t arith_hash(TCCIRState *ir, int idx)
{
    IRQuadCompact *q = &ir->compact_instructions[idx];
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    return q->op * 31 + operand_hash(s1) * 17 + operand_hash(s2) * 13;
}

static const IRHashRule arith_cse_rule = {
    .name        = "cse_arith",
    .is_eligible = arith_eligible,
    .hash_expr   = arith_hash,
    .exprs_equal = arith_exprs_equal,
    .is_boundary = default_cf_boundary,  /* JUMP, JUMPIF, CALL, RET */
};
```

**Batching multiple hash rules:**

CSE passes that share the same boundary semantics can be merged into a single forward pass with a combined hash table:

```c
/* Bool CSE + arith CSE + global load CSE in one pass */
const IRHashRule *cse_rules[] = {
    &arith_cse_rule,
    &bool_cse_rule,
    &global_load_cse_rule,
};
ir_opt_run_bb_hash_multi(ir, &analysis, cse_rules, 3);
```

### Block 4: Collect-Transform Engine

Covers ~12 passes that scan the IR to build metadata, then iterate again to apply changes.

```c
typedef struct IRCollectRule {
    const char *name;
    size_t state_size;          /* sizeof(pass-specific state) */

    /* Phase 1: called for each instruction, collect metadata */
    void (*collect)(void *state, TCCIRState *ir, int idx);

    /* Between phases: finalize collected data */
    void (*finalize)(void *state, TCCIRState *ir);

    /* Phase 2: called for each instruction, apply transforms */
    int (*transform)(void *state, TCCIRState *ir, int idx);
} IRCollectRule;

int ir_opt_run_collect(TCCIRState *ir, IROptAnalysis *a,
                       const IRCollectRule *rule);
```

**Example — const_var_prop as a rule:**

```c
typedef struct {
    VarInfo *vars;
    int n_vars, cap_vars;
} ConstVarState;

static void cv_collect(void *s, TCCIRState *ir, int idx)
{
    ConstVarState *st = s;
    IRQuadCompact *q = &ir->compact_instructions[idx];
    /* Track VAR definitions: if ASSIGN with immediate source, mark constant */
    /* If second def or address-taken, mark non-constant */
}

static int cv_transform(void *s, TCCIRState *ir, int idx)
{
    ConstVarState *st = s;
    /* Replace LOAD from known-constant VAR with ASSIGN #imm */
}
```

### Block 5: Transform Primitives

Reusable transformation operations used by all rule types:

```c
/* NOP out an instruction */
void ir_xform_nop(TCCIRState *ir, int idx);

/* Replace instruction with ASSIGN from one vreg/imm to dest */
void ir_xform_assign(TCCIRState *ir, int idx, IROperand src);

/* Change opcode, keeping existing operands */
void ir_xform_change_op(TCCIRState *ir, int idx, TccIrOp new_op);

/* Rewrite a specific operand slot */
void ir_xform_set_src1(TCCIRState *ir, int idx, IROperand op);
void ir_xform_set_src2(TCCIRState *ir, int idx, IROperand op);
void ir_xform_set_dest(TCCIRState *ir, int idx, IROperand op);

/* Allocate new operand pool slots (for MLA, indexed, etc.) */
int ir_xform_alloc_pool(TCCIRState *ir, int n_slots);

/* Same-block check between two instruction indices */
int ir_xform_same_block(TCCIRState *ir, int from, int to);
```

### Pipeline Integration

The engine doesn't replace the pipeline driver — it replaces the **pass bodies**. The driver in `tccgen.c` stays the same, but instead of calling 60 individual pass functions, it calls engine runs:

```c
/* Before (current): 7 separate fusion passes, 7 O(n) scans */
tcc_ir_opt_fusion_pass(ir, do_mla, do_indexed);
tcc_ir_opt_deref_indexed_fusion(ir);
tcc_ir_opt_disp_fusion(ir);
tcc_ir_opt_add_deref_fold(ir);
tcc_ir_opt_lea_fold(ir);
tcc_ir_opt_postinc_fusion(ir);

/* After (engine): 1 engine run + 1 hand-written pass */
ir_opt_run_peephole(ir, &a, fusion_rules, 6);  /* 6 fusions in 1 pass */
tcc_ir_opt_add_deref_fold(ir);                  /* stays separate (inserts instrs) */
```

### Detailed Implementation Plans

---

#### Phase 4.1: Analysis Cache (`ir/opt_analysis.h` + `ir/opt_analysis.c`)

**Goal:** Provide a shared, lazily-built analysis context that multiple passes and engine runs can share, eliminating redundant O(n) analysis builds.

**What moves here:**
- `IROptDU` struct and `ir_opt_du_build/def/uses/idx` functions (from opt.c lines 13348-13443)
- `ir_opt_build_merge_bitmap` (from opt.c lines 5624-5658)
- `ir_opt_mark_block_starts` (from opt.c lines 5690-5704)
- New: `ir_opt_build_pred_count` — extracted from the inline computation in `sl_forward` (lines 10232-10259)

**New struct:**
```c
typedef struct IROptAnalysis {
    TCCIRState *ir;
    int         n;              /* cached ir->next_instruction_index */
    uint32_t    generation;     /* bumped on invalidate */

    /* Def-use chains — built by ir_opt_du_build */
    IROptDU     du;
    uint32_t    du_gen;         /* 0 = not built */

    /* Predecessor counts — int[n] */
    int        *pred_count;
    uint32_t    pred_gen;

    /* Merge bitmap — uint8_t[(n+7)/8] */
    uint8_t    *merge_bitmap;
    uint32_t    merge_gen;
} IROptAnalysis;
```

**API:**
```c
/* Lifecycle */
void ir_opt_analysis_init(IROptAnalysis *a, TCCIRState *ir);
void ir_opt_analysis_free(IROptAnalysis *a);

/* Lazy builders — return cached result if generation matches */
const IROptDU *ir_opt_require_du(IROptAnalysis *a);
const int     *ir_opt_require_pred(IROptAnalysis *a);
const uint8_t *ir_opt_require_merge(IROptAnalysis *a);

/* Invalidation */
void ir_opt_analysis_invalidate(IROptAnalysis *a);       /* full — du + pred + merge */
void ir_opt_analysis_invalidate_pred(IROptAnalysis *a);  /* pred + merge only (after NOP-only changes) */
```

**Implementation details:**
- `ir_opt_require_du`: checks `du_gen == generation`; if stale, calls `ir_opt_du_build(a->ir, &a->du)` and sets `du_gen = generation`. The existing `ir_opt_du_build` allocates `def` and `use` arrays in a single `tcc_malloc` call — keep this.
- `ir_opt_require_pred`: recomputes `pred_count[n]` by scanning JUMP/JUMPIF targets + fall-through edges. Also cleans up stale `is_jump_target` flags (logic from `sl_forward` lines 10232-10267).
- `ir_opt_require_merge`: calls `ir_opt_build_merge_bitmap` (already exists).
- `ir_opt_analysis_invalidate`: bumps `generation`, frees `du.def` if allocated.
- **Key constraint:** the DU table becomes invalid when any instruction changes opcode (not just NOP — e.g., MUL→MLA changes which vregs are defined). Peephole transforms that change opcodes must invalidate. NOP-only changes preserve DU validity since NOPs have no def/use.

**Migration path:**
1. Move `IROptDU` and its 4 functions from opt.c → opt_analysis.c (change `static` → extern).
2. Move `ir_opt_build_merge_bitmap`, `ir_opt_mark_block_starts` → opt_analysis.c.
3. Extract `pred_count` computation from `sl_forward` into `ir_opt_build_pred_count`.
4. Add `IROptAnalysis` wrapper struct with lazy accessors.
5. Update all 6 callers of `ir_opt_du_build` in opt.c to `#include "opt_analysis.h"`.
6. **Do NOT change caller behavior yet** — each pass still creates its own local `IROptDU` or calls the cache. The engine (Phase 4.3) will be the first user of the cache.

**Files touched:** `ir/opt.c` (remove ~120 lines), new `ir/opt_analysis.h` (~60 lines), new `ir/opt_analysis.c` (~250 lines), `Makefile` (add to IR_FILES).

**Verify:** `make cross && make test -j16`. All existing passes must produce identical results.

**Est. time:** 2-3 hours.

---

#### Phase 4.2: Transform Primitives (`ir/opt_xform.h` + `ir/opt_xform.c`)

**Goal:** Extract the 6 most common transformation patterns (currently inlined 81+155 times across opt.c) into named, reusable functions.

**Functions to create:**

```c
/* 1. NOP out an instruction (81 occurrences of q->op = TCCIR_OP_NOP in opt.c) */
static inline void ir_xform_nop(TCCIRState *ir, int idx);

/* 2. Replace instruction with ASSIGN from src to existing dest */
void ir_xform_replace_with_assign(TCCIRState *ir, int idx, IROperand src);

/* 3. Replace instruction with ASSIGN from immediate value to existing dest */
void ir_xform_replace_with_imm(TCCIRState *ir, int idx, int64_t value, int btype);

/* 4. Same-block check: return 1 if no JUMP/JUMPIF between from..to */
int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx);

/* 5. Allocate N contiguous operand pool slots, return base index. -1 on failure. */
int ir_xform_alloc_pool(TCCIRState *ir, int n_slots);

/* 6. NOP an instruction and decrement use counts in a DU table (for cascading elimination) */
void ir_xform_nop_with_du(TCCIRState *ir, int idx, IROptDU *du);
```

**What this replaces:**
- `ir_xform_nop`: Currently 81 instances of `q->op = TCCIR_OP_NOP` — most are fine as-is (a single assignment), but wrapping it documents intent and allows adding logging/stats.
- `ir_xform_replace_with_assign`: Currently ~40 instances of `q->op = TCCIR_OP_ASSIGN; tcc_ir_set_src1(ir, i, new_src); tcc_ir_set_src2(ir, i, IROP_NONE)` — this 3-line pattern appears in every CSE match, every constant fold, etc.
- `ir_xform_same_block`: Currently reimplemented in every fusion pass as a `for (j = idx1+1; j < idx2; j++) if (JUMP||JUMPIF) same_block=0` loop. Appears 6+ times.
- `ir_xform_alloc_pool`: The pattern `while (ir->iroperand_pool_count <= target_idx) tcc_ir_pool_add(ir, IROP_NONE)` appears in every fusion pass.

**Implementation details:**
- `ir_xform_nop`: Inline in header. Just sets `ir->compact_instructions[idx].op = TCCIR_OP_NOP`.
- `ir_xform_replace_with_assign`: Sets op to ASSIGN, sets src1 to the given operand, clears src2. Preserves existing dest operand.
- `ir_xform_same_block`: Loop from `from_idx+1` to `to_idx`, check for JUMP/JUMPIF/`is_jump_target`. Returns 0 on first hit.
- `ir_xform_alloc_pool`: Calls `tcc_ir_pool_ensure(ir, n)`, then does `n` sequential `tcc_ir_pool_add(ir, IROP_NONE)`, returns the base index of the first slot.

**Migration path:**
1. Create `ir/opt_xform.h` and `ir/opt_xform.c`.
2. **Do NOT bulk-replace** existing code yet. The primitives are available for new engine rules (Phase 4.4+) and for gradual migration of existing passes.
3. Convert the most duplicated pattern first: `ir_xform_same_block` used by all 6 fusion passes.

**Files touched:** New `ir/opt_xform.h` (~30 lines), new `ir/opt_xform.c` (~80 lines), `Makefile`.

**Verify:** Build-only (no behavior change yet).

**Est. time:** 1 hour.

---

#### Phase 4.3: Peephole Rule Engine (`ir/opt_engine.h` + `ir/opt_engine.c`)

**Goal:** A single forward-pass engine that tries multiple pattern-matching rules at each instruction, sharing a pre-built DU table.

**Rule struct:**
```c
typedef struct IRPeepholeMatch {
    int instr[8];           /* matched instruction indices */
    int n_matched;
    IROperand captured[8];  /* captured operands for transform */
    int n_captured;
} IRPeepholeMatch;

typedef struct IRPeepholeRule {
    const char *name;
    TccIrOp     trigger_op;     /* -1 = match any opcode */
    int         needs_du;
    int         same_block;     /* engine checks same-block for instr[0]..instr[n-1] */

    int (*match)(TCCIRState *ir, const IROptDU *du, int idx, IRPeepholeMatch *m);
    int (*transform)(TCCIRState *ir, IRPeepholeMatch *m);
} IRPeepholeRule;
```

**Engine function:**
```c
int ir_opt_run_peephole(TCCIRState *ir, IROptAnalysis *a,
                        const IRPeepholeRule *rules, int nrules);
```

**Engine loop (pseudocode):**
```c
int ir_opt_run_peephole(TCCIRState *ir, IROptAnalysis *a,
                        const IRPeepholeRule *rules, int nrules)
{
    int n = ir->next_instruction_index;
    int changes = 0;
    int any_needs_du = 0;
    const IROptDU *du = NULL;

    /* Check if any rule needs DU */
    for (int r = 0; r < nrules; r++)
        if (rules[r].needs_du) { any_needs_du = 1; break; }
    if (any_needs_du)
        du = ir_opt_require_du(a);

    /* Build trigger_op dispatch table: for each opcode, list of rule indices */
    /* (small array — TccIrOp range is ~80 opcodes) */

    for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP) continue;

        /* Try rules whose trigger_op matches q->op (or trigger_op == -1) */
        for (int r = 0; r < nrules; r++) {
            if (rules[r].trigger_op >= 0 && rules[r].trigger_op != q->op)
                continue;

            IRPeepholeMatch m = {0};
            if (!rules[r].match(ir, du, i, &m))
                continue;

            /* Engine-level same-block check if requested */
            if (rules[r].same_block && m.n_matched >= 2) {
                if (!ir_xform_same_block(ir, m.instr[0], m.instr[m.n_matched - 1]))
                    continue;
            }

            changes += rules[r].transform(ir, &m);
            break;  /* first matching rule wins; skip to next instruction */
        }
    }
    return changes;
}
```

**Design decisions:**
- **`trigger_op` dispatch:** Most rules trigger on a single opcode (ADD for MLA, LOAD/STORE for indexed). The engine skips rules whose trigger doesn't match, avoiding ~24 unnecessary function pointer calls per instruction. For rules that trigger on 2 opcodes (e.g., indexed fusion triggers on both LOAD and STORE), use `trigger_op = -1` and check inside `match`.
- **First-match-wins:** When a rule matches, the engine skips remaining rules for that instruction and moves to the next. Rules are tried in array order, so put higher-priority rules first.
- **DU sharing:** The engine builds the DU table once via `ir_opt_require_du`. All rules receive the same `const IROptDU *du`. After the engine run, if any instruction changed opcode (not just NOP), the analysis is invalidated.
- **No `run_bb_hash` or `run_collect` engines:** After studying the actual CSE and collect-transform passes, these patterns are too diverse for a useful generic engine. ArithCSE has commutative matching + invalidation-on-write; BoolCSE uses a different key type; DSE has 4+ sub-phases with worklists. These passes benefit from shared infrastructure (analysis cache, transform primitives, generic hash table from Phase 1.2's `opt_utils`) but not from a rule abstraction.

**Files:** New `ir/opt_engine.h` (~40 lines), new `ir/opt_engine.c` (~150 lines), `Makefile`.

**Verify:** Build-only (no rules defined yet).

**Est. time:** 2 hours.

---

#### Phase 4.4: Fusion Rules (`ir/opt_rules_fusion.c`)

**Note:** SSA equivalents for MLA fusion, indexed memory fusion, and MUL→SHL strength reduction already exist as generators in `arch/arm/ssa_opt_arm.c`. Once the SSA optimization engine fully replaces pre-SSA passes, these pre-SSA fusion rules become unnecessary. This phase is only needed if the pre-SSA engine outlives the SSA migration.

**Goal:** Convert the 7 fusion passes into peephole rules that run in a single engine pass.

**Current passes to convert:**

| Pass | trigger_op | Lines | Pattern |
|------|-----------|-------|---------|
| `mla_fusion` | ADD | 13467-13716 | ADD where src is single-use MUL → MLA |
| `indexed_memory_fusion` | LOAD, STORE | 13717-13990 | SHL+ADD+LOAD/STORE → LOAD/STORE_INDEXED |
| `deref_indexed_fusion` | (any ALU with deref) | 14994-15205 | SHL+ADD+deref_ALU → LOAD_INDEXED+ALU |
| `disp_fusion` | LOAD, STORE, ASSIGN | 15206-15475 | ADD(base,#imm)+LOAD/STORE → LOAD/STORE_INDEXED |
| `postinc_fusion` | LOAD, STORE | 13991-14265 | LOAD/STORE+ADD(addr,#stride) → LOAD/STORE_POSTINC |
| `lea_fold` | (any with deref src) | 15476-15796 | LEA+ADD+deref → direct StackLoc access |
| `add_deref_fold` | (any with deref src) | 2248-2422 | ADD(base,#imm)+deref_use → LOAD_INDEXED+plain use |

**Conversion strategy for each:**

**Rule 1: `match_mla` + `transform_mla`**
- **trigger_op:** `TCCIR_OP_ADD`
- **match:** Check src1 and src2 for single-use MUL via `ir_opt_du_def`. Reject SYMREF operands, immediate MUL operands, non-lval stack addresses. Check accumulator def is before MUL.
- **transform:** Change MUL→MLA, copy ADD dest to MUL dest, allocate pool slot 3 for accumulator, NOP the ADD.
- **same_block:** 1 (engine does the check generically)
- **Estimated:** ~60 lines match, ~25 lines transform (vs. current ~250 lines including iteration boilerplate and DU build)

**Rule 2: `match_indexed_mem` + `transform_indexed_mem`**
- **trigger_op:** -1 (triggers on both LOAD and STORE, check inside match)
- **match:** Find ADD def of address vreg, then SHL def of ADD source. Check single-use chain, shift amount 2/3/4, no local/llocal operands.
- **transform:** Change LOAD→LOAD_INDEXED or STORE→STORE_INDEXED, allocate 4 pool slots (dest/base/index/scale), NOP SHL and ADD.
- **same_block:** 1
- **Estimated:** ~70 lines match, ~40 lines transform

**Rule 3: `match_deref_indexed` + `transform_deref_indexed`**
- **trigger_op:** -1 (any ALU op with deref operand)
- **match:** Find deref operand in src1/src2, trace through ADD→SHL chain.
- **transform:** Reuse the ADD slot for LOAD_INDEXED, rewrite ALU operand to remove deref flag.
- **Estimated:** ~60 lines match, ~35 lines transform

**Rule 4: `match_disp` + `transform_disp`**
- **trigger_op:** -1 (LOAD, STORE, or ASSIGN with lval)
- **match:** Find ADD(base, #imm) def of address, check single-use, no SYMREF.
- **transform:** Change to _INDEXED variant with scale=0 and immediate index, NOP the ADD.
- **Estimated:** ~50 lines match, ~30 lines transform

**Rule 5: `match_postinc` + `transform_postinc`**
- **trigger_op:** -1 (LOAD or STORE)
- **match:** Lookahead ≤10 instructions for ADD(addr, #stride) after the LOAD/STORE. Check single-use of address, no intervening write to address vreg.
- **transform:** Change to _POSTINC variant, NOP the ADD.
- **Estimated:** ~55 lines match, ~25 lines transform

**Rule 6: `match_lea_fold` + `transform_lea_fold`**
- **trigger_op:** -1 (any op with deref source)
- **match:** Source has deref flag, trace def chain: LEA(StackLoc) → optional ADD(#offset).
- **transform:** Replace deref source with direct StackLoc[adjusted_offset], NOP LEA and ADD.
- **Estimated:** ~65 lines match, ~30 lines transform

**Rule 7: `match_add_deref_fold` + `transform_add_deref_fold`**
- **trigger_op:** -1 (any op with deref source from ADD)
- **match:** Source with deref, defined by ADD(base, #imm). Base is not a stack address.
- **transform:** Insert LOAD_INDEXED before the use, rewrite source to plain vreg.
- **Note:** This rule INSERTS an instruction. The peephole engine must handle this — after transform, invalidate analysis and adjust the loop counter. Alternatively, keep this as a hand-written pass since it's the only fusion that inserts.
- **Estimated:** ~50 lines match, ~35 lines transform

**Rule ordering in array:**
```c
const IRPeepholeRule fusion_rules[] = {
    mla_rule,              /* ADD → MLA: must run before disp (both trigger on ADD results) */
    indexed_mem_rule,      /* SHL+ADD+LOAD/STORE → LOAD_INDEXED */
    deref_indexed_rule,    /* SHL+ADD+deref_ALU → LOAD_INDEXED+ALU */
    disp_rule,             /* ADD(base,#imm)+LOAD/STORE → LOAD_INDEXED */
    postinc_rule,          /* LOAD/STORE+ADD → LOAD_POSTINC */
    lea_fold_rule,         /* LEA+ADD+deref → direct StackLoc */
};
/* add_deref_fold stays hand-written (inserts instructions) */
```

**Pipeline integration (tccgen.c change):**
```c
/* Before: 6 separate calls */
tcc_ir_opt_fusion_pass(ir, do_mla, do_indexed);
tcc_ir_opt_deref_indexed_fusion(ir);
tcc_ir_opt_disp_fusion(ir);
tcc_ir_opt_lea_fold(ir);
tcc_ir_opt_postinc_fusion(ir);

/* After: 1 engine call */
IROptAnalysis a;
ir_opt_analysis_init(&a, ir);
ir_opt_run_peephole(ir, &a, fusion_rules, 6);
ir_opt_analysis_free(&a);

/* add_deref_fold stays separate (inserts instructions) */
tcc_ir_opt_add_deref_fold(ir);
```

**Testing strategy:**
1. Convert `mla_rule` first. Run `make test`. Compare IR dump output for a test case that triggers MLA fusion (e.g., `tests/ir_tests/60_mla_test.c` or similar).
2. Add `indexed_mem_rule`. Run tests.
3. Continue one rule at a time, testing after each.
4. After all 6 rules work, remove the old pass functions from opt.c and their declarations from opt.h.

**Files:** New `ir/opt_rules_fusion.c` (~500 lines), modified `tccgen.c` (~10 lines), modified `ir/opt.c` (remove ~2200 lines), modified `ir/opt.h` (remove 7 declarations), `Makefile`.

**Verify:** `make cross && make test -j16 && make test-gcc-torture-compile`.

**Est. time:** 4-6 hours (most time goes to testing each rule individually).

---

#### Phase 4.5: Branch Folding Rules (`ir/opt_rules_branch.c`)

**Goal:** Convert 5 branch folding passes into peephole rules.

**Current passes to convert:**

| Pass | trigger_op | Lines | Pattern |
|------|-----------|-------|---------|
| `branch_folding` | JUMPIF | 16049-16206 | CMP(imm,imm)+JUMPIF → JUMP or NOP |
| `setif_branch_fuse` | JUMPIF | 16629-16755 | CMP+SETIF+TEST_ZERO+JUMPIF → CMP+JUMPIF |
| `nonneg_branch_fold` | JUMPIF | 12381-12694 | JUMPIF on known non-negative value → fold |
| `float_branch_fold` | JUMPIF | 6767-7017 | Redundant FCMP+JUMPIF on fall-through path |
| `stack_addr_nonnull_fold` | JUMPIF | 16207-16628 | TEST_ZERO(stack_addr)+JUMPIF → fold (stack is never NULL) |

**Key observation:** All 5 passes trigger on JUMPIF and look backward at the defining instruction chain. This is a natural fit for peephole rules with `trigger_op = TCCIR_OP_JUMPIF`.

**Rule ordering matters:** `branch_folding` (constant conditions) should run first — it's the cheapest check and eliminates the most code. `setif_branch_fuse` should run after `branch_folding` since folding may expose new setif patterns.

```c
const IRPeepholeRule branch_rules[] = {
    branch_fold_rule,          /* CMP(imm,imm)+JUMPIF → constant fold */
    setif_branch_fuse_rule,    /* CMP+SETIF+TEST_ZERO+JUMPIF → CMP+JUMPIF */
    stack_nonnull_rule,        /* TEST_ZERO(stack_addr)+JUMPIF → fold */
    nonneg_branch_rule,        /* known non-negative branch fold */
    float_branch_rule,         /* redundant FCMP fold */
};
```

**Conversion strategy:**

**Rule 1: `match_branch_fold`**
- **match:** Current instruction is JUMPIF. Look backward (skip NOPs) for TEST_ZERO or CMP. If both operands of the comparison are immediate, evaluate the condition at compile time using `evaluate_compare_condition(val1, val2, tok)`.
- **transform:** If condition is always true → convert JUMPIF to unconditional JUMP, NOP the CMP/TEST_ZERO. If always false → NOP both JUMPIF and CMP/TEST_ZERO.
- **needs_du:** 0 (just backward scan)
- **Estimated:** ~40 lines match, ~15 lines transform

**Rule 2: `match_setif_branch_fuse`**
- **match:** JUMPIF at `i`, look back for TEST_ZERO at `i-1` (skip NOPs), SETIF at `i-2`, CMP at `i-3`. Check SETIF result is single-use (via `tcc_ir_vreg_has_single_use`). Check no `is_jump_target` on intermediate instructions.
- **transform:** Rewrite JUMPIF to use original CMP condition (possibly inverted), NOP the SETIF and TEST_ZERO.
- **needs_du:** 0 (uses existing `tcc_ir_vreg_has_single_use` extern)
- **Estimated:** ~45 lines match, ~20 lines transform

**Rule 3: `match_stack_nonnull`**
- **match:** JUMPIF preceded by TEST_ZERO, where the tested value is a stack address (STACKOFF operand). Stack addresses are never NULL on embedded targets.
- **transform:** If branch-on-zero → NOP both (stack addr is always non-zero). If branch-on-nonzero → convert to unconditional JUMP.
- **needs_du:** 1 (needs to trace the tested value's definition to find stack address)
- **Note:** This pass is the most complex of the 5 — it handles multi-level chains like `LEA(StackLoc) → ASSIGN → TEST_ZERO → JUMPIF`. The match function needs to follow def-use chains up to 3 levels deep. Currently 420 lines — even as a rule it will be ~120 lines for match alone. Consider keeping this hand-written if the match function is too complex.
- **Estimated:** ~120 lines match, ~30 lines transform

**Rule 4: `match_nonneg_branch`**
- **match:** JUMPIF with unsigned comparison, where one operand has a known non-negative value (from VRP or from being the result of a known-nonneg operation like ABS).
- **needs_du:** 1
- **Note:** This pass uses `ir_opt_build_merge_bitmap` and tracks value ranges through the IR. It's closer to a dataflow pass than a peephole. **Recommend keeping hand-written** — use analysis cache for merge bitmap but don't force it into a rule.

**Rule 5: `match_float_branch`**
- **match:** JUMPIF where the tested value is an FCMP result that was already tested on the fall-through path.
- **needs_du:** 1
- **Note:** Uses merge bitmap to detect fall-through paths. Similar complexity to nonneg_branch. **Recommend keeping hand-written.**

**Revised scope:** Convert 3 passes (branch_folding, setif_branch_fuse, stack_addr_nonnull_fold) to rules. Keep nonneg_branch_fold and float_branch_fold hand-written (they use merge bitmap / value tracking that doesn't fit the simple peephole model).

**Pipeline integration:**
```c
/* Before */
tcc_ir_opt_branch_folding(ir);
tcc_ir_opt_stack_addr_nonnull_fold(ir);
tcc_ir_opt_setif_branch_fuse(ir);

/* After */
ir_opt_run_peephole(ir, &a, branch_rules, 3);

/* These stay hand-written */
tcc_ir_opt_nonneg_branch_fold(ir);
tcc_ir_opt_float_branch_fold(ir);
```

**Files:** New `ir/opt_rules_branch.c` (~300 lines), modified `tccgen.c`, modified `ir/opt.c` (remove ~700 lines), `Makefile`.

**Verify:** `make cross && make test -j16`. Branch folding is heavily exercised by the test suite — any regression will be caught.

**Est. time:** 3-4 hours.

---

#### Phase 4.6: Boolean Simplification Rules (`ir/opt_rules_bool.c`)

**Goal:** Convert 3 boolean simplification passes into peephole rules.

| Pass | trigger_op | Lines | Pattern |
|------|-----------|-------|---------|
| `bool_idempotent` | BOOL_AND, BOOL_OR | 8437-8503 | `a && a → a`, `a && 1 → a`, `a \|\| 0 → a` |
| `bool_simplify` | BOOL_AND, BOOL_OR | 8505-8584 | `!a && !b → !(a \|\| b)`, double-negation |
| `bool_pass` (combined) | BOOL_AND, BOOL_OR | 15797-16048 | Runs bool_idempotent + cse_bool in one loop |

**Note:** `bool_pass` is already a combined pass that merges `bool_idempotent` and `cse_bool` into one forward loop. The CSE part uses a hash table — it won't fit the peephole model. Strategy: extract only the idempotent/simplify patterns as peephole rules. Keep `cse_bool` hash table logic in `bool_pass` or in a separate hand-written pass.

**Rule 1: `match_bool_idempotent`**
- **trigger_op:** -1 (matches BOOL_AND and BOOL_OR)
- **match:** Check if both operands are the same vreg (a && a), or if one operand is a constant 0/1.
- **transform:** Replace with ASSIGN from the non-trivial operand (or ASSIGN #0/#1 for tautology/contradiction).
- **Estimated:** ~30 lines match, ~15 lines transform

**Rule 2: `match_bool_simplify`**
- **trigger_op:** -1 (matches BOOL_AND and BOOL_OR)
- **match:** Check if both operands are BOOL_NOT results (DU lookup). If so, De Morgan's law applies.
- **transform:** Replace with BOOL_NOT(opposite_op(a, b)).
- **needs_du:** 1
- **Estimated:** ~35 lines match, ~20 lines transform

**Pipeline integration:**
```c
/* Before */
tcc_ir_opt_bool_pass(ir, do_idempotent, do_cse);
tcc_ir_opt_bool_simplify(ir);

/* After */
ir_opt_run_peephole(ir, &a, bool_rules, 2);
/* CSE bool stays as hand-written hash pass */
tcc_ir_opt_cse_bool(ir);
```

**Files:** New `ir/opt_rules_bool.c` (~120 lines), modified `ir/opt.c` (remove ~200 lines), `Makefile`.

**Verify:** `make cross && make test -j16`.

**Est. time:** 1-2 hours.

---

#### Phase 4.7: Generic Hash Table for CSE Passes (`ir/opt_hash.h` + `ir/opt_hash.c`)

**Goal:** Instead of a rule engine for CSE, provide a **reusable hash table** that CSE passes use directly, eliminating the 4 separate hash table implementations.

**Current state:** 4 different hash table structs (BoolCSEEntry/64, ArithCSEEntry/256, StoreEntry/128, GSymEntry/linear-16), each with their own alloc/lookup/insert/clear functions. The hash functions differ but the lifecycle is identical: init → forward scan → insert or lookup at each instruction → clear at BB boundary → free at end.

**Shared hash table:**
```c
typedef struct IROptHashEntry {
    uint32_t hash;
    int      instruction_idx;
    int32_t  result_vr;         /* vreg that holds the computed result */
    int      extra[4];          /* pass-specific payload (operand vregs, flags) */
    struct IROptHashEntry *next;
} IROptHashEntry;

typedef struct IROptHashTable {
    IROptHashEntry **buckets;
    int              n_buckets;        /* 64, 128, or 256 */
    IROptHashEntry  *entry_pool;       /* pre-allocated entries (n instructions max) */
    int              entry_count;
} IROptHashTable;

/* Lifecycle */
void ir_opt_hash_init(IROptHashTable *ht, int n_buckets, int max_entries);
void ir_opt_hash_free(IROptHashTable *ht);
void ir_opt_hash_clear(IROptHashTable *ht);  /* reset all buckets, reuse pool */

/* Operations */
IROptHashEntry *ir_opt_hash_find(IROptHashTable *ht, uint32_t hash,
                                  int (*eq)(const IROptHashEntry *entry, const void *key),
                                  const void *key);
IROptHashEntry *ir_opt_hash_insert(IROptHashTable *ht, uint32_t hash);
```

**Key design:** The `entry_pool` is pre-allocated to `max_entries` (= instruction count). Entries are allocated from the pool sequentially (bump allocator). `ir_opt_hash_clear` resets `entry_count = 0` and zeros the bucket array — O(n_buckets), not O(entries). This matches the current ArithCSE pattern where clearing is done via `memset(hash_table, 0, sizeof(hash_table))`.

**Migration path:**
1. Create `ir/opt_hash.h` + `ir/opt_hash.c` with the generic table.
2. Rewrite `cse_arith` to use `IROptHashTable` instead of its local `ArithCSEEntry hash_table[256]`. The pass body stays the same — only the hash table alloc/lookup/insert/clear calls change.
3. Rewrite `cse_bool` similarly.
4. Rewrite `cse_global_load`, `cse_param_add`, `globalsym_cse`.
5. **Do not** rewrite `sl_forward`'s StoreEntry table — it has complex alias semantics that don't fit a generic key-value hash.

**Files:** New `ir/opt_hash.h` (~40 lines), new `ir/opt_hash.c` (~100 lines), modified CSE passes in `ir/opt.c` (net ~200 lines removed), `Makefile`.

**Verify:** `make cross && make test -j16` after each CSE pass conversion.

**Est. time:** 3-4 hours.

---

#### Phase 4.8: Pipeline Driver Update (`tccgen.c`)

**Goal:** Replace individual pass calls with engine runs where applicable.

**Changes to `tccgen.c` optimization pipeline (~25227-26230):**

**Change 1: Iterative loop (lines 25242-25350)**
Branch folding passes inside the `do { } while (changes)` loop:
```c
/* Before (lines 25277-25291) */
changes += tcc_ir_opt_branch_folding(ir);
changes += tcc_ir_opt_stack_addr_nonnull_fold(ir);
changes += tcc_ir_opt_setif_branch_fuse(ir);
changes += tcc_ir_opt_stack_bool_diamond(ir);

/* After */
{
    IROptAnalysis a;
    ir_opt_analysis_init(&a, ir);
    changes += ir_opt_run_peephole(ir, &a, branch_rules, n_branch_rules);
    ir_opt_analysis_free(&a);
}
changes += tcc_ir_opt_stack_bool_diamond(ir);  /* stays hand-written (complex CFG pattern) */
```

**Change 2: Fusion passes (lines 25446-25478)**
```c
/* Before */
tcc_ir_opt_fusion_pass(ir, do_mla, do_indexed);
tcc_ir_opt_deref_indexed_fusion(ir);
tcc_ir_opt_disp_fusion(ir);
tcc_ir_opt_add_deref_fold(ir);
tcc_ir_opt_lea_fold(ir);
tcc_ir_opt_postinc_fusion(ir);

/* After */
{
    IROptAnalysis a;
    ir_opt_analysis_init(&a, ir);
    ir_opt_run_peephole(ir, &a, fusion_rules, n_fusion_rules);
    ir_opt_analysis_free(&a);
}
tcc_ir_opt_add_deref_fold(ir);  /* stays hand-written (inserts instructions) */
```

**Change 3: Boolean passes (lines 25480-25484)**
```c
/* Before */
tcc_ir_opt_bool_pass(ir, do_idempotent, do_cse);
tcc_ir_opt_bool_simplify(ir);

/* After */
{
    IROptAnalysis a;
    ir_opt_analysis_init(&a, ir);
    ir_opt_run_peephole(ir, &a, bool_rules, n_bool_rules);
    ir_opt_analysis_free(&a);
}
tcc_ir_opt_cse_bool(ir);  /* stays hand-written (hash-table CSE) */
```

**Change 4: SL-FWD inner loop (lines 25535-25589)**
The iterative SL-FWD loop calls branch_folding, setif_branch_fuse, stack_addr_nonnull_fold, etc. Replace those 3 calls with a single `ir_opt_run_peephole(ir, &a, branch_rules, ...)` per iteration.

**Important: analysis lifetime scoping.** Each engine run creates and destroys an `IROptAnalysis`. Within the iterative loop, a fresh analysis is needed each iteration because `compact_nops` and `dce` change the instruction array between iterations.

**Files:** Modified `tccgen.c` (~40 lines changed), possibly new `ir/opt_rules.h` (declares rule arrays as extern).

**Verify:** `make cross && make test -j16 && make test-all`. This is the highest-risk step — run the full test suite including GCC torture tests.

**Est. time:** 2-3 hours.

---

### Passes That Stay Hand-Written

After Phases 4.4-4.8, these passes remain unconverted (using shared infrastructure but not the rule engine):

| Pass | Reason |
|------|--------|
| `dce` | Worklist-based CFG reachability — not a forward scan |
| `compact_nops` | Index remapping — sui generis |
| `const_prop` | 3-phase with 1100 lines, complex algebraic simplification |
| `const_prop_tmp` | Forward scan with generation-tagged block state |
| `const_var_prop` | 3-phase: collect VarInfo, propagate, dead code cleanup |
| `value_tracking` | Tracks constants through arithmetic chains, fold comparisons |
| `dse` | 4+ sub-phases: orphaned params, cascading worklist, dead VAR, dead StackLoc |
| `dead_var_store_elim` | 2-phase with bitsets |
| `dead_addrvar_elim` | 4-5 phases with LEA map propagation and fixed-point |
| `redundant_var_assign` | State machine with pending-write tracking |
| `redundant_init_elim` | Must-kill analysis |
| `copy_prop` | Forward scan with generation-tagged copy chains |
| `var_tmp_fwd` / `var_to_tmp` | BB-scoped forwarding with special lval handling |
| `cse_arith` | Hash table with commutative matching + invalidation-on-write |
| `cse_bool` | Hash table CSE (uses shared `IROptHashTable` from Phase 4.7) |
| `cse_global_load` | Hash table CSE |
| `globalsym_cse` | Frequency-based hoisting with instruction insertion |
| `cse_param_add` | Hash table CSE |
| `cse_global` | Multi-BB CSE with dominator trees |
| `sl_forward` | 2000-line stateful forward with LEA maps, alias tracking, generation counters |
| `entry_store_prop` | Cross-BB forwarding with custom dominance |
| `store_redundant` | Forward scan with hash-based store tracking |
| `vrp` | Dataflow with range lattice |
| `nonneg_branch_fold` | Uses merge bitmap, value range analysis |
| `float_branch_fold` | Uses merge bitmap, fall-through path analysis |
| `float_narrowing` | Pattern match on call targets |
| `stack_bool_diamond` | Complex 4-instruction CFG diamond pattern |
| `strength_reduction` | MUL→shift/add expansion |
| `stack_addr_cse` | Pattern match on LEA sequences |
| `postinc_assign_fold` | lval ASSIGN folding |
| `select` | If-then-else diamond → conditional select |
| `const_string_calls` | Fold strlen/strcmp/etc. on known strings |
| `const_call_replace` | Interprocedural constant return folding |
| All loop passes | CFG-restructuring (loop detection, IV analysis, unroll, rotate) |
| `add_deref_fold` | Only fusion pass that inserts instructions |

These passes use:
- `ir_opt_require_du(a)` from the analysis cache instead of calling `ir_opt_du_build` directly
- `ir_xform_nop`, `ir_xform_replace_with_assign`, `ir_xform_same_block` from transform primitives
- `IROptHashTable` from the generic hash table (CSE passes)

---

### What Changes for Adding a New Optimization

**Adding a new peephole rule (fusion, branch fold, boolean):**
Write a `match` + `transform` function pair (~50-100 lines total). Set `trigger_op` and flags. Add to the appropriate rule array in `opt_rules_*.c`. No iteration boilerplate, no DU build, no BB-boundary handling. Run `make test`.

**Adding a new CSE pass:**
Use `IROptHashTable` from `opt_hash.h`. Write the hash/equality/boundary functions. The pass body handles its own iteration but the hash table lifecycle is generic. ~100-150 lines instead of ~250.

**Adding a complex dataflow pass:**
Write a hand-written pass function. Use `ir_opt_require_du(a)` and `ir_opt_require_pred(a)` from the analysis cache. Use `ir_xform_*` primitives. ~200-400 lines, same as today but with less boilerplate.

---

### Estimated Effort

| Phase | What | Est. Time |
|-------|------|-----------|
| 4.1 | Analysis cache (`opt_analysis.c`) | 2-3 hours |
| 4.2 | Transform primitives (`opt_xform.c`) | 1 hour |
| 4.3 | Peephole engine (`opt_engine.c`) | 2 hours |
| 4.4 | Fusion rules (`opt_rules_fusion.c`) | 4-6 hours |
| 4.5 | Branch folding rules (`opt_rules_branch.c`) | 3-4 hours |
| 4.6 | Boolean rules (`opt_rules_bool.c`) | 1-2 hours |
| 4.7 | Generic hash table (`opt_hash.c`) | 3-4 hours |
| 4.8 | Pipeline driver update (`tccgen.c`) | 2-3 hours |
| **Total Phase 4** | | **18-25 hours** |

### Risks

- **Performance regression from function pointer dispatch.** The engine calls `match()` via function pointer for each (instruction, rule) pair. With 6 fusion rules and ~22K instructions, that's up to 132K indirect calls per engine run. Mitigation: `trigger_op` filtering skips ~90% of rules per instruction (most opcodes only match 1-2 rules). Benchmark after Phase 4.4 — if overhead >5%, consider a switch-based dispatch instead of function pointers.
- **Ordering changes.** Batching 6 fusion passes into 1 forward pass means MLA fusion now runs at the same instruction as indexed fusion (instead of MLA finishing the entire IR first). This changes which patterns fire first when multiple rules match the same instruction. Mitigation: rules are tried in array order (MLA before indexed), and first-match-wins means only one rule fires per instruction. Test after each rule addition.
- **DU table invalidation.** Peephole transforms that change opcodes (MUL→MLA, LOAD→LOAD_INDEXED) invalidate the DU table because the set of defined/used vregs changes. Mitigation: the engine does NOT rebuild DU mid-pass — it relies on the fact that NOP'd instructions are skipped and opcode changes preserve the vreg layout. Verify this assumption per-rule. If a rule changes which vregs are defined (not just which opcode), it must set a flag and the engine rebuilds DU before continuing.
- **add_deref_fold inserts instructions.** This is the only fusion pass that calls `insert_instr_at`, which shifts all subsequent instructions and patches jump targets. The peephole engine cannot handle this because it invalidates instruction indices mid-loop. Solution: keep `add_deref_fold` as a hand-written pass, called after the engine run.
- **Stack_addr_nonnull_fold complexity.** This 420-line pass traces multi-level def chains to prove a value is a non-NULL stack address. As a peephole rule, the match function alone would be ~120 lines. If the match function becomes too complex to maintain, revert to a hand-written pass.
