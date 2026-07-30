# Optimization Flags Reference

All optimization flags are `unsigned char` fields in `TCCState`
(`tcc.h` lines ~1027-1052). They gate individual passes and are checked
at runtime via the `FLAG()` macro in `opt_pipeline.c`.

## Command-Line Flags

| Flag | CLI | Default | Phase | Description |
|------|-----|---------|-------|-------------|
| `opt_dce` | `-fdce` / `-fno-dce` | O1+ | Legacy + SSA | Dead code elimination |
| `opt_const_prop` | `-fconst-prop` / `-fno-const-prop` | O1+ | Legacy + SSA | Constant propagation |
| `opt_copy_prop` | `-fcopy-prop` / `-fno-copy-prop` | O1+ | Legacy | Copy propagation |
| `opt_cse` | `-fcse` / `-fno-cse` | O1+ | Legacy | Common subexpression elimination |
| `opt_bool_idempotent` | `-fbool-idempotent` / `-fno-bool-idempotent` | O1+ | Legacy | Boolean idempotent simplification |
| `opt_bool_simplify` | `-fbool-simplify` / `-fno-bool-simplify` | O1+ | Legacy | Boolean expression simplification |
| `opt_store_load_fwd` | `-fstore-load-fwd` / `-fno-store-load-fwd` | O1+ | Legacy | Store-load forwarding |
| `opt_redundant_store` | `-fredundant-store-elim` / `-fno-redundant-store-elim` | O1+ | Legacy + SSA | Redundant store elimination |
| `opt_dead_store` | `-fdead-store-elim` / `-fno-dead-store-elim` | O1+ | Legacy | Dead store elimination |
| `opt_fp_offset_cache` | `-ffp-offset-cache` / `-fno-fp-offset-cache` | O1+ | Legacy | Frame pointer offset caching |
| `opt_indexed_memory` | `-findexed-memory` / `-fno-indexed-memory` | O1+ | Legacy | Indexed load/store fusion |
| `opt_disp_fusion` | `-fdisp-fusion` / `-fno-disp-fusion` | O1+ | Legacy | ADD+LOAD/STORE → displacement addressing |
| `opt_lea_fold` | `-flea-fold` / `-fno-lea-fold` | O1+ | Legacy | LEA Addr[StackLoc]+deref → direct stack slot |
| `opt_mla_fusion` | `-fmla-fusion` / `-fno-mla-fusion` | O1+ | Legacy | Multiply-accumulate fusion |
| `opt_stack_addr_cse` | `-fstack-addr-cse` / `-fno-stack-addr-cse` | O1+ | Legacy | Stack address CSE |
| `opt_licm` | `-flicm` / `-fno-licm` | O2 | Legacy + Flat-IR | Loop-invariant code motion |
| `opt_strength_red` | `-fstrength-reduce` / `-fno-strength-reduce` | O1+ | Legacy | Strength reduction for multiply |
| `opt_iv_strength_red` | `-fiv-strength-red` / `-fno-iv-strength-red` | O1+ | Flat-IR | IV strength reduction for array access |
| `opt_loop_unroll` | `-floop-unroll` / `-fno-loop-unroll` | O2 | Flat-IR | Full unroll small constant-trip loops |
| `opt_reroll` | `-freroll-blocks` / `-fno-reroll-blocks` | O2 | Flat-IR | Re-roll identical consecutive blocks |
| `opt_vrp` | `-fvrp` / `-fno-vrp` | O1+ | Legacy + SSA | Value range propagation |
| `opt_float_narrow` | `-ffloat-narrow` / `-fno-float-narrow` | O1+ | Legacy | Narrow double→float when safe |
| `opt_jump_threading` | `-fjump-threading` / `-fno-jump-threading` | O1+ | Legacy + Flat-IR | Jump threading optimization |
| `opt_inline_functions` | `-finline-functions` / `-fno-inline-functions` | O2 | Frontend | Auto-inline small functions |
| `opt_inline_small` | `-finline-small-functions` / `-fno-inline-small-functions` | O1+ | Frontend | Auto-inline tiny functions |
| `opt_ipc` | `-fipc` / `-fno-ipc` | O2 | Frontend | Interprocedural constant propagation |

## Environment Variable Disable

Any pass can be disabled at runtime via:
```bash
TCC_DISABLE_PASS=<pass_name> tcc ...
```

Examples:
```bash
TCC_DISABLE_PASS=ssa:branch     # Disable SSA branch folding
TCC_DISABLE_PASS=known_bits     # Disable known-bits analysis
TCC_DISABLE_PASS=kb_cascade     # Disable kb_cascade fixpoint
TCC_DISABLE_PASS=entry_store    # Disable entry-store cleanup
```

Pass names match the `"name"` field in the IROptPass / SSA_RUN macros.

## Optimization Level Presets

### O0
```
cleanup: dce
```
Only DCE for correctness. No propagation, no fusion.

### O1
```
propagation: ~30 passes (const_prop, known_bits, vrp, etc.)
late_cleanup: ~20 passes (dead_store, branch_cleanup, etc.)
```

### Os
```
propagation: same as O1
memory: sl_forward + const_cascade + branch cleanup
late_cleanup: same as O1
(fusion skipped — keeps code size smaller)
```

### O2
```
propagation: same as O1
memory: same as Os
fusion: MLA, indexed memory, disp fusion, bool simplify
late_cleanup: same as O1
```

## Gating Logic

### flag_offset Mechanism
Each `IROptPass` has a `flag_offset` field set via `FLAG(opt_xxx)`:
```c
#define FLAG(f) (uint16_t)offsetof(TCCState, f)
```
The pipeline checks:
```c
if (pass->flag_offset && !*((unsigned char *)tcc_state + pass->flag_offset))
    continue;
```

### Trigger-Based Groups
Groups with `trigger_idx >= 0` use the trigger pass as a go/no-go gate:
- If trigger returns 0 changes, the group exits immediately
- The trigger runs first, before other passes in the group
- Examples: `sl_forward` (memory group), `kb_cascade` (memory group)

### Fixpoint Termination
- Groups without triggers: terminate when a round produces 0 changes
- Groups with triggers: terminate when trigger returns 0 OR max_iterations hit
- Cascade wrappers: embed internal fixpoint loops (e.g. kb_cascade runs
  known_bits → const_prop → branch_fold → DCE → elim_fallthru → sl_forward
  up to 8 times internally)

## Pass Dependency Graph

```
                    ┌─────────────┐
                    │  DCE        │◄────────────────────────────────────┐
                    └──────┬──────┘                                     │
                           │ requires DU                                │
                    ┌──────▼──────┐                                     │
                    │ const_prop  │◄────────────────────────────────────┤
                    └──────┬──────┘                                     │
                           │ requires DU                                │
                    ┌──────▼──────┐                                     │
                    │ known_bits  │                                     │
                    └──────┬──────┘                                     │
                           │ invalidates DU                             │
                    ┌──────▼──────┐                                     │
                    │  vrp        │                                     │
                    └──────┬──────┘                                     │
                           │ invalidates ALL                            │
                    ┌──────▼──────┐                                     │
                    │ sl_forward  │◄── trigger for memory group         │
                    └──────┬──────┘                                     │
                           │ invalidates ALL                            │
                    ┌──────▼──────┐                                     │
                    │ fusion      │                                     │
                    └─────────────┘                                     │
                                                                        │
                 ┌──────────────────────────────────────────────────────┘
                 │
    ┌────────────▼────────────┐
    │    late_cleanup         │
    │  branch_cleanup (x2)    │
    │  dead_store passes      │
    │  redundant_store passes │
    └─────────────────────────┘
```

## Legacy vs SSA Pass Flag Mapping

| Legacy Flag | SSA Equivalent | Notes |
|-------------|---------------|-------|
| `opt_const_prop` | `ssa:cprop`, `ssa:sccp`, `ssa:var_const_fold` | SSA cprop runs unconditionally at -O1+ |
| `opt_dce` | `ssa:dce` | SSA DCE runs at all -O levels |
| `opt_store_load_fwd` | `ssa:load_cse` | SSA load_cse uses use-def chains, not flat IR |
| `opt_jump_threading` | `ssa:branch` | SSA branch folding replaces flat jump threading |
| `opt_vrp` | `ssa:cmp_eq_prop` | SSA cmp_eq_prop is the SSA equivalent |
| `opt_indexed_memory` | ARM gen: `arm_fuse_shl_add_to_load_indexed` | Target-specific |
| `opt_disp_fusion` | Legacy fusion group | Not ported to SSA (backend handles) |
| `opt_mla_fusion` | ARM gen: `arm_fuse_mul_add_to_mla` | Target-specific |
