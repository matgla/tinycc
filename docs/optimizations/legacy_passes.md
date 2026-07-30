# Legacy Pre-SSA Flat-IR Optimizations

These passes operate on flat IR before SSA construction. They are organized
into **pass groups** defined in `source/opt/engine/pipeline_table.c`, each with a name,
pass list, max iterations, and optional trigger pass.

## Pipeline Structure

```
O0: [cleanup]
O1: [propagation] → [late_cleanup]
Os: [propagation] → [memory] → [late_cleanup]
O2: [propagation] → [memory] → [fusion] → [late_cleanup]
```

Each group iterates up to `max_iterations` times. A group terminates when
a round produces zero changes, or when the trigger pass returns 0.

## Pass Groups

### entry_store_prop (1 group, trigger-based, 3 iterations)

Entry-point store/load cleanup. Runs at all optimization levels.

| Pass | Flag | Description |
|------|------|-------------|
| `entry_store` | `opt_store_load_fwd` | Initial store-load forwarding at function entry |
| `esp_cleanup` | `opt_const_prop` | Cascade: const_prop → const_var_prop → stack_nonnull → redundant_loop_check → DCE → sl_forward → dead_var_store → compact. Cleans up entry-store patterns (e.g. struct init before first use) |

### propagation (1 group, 10 iterations)

Constant propagation and value analysis. The largest group — ~30 passes.

| Pass | Flag | Description |
|------|------|-------------|
| `uninit_ub` | `opt_dce` | UB-exploit: fold code after undefined-local reads as unreachable |
| `uninit_dom_ret` | `opt_dce` | Dead-code after UB that dominates a return |
| `dce` | `opt_dce` | Dead code elimination |
| `const_prop` | `opt_const_prop` | Constant propagation (full cascade: const_prop + const_prop_tmp + const_var_prop + value_tracking) |
| `const_var_prop` | `opt_const_prop` | Single-def constant-immediate VAR propagation |
| `global_init` | `opt_const_prop` | Global initializer propagation |
| `symref_prop` | `opt_const_prop` | Symbol reference constant propagation |
| `global_sl_fwd` | `opt_store_load_fwd` | Global store-load forwarding |
| `const_prop_tmp` | `opt_const_prop` | Temporary constant propagation |
| `const_agg_fold` | `opt_const_prop` | Fold deterministic RMW chains on non-escaping locals |
| `known_bits` | `opt_const_prop` | Known-bits analysis (bitmask propagation) |
| `neg_chain_cse` | `opt_const_prop` | Negation chain CSE |
| `add_reassoc` | `opt_const_prop` | Addition reassociation |
| `redundant_assign` | `opt_const_prop` | Redundant variable assignment elimination |
| `string_calls` | `opt_const_prop` | String function constant folding |
| `self_copy_elim` | `opt_const_prop` | Self-copy elimination |
| `value_tracking` | `opt_const_prop` | Value range tracking |
| `cmp_expr_fold` | `opt_const_prop` | Compare expression folding |
| `self_arith` | `opt_const_prop` | Self-arith fold (x + 0, x * 1, etc.) |
| `cmp_offset_fold` | `opt_const_prop` | Compare-constant-offset fold |
| `switch_collapse` | `opt_const_prop` | Switch table collapse |
| `stack_nonnull` | `opt_const_prop` | Stack address non-null fold |
| `setif_fuse` | `opt_const_prop` | SETIF + conditional branch fuse |
| `stack_bool` | `opt_const_prop` | Stack bool diamond fold |
| `setif_or_taut` | `opt_const_prop` | SETIF or-tautology fold |
| `var_tmp_fwd` | `opt_const_prop` | VAR-to-temporary forwarding |
| `float_branch` | `opt_vrp` | Float branch folding |
| `vrp` | `opt_vrp` | Value range propagation |
| `single_val_tmp` | `opt_const_prop` | Single-value temporary elimination |
| `float_narrow` | `opt_float_narrow` | Double→float narrowing |
| `deref_fwd` | `opt_const_prop` | Dereference forwarding |

### memory (1 group, trigger-based, 12 iterations)

Memory optimization with sl_forward as trigger.

| Pass | Flag | Description |
|------|------|-------------|
| `sl_forward` (trigger) | `opt_store_load_fwd` | Store-load forwarding + elim_fallthrough. If nothing forwarded, skip the rest of the group |
| `bf_insert_extract` | `opt_const_prop` | Bitfield insert+extract fold (post-forwarding) |
| `cmp_field_fuse` | `opt_const_prop` | Field-compare fusion |
| `const_cascade` | `opt_const_prop` | Full const_prop fixpoint cascade |
| `stack_nonnull` | `opt_const_prop` | Stack address non-null fold |
| `setif_fuse` | `opt_const_prop` | SETIF + conditional branch fuse |
| `stack_bool` | `opt_const_prop` | Stack bool diamond fold |
| `setif_or_taut` | `opt_const_prop` | SETIF or-tautology fold |
| `var_tmp_fwd` | `opt_const_prop` | VAR-to-temporary forwarding |
| `dce` | `opt_dce` | Dead code elimination |
| `elim_fallthru` | `opt_jump_threading` | Eliminate fall-through jumps |
| `kb_cascade` | `opt_const_prop` | Known-bits cascade: known_bits → const_prop → branch_fold → DCE → elim_fallthru → sl_forward fixpoint |

### fusion (1 group, 1 iteration)

Instruction fusion / code generation optimization.

| Pass | Flag | Description |
|------|------|-------------|
| `fusion_mla` | — | Multiply-accumulate fusion (always runs) |
| `deref_indexed` | `opt_indexed_memory` | Indexed memory deref fusion |
| `disp_fusion` | `opt_disp_fusion` | ADD+LOAD/STORE → displacement-addressed mem op |
| `dce` | `opt_dce` | Dead code elimination |
| `chain_fold` | `opt_disp_fusion` | Disp fusion chain fold |
| `pair_reorder` | `opt_disp_fusion` | Disp fusion pair reordering |
| `bool_simplify` | `opt_bool_idempotent` | Boolean idempotent simplification |

### late_cleanup (1 group, 2 iterations)

Post-propagation cleanup.

| Pass | Flag | Description |
|------|------|-------------|
| `branch_cleanup` | `opt_jump_threading` | Dead converging control flow cascade (elim_fallthru + DCE) |
| `dead_vla_struct` | `opt_dead_store` | Dead VLA struct elimination |
| `alloca_load_fwd` | `opt_dead_store` | Alloca load forwarding |
| `zero_vla` | `opt_dead_store` | Zero-size VLA elimination |
| `byte_store_merge` | `opt_redundant_store` | Byte store merge |
| `store_redundant` | `opt_redundant_store` | Redundant store elimination |
| `dse` | `opt_dead_store` | Dead store elimination |
| `dead_static_store` | `opt_dead_store` | Dead static store (TU-wide) |
| `dead_var_store` | `opt_dead_store` | Dead local variable store |
| `dead_addrvar` | `opt_dead_store` | Dead address-taken variable store |
| `dead_trail_addrvar` | `opt_dead_store` | Dead trailing address-taken store |
| `dead_alloca_vreg` | `opt_dead_store` | Dead alloca VREG elimination |
| `dead_local_slot` | `opt_dead_store` | Dead local slot elimination |
| `dead_lea_store` | `opt_dead_store` | Dead LEA store elimination |
| `dead_temp_local` | `opt_dead_store` | Dead temporary local elimination |
| `redundant_assign` | `opt_dead_store` | Redundant variable assignment |
| `inplace_arith` | `opt_redundant_store` | In-place arithmetic fold |
| `global_base_share` | `opt_indexed_memory` | Global base pointer sharing |
| `branch_cleanup` | `opt_jump_threading` | Second branch cleanup pass |
| `inf_loop_simpl` | `opt_dce` | Infinite loop simplification |
| `dead_pre_inf` | `opt_dce` | Dead code before infinite loop |
| `return_reuse` | `opt_const_prop` | Return constant reuse |

## Cascade Wrappers

These are inline fixpoint loops embedded in the pipeline:

### tcc_ir_opt_known_bits_cascade_ex
Iterates 8 times: known_bits → const_prop_tmp → gens_branch → DCE → elim_fallthrough → compact → sl_forward → global_sl_fwd. Ensures known-bits discovery cascades through the full memory optimization chain.

### tcc_ir_opt_const_prop_cascade_ex
Iterates 4 times: const_prop + const_prop_tmp + const_var_prop + value_tracking. Converges arbitrary-depth constant propagation.

### tcc_ir_opt_branch_cleanup_cascade_ex
Iterates 8 times: elim_fallthrough → compact → DCE. Collapses dead converging control flow.

### tcc_ir_opt_entry_store_cleanup_ex
Single-pass cascade: const_prop → const_var_prop → stack_nonnull → redundant_loop_check → DCE → compact → sl_forward → stack_nonnull → DCE → dead_var_store → compact. Cleans up entry-store patterns.

## Legacy Notes

- The `entry_store_group` is also exposed as a standalone `IRPassGroup` for
  external use (e.g. cfg_cleanup reruns).
- Some passes have compound implementations (`*_ex` suffix) that wrap the
  flat engine with the `IROptCtx` layer.
- The `trigger_idx` field on `IRPassGroup` designates one pass as the
  group's "go/no-go" gate. If the trigger returns 0 changes, the group
  exits early.
