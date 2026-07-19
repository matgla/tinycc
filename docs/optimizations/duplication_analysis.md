# Duplication Analysis: Legacy (Pre-SSA) vs SSA Optimizations

This document catalogs where the same or similar optimization goals are
pursued in both the legacy flat-IR phase and the SSA phase, and explains
why the duplication exists.

## Methodology

- Legacy passes: defined in `source/opt/engine/pipeline_table.c`, run on flat IR before
  CFG/SSA construction.
- SSA passes: driver in `source/opt/ssa/engine/`, implementations under
  `source/opt/ssa/{scalar,cfg,memory,string,dce,loop}/`, run after SSA
  construction on SSA-form IR with use-def chains.
- Flat-IR loop transforms: defined inline in `ir/regalloc.c` before SSA
  construction, use engines from `source/opt/ssa/loop/` and `source/opt/flat/loop/`.

## True Duplicates (Same Goal, Different Implementation)

These pairs pursue the same optimization but operate on different IR forms.
The SSA version is strictly more powerful (SSA form enables better analysis)
but runs later in the pipeline.

### 1. DCE (Dead Code Elimination)

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_dce()` in `source/opt/flat/dce/dce.c` | `ssa_opt_dce()` in `source/opt/ssa/dce/dce.c` |
| Operates on flat IR, uses DU chains (live analysis) | Operates on SSA IR, uses vreg def_count + use lists |
| Gated by `opt_dce` flag | Always runs at -O1+ (no flag gate) |
| Present in: propagation, fusion, late_cleanup, kb_cascade, const_cascade, entry_store | Present in: main SSA driver, guard_collapse, cfg_cleanup |

**Verdict**: Genuine duplication. The SSA DCE is more precise (SSA-form, single-def
temps). The legacy DCE is needed before SSA to clean up dead code that would
confuse the SSA construction (e.g. dead phi operands). **Not removable** — they
serve different pipeline positions.

### 2. Constant Propagation

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_const_prop()` cascades | `ssa_opt_cprop()` + `ssa_opt_sccp()` + `ssa_opt_var_const_fold()` |
| Flat-IR forward scan, tracks known constants per vreg | SSA-form lattice propagation through dominator tree |
| Gated by `opt_const_prop` | Always runs at -O1+ |
| ~30 iterations in const_cascade fixpoint | 2 passes in main driver, converges in ≤5 iterations |

**Verdict**: Same goal, different algorithms. Legacy uses flat-IR forward
scanning; SSA uses lattice-based SCCP + copy propagation. The SSA version
is more powerful (handles conditional paths via SCCP). Legacy still needed
to reduce the IR before SSA construction.

### 3. Store-Load Forwarding / Load CSE

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_sl_forward()` | `ssa_opt_load_cse()` |
| Flat IR, tracks stores per stack slot, forwards loads | SSA-form, uses use-def chains + address resolution |
| Gated by `opt_store_load_fwd` | Always runs at -O1+ |
| Trigger for the memory group | Part of main SSA driver |

**Verdict**: Same goal. Legacy does flat store-load forwarding (can forward
across blocks since it's flat). SSA does load CSE using address canonicalization
(`ssa_opt_resolve_lea_stackloc`, `ssa_opt_resolve_temp_to_base_off`). The SSA
version is more precise (SSA temps can't alias) but the legacy version catches
patterns the SSA version might miss (e.g. global stores).

### 4. Branch Folding / Jump Threading

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_eliminate_fallthrough()` + `tcc_ir_opt_jump_threading()` | `ssa_opt_branch()` |
| Flat IR, threads jump chains, removes fall-through | SSA-form, folds dead branches using dominance |
| Gated by `opt_jump_threading` | Always runs at -O1+ |
| Runs in: memory group (elim_fallthru), late_cleanup (branch_cleanup), cfg_cleanup | Runs in: main SSA driver |

**Verdict**: Same goal. Legacy handles flat-IR jump chains; SSA handles
SSA-form branch folding. Both are needed — legacy cleans up before CFG
construction, SSA refines after.

### 5. VRP (Value Range Propagation)

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_vrp()` in `ir/opt_branch.c` | `ssa_opt_cmp_eq_prop()` |
| Flat IR, folds branches based on known value ranges | SSA-form, pushes equality facts down dominance tree |
| Gated by `opt_vrp` | Always runs at -O1+ |
| Runs in: propagation group | Runs in: main SSA driver |

**Verdict**: Same goal. Legacy VRP folds branches in flat IR; SSA cmp_eq_prop
does the same but on SSA form with dominance info. The SSA version is more
precise (handles nested conditionals better).

### 6. Known Bits Analysis

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_known_bits()` in `ir/opt_knownbits.c` | No direct equivalent |
| Flat IR bitmask propagation | — |
| Gated by `opt_const_prop` | Runs in: kb_cascade fixpoint |

**Verdict**: No SSA equivalent. Known bits is a flat-IR-specific optimization
that doesn't need SSA form. It feeds into the kb_cascade fixpoint.

### 7. Dead Store Elimination

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_dse()`, `tcc_ir_opt_dead_var_store_elim()`, `tcc_ir_opt_dead_lea_store_elim()`, etc. | `ssa_opt_dce()` (covers all dead temps) |
| Flat IR, specialized passes for different dead-store patterns | SSA-form, single DCE pass handles all dead temps |
| Gated by `opt_dead_store` | Always runs at -O1+ |
| ~10 specialized dead-store passes | One general DCE pass |

**Verdict**: Genuine duplication but the SSA version subsumes the legacy
dead-store passes. The legacy passes run before SSA to clean up the IR
for SSA construction. The SSA DCE handles everything after SSA.

### 8. Copy Propagation

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_const_prop()` includes copy prop | `ssa_opt_cprop()` |
| Gated by `opt_copy_prop` / `opt_const_prop` | Always runs at -O1+ |

**Verdict**: Same goal. Legacy copy prop is part of the const_prop cascade.
SSA copy prop is a dedicated pass.

### 9. Addition Reassociation

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_add_reassoc()` | `ssa_opt_reassoc()` |
| Gated by `opt_const_prop` | Always runs at -O1+ |

**Verdict**: Same goal. SSA version is more powerful (SSA form enables
better reassociation). Legacy version runs first to preprocess.

### 10. Folding / Expression Simplification

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_constfold.c` (many patterns) | `ssa_opt_fold()` |
| Gated by various flags | Always runs at -O1+ |

**Verdict**: Same goal. SSA fold is the SSA-form equivalent of the legacy
constant folding.

### 11. Strength Reduction

| Legacy | SSA |
|--------|-----|
| N/A (no dedicated legacy strength reduction) | `ssa_opt_strength()` |
| — | Always runs at -O1+ |

**Verdict**: SSA-only. No legacy equivalent.

### 12. Narrowing

| Legacy | SSA |
|--------|-----|
| `tcc_ir_opt_float_narrowing()` (double→float) | `ssa_opt_narrow()` (general type narrowing) |
| Gated by `opt_float_narrow` | Always runs at -O1+ |

**Verdict**: Partial overlap. Legacy handles float narrowing; SSA handles
general type narrowing (including integer).

### 13. Global Value Numbering

| Legacy | SSA |
|--------|-----|
| N/A | `ssa_opt_gvn()` |
| — | Always runs at -O1+ |

**Verdict**: SSA-only. GVN requires SSA form.

### 14. Phi Simplification

| Legacy | SSA |
|--------|-----|
| N/A | `ssa_opt_phi_simplify()` |
| — | Always runs at -O1+ |

**Verdict**: SSA-only. Phis don't exist in flat IR.

## Flat-IR Loop Transforms vs SSA Loop Passes

These run on flat IR before SSA construction but share engines with
the SSA-era loop transform documentation:

| Legacy (tccgen-era) | SSA-Era Replacement | Status |
|---------------------|---------------------|--------|
| `tcc_ir_opt_loop_rotate` (Phase 4a) | `ssa_opt_loop_rotate()` | Ported, runs in regalloc.c before SSA |
| `tcc_ir_opt_loop_dead_first_iter` (Phase 4b) | `ssa_opt_first_iter_exit()` | Ported |
| `tcc_ir_opt_loop_ptr_iv_exit_subst` (Phase 4c) | `ssa_opt_ptr_iv_exit_subst()` | Ported |
| `tcc_ir_opt_loop_const_sim` (Phase 4d) | `ssa_opt_loop_const_sim()` | Ported |
| `tcc_ir_opt_loop_unroll` (Phase 5a) | `ssa_opt_loop_unroll()` | Ported |
| `tcc_ir_opt_loop_iv_strength_reduction` (Phase 6) | `ssa_opt_iv_strength_reduction()` | Ported |
| `tcc_ir_opt_loop_decrement_to_zero` | `ssa_opt_decrement_to_zero()` | Ported |
| `tcc_ir_opt_loop_reroll` | `ssa_opt_reroll()` | Ported |
| `tcc_ir_opt_loop_licm` | `ssa_opt_licm()` | Ported |
| `tcc_ir_opt_loop_guard_elim` | `ssa_opt_guard_collapse()` (SSA phase) | Moved to SSA phase |
| `tcc_ir_opt_loop_bound_remat` | Retired | — |
| `tcc_ir_opt_loop_backedge_phi_hoist` | Retired | — |
| `tcc_ir_opt_loop_postinc_fusion` | Retired | — |
| `tcc_ir_opt_loop_ssa_replacement` | Retired | — |

**Verdict**: The loop transforms were migrated from tccgen.c to the
regalloc.c pipeline. They operate on flat IR (before CFG/SSA) but use
the same engine code. No duplication — just relocation.

## Overlap Summary

| Category | Legacy Count | SSA Count | Overlap |
|----------|-------------|-----------|---------|
| DCE | 1 (+ ~10 dead-store specials) | 1 | Same goal, different IR |
| Const Prop | 1 (+ cascade) | 3 (cprop, sccp, var_const_fold) | Same goal, different algorithms |
| Store-Load Fwd / Load CSE | 1 (+ global) | 1 | Same goal |
| Branch Folding | 2 (elim_fallthru, jump_thread) | 1 | Same goal |
| VRP | 1 | 1 (cmp_eq_prop) | Same goal |
| Known Bits | 1 | 0 | Legacy-only |
| Dead Store | ~10 specials | 1 (DCE) | SSA subsumes legacy |
| Copy Prop | 1 | 1 | Same goal |
| Reassociation | 1 | 1 | Same goal |
| Folding | 1 (many patterns) | 1 | Same goal |
| Strength Reduction | 0 | 1 | SSA-only |
| Narrowing | 1 (float only) | 1 (general) | Partial overlap |
| GVN | 0 | 1 | SSA-only |
| Phi Simplify | 0 | 1 | SSA-only |
| Loop Transforms | ~14 (tccgen-era) | ~10 (ported) | Relocated, not duplicated |
| Fusion (MLA, indexed, disp) | ~7 | ~8 (ARM-specific) | Same goal, target-specific |

## Why Duplication Exists

1. **Different IR forms**: Flat IR vs SSA-form require different algorithms.
   You can't run SSA copy propagation on flat IR, and you can't run flat-IR
   store-load forwarding on SSA-form (SSA temps don't alias).

2. **Pipeline position**: Legacy passes clean up the IR *before* SSA
   construction. Without them, the SSA construction would see more dead
   code, more branches, and more complex patterns.

3. **Convergence**: The legacy cascade wrappers (kb_cascade, const_prop_cascade)
   ensure cross-dependencies converge before SSA sees the IR.

4. **Correctness gates**: Legacy passes are flag-gated for bisection. If a
   bug appears at -O1, you can disable individual legacy passes to find
   the culprit. SSA passes run unconditionally at -O1+ because they've
   been validated as a unit.

5. **Performance**: Legacy passes operate on simpler IR (no phi nodes,
   no dominance tree), so they're faster per-pass even if less powerful.

## Recommendations

1. **No removals**: The duplication is intentional and necessary. Removing
   legacy passes would break SSA construction and degrade optimization
   quality at lower levels.

2. **Consolidation opportunity**: The ~10 legacy dead-store passes could
   potentially be folded into a single "dead_store_cleanup" pass that
   runs before SSA, reducing code complexity.

3. **Flag unification**: The `opt_const_prop` flag gates ~15 legacy passes.
   Consider whether some of these should have independent flags for
   finer-grained control.

4. **Documentation**: This file should be updated when new passes are
   added to either phase.
