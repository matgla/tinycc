# SSA Optimization Loop Consolidation

**Status**: Analysis phase
**Created**: 2026-07-09
**Goal**: Reduce code and performance bloat by retiring the legacy flat-IR
optimization system and running only the new SSA-based passes.

---

## Current State

Two parallel optimization systems coexist in the build:

| System | Location | Lines | Role |
|--------|----------|-------|------|
| **Legacy flat-IR** | `ir/opt_*.c` (35 files) | ~57,800 | Runs first, on flat IR before CFG/SSA |
| **New SSA** | `ir/opt/ssa_opt_*.c` (15 files) | ~13,700 | Runs after CFG/SSA construction |

Both compile into the same binary. The pipeline is:

```
Flat IR loop transforms (legacy)
  → CFG + dominators + SSA construction
  → SSA optimization passes (new)
  → Phi resolution + register allocation
```

### Legacy Pipeline (O2)

```
entry_store_prop → propagation → memory → fusion → late_cleanup
```

- **propagation**: 30 passes, 10 iterations, `opt_const_prop` gated
- **memory**: 12 iterations, trigger-gated by `sl_forward`
- **fusion**: 1 iteration, instruction fusion
- **late_cleanup**: 20+ passes, dead-store heavy

### SSA Pipeline

```
var_const_fold → sccp → cprop → var_to_param_forward → fold → cprop →
var_imm_prop → load_cse → branch → cmp_eq_prop → reassoc → strength →
narrow → gvn → phi_simplify → dead_loop → dce → [target gens]
→ guard_collapse (fixpoint)
```

5 iterations max, then guard_collapse fixpoint.

---

## Problem: Duplication & Bloat

### Code Duplication

| Legacy Pass | SSA Equivalent | Legacy Lines | SSA Lines | Overlap |
|-------------|---------------|-------------|-----------|---------|
| `opt_constprop.c` | `ssa_opt_sccp.c` + `ssa_opt_cprop.c` | 7,676 | 4,061 | ~60% semantic overlap |
| `opt_dce.c` | `ssa_opt_dce.c` | 7,888 | 1,566 | ~70% overlap (SSA is leaner) |
| `opt_constfold.c` | `ssa_opt_fold.c` | 3,115 | 787 | ~50% overlap |
| `opt_branch.c` | `ssa_opt_branch.c` | 2,259 | 802 | ~40% overlap |
| `opt_memory.c` | *(no SSA equivalent)* | 11,727 | — | Store-load forwarding only |
| `opt_loop_utils.c` + `opt_loop_const_sim.c` + `opt_loop.c` + `opt_reroll.c` | `ssa_opt_loop.c` | 7,613 | 2,047 | ~50% shared engine code |
| `opt_fusion.c` + `opt_gens_*.c` | target gens (ARM) | ~10,000 | ~3,000 | Fusion only in legacy |
| `opt_knownbits.c` | *(no SSA equivalent)* | 1,706 | — | Known-bits analysis only |
| `opt_neg_chain.c` | *(no SSA equivalent)* | 449 | — | Negation chain CSE only |
| `opt_pack64.c` | *(no SSA equivalent)* | 1,238 | — | 64-bit packing only |
| `opt_promote.c` | *(no SSA equivalent)* | 2,152 | — | Type promotion only |
| `opt_jump_thread.c` | `ssa_opt_branch.c` | 334 | — | Subsumed |
| `opt_copyprop.c` | `ssa_opt_cprop.c` | 785 | — | Subsumed |
| `opt_dead_vla.c` | `late_cleanup` (legacy only) | 999 | — | VLA-specific, pre-SSA |
| `opt_dead_lea_store.c` | `late_cleanup` (legacy only) | 642 | — | LEA store dead code |
| `opt_alias.c` | *(shared utility)* | 127 | — | Alias analysis utility |
| `opt_du.c` | *(shared utility)* | 162 | — | Dominator utility |
| `opt_bitfield.c` | *(shared utility)* | 630 | — | Bitfield utility |
| `opt_cmp_fuse.c` | *(shared utility)* | 277 | — | Compare fusion utility |
| `opt_const_aggregate.c` | *(shared utility)* | 728 | — | Const aggregate utility |
| `opt_switch_data.c` | *(shared utility)* | 462 | — | Switch data utility |
| `opt_setif_or_taut.c` | *(shared utility)* | 362 | — | SETIF tautology utility |
| `opt_xform.c` | *(shared utility)* | 203 | — | Transform utility |
| `opt_utils.c` | *(shared utility)* | 1,524 | — | Utility functions |
| `opt_engine.c` + `opt_pipeline.c` | `ssa_opt.c` | 1,018 | 802 | Pipeline orchestration |

**Total legacy**: ~57,800 lines
**Total SSA**: ~13,700 lines
**Estimated retirable**: ~35,000 lines (passes with SSA equivalents or no SSA need)

### Performance Bloat

1. **Redundant pass execution**: The legacy pipeline runs `branch_cleanup` twice
   in `late_cleanup_passes`, `stack_nonnull`/`setif_fuse`/`setif_or_taut`/
   `var_tmp_fwd`/`dce` appear in both `propagation_passes` and `memory_passes`.
   The SSA pipeline also runs `cprop` twice per iteration.

2. **Cascade overhead**: Legacy cascades (`kb_cascade`, `const_prop_cascade`,
   `branch_cleanup_cascade`, `entry_store_cleanup`) each loop 4-8 times with
   multiple passes per iteration. SSA's `guard_collapse` is similarly nested
   but runs fewer passes per round.

3. **Dual-system cost**: Every function gets optimized twice — once on flat IR
   by the legacy system, then again on SSA by the new system. Many transformations
   the legacy system performs (const prop, DCE, branch folding) are redone by SSA
   passes, wasting cycles on already-collapsed IR.

4. **Memory pressure**: Legacy `opt_constprop.c` (7.7K lines) and `opt_memory.c`
   (11.7K lines) maintain large data structures for flat-IR analysis. The SSA
   equivalents are significantly more compact.

5. **Pass invalidation churn**: Legacy pipeline invalidates DU, merge bitmap,
   block starts, and loops between groups. SSA engine uses a simpler change
   counter with full rebuild on each iteration.

---

## Migration Map: Legacy → SSA

### Fully Migrated (safe to retire legacy)

| Legacy | SSA Equivalent | Notes |
|--------|---------------|-------|
| `opt_constprop.c` | `ssa_opt_sccp.c` + `ssa_opt_cprop.c` | SCCP subsumes const prop; cprop handles copies |
| `opt_dce.c` | `ssa_opt_dce.c` + `ssa_opt_dce_light.c` | SSA DCE is leaner, runs after all transforms |
| `opt_constfold.c` | `ssa_opt_fold.c` | Fold pass handles arithmetic/logic simplification |
| `opt_branch.c` | `ssa_opt_branch.c` | Branch folding, dead branch removal |
| `opt_copyprop.c` | `ssa_opt_cprop.c` | Copy propagation with use-def chains |
| `opt_jump_thread.c` | `ssa_opt_branch.c` | Subsumed by branch folding |
| Loop transforms (rotate, unroll, const_sim, reroll, etc.) | `ssa_opt_loop.c` | All ported, some reuse shared engines |

### Partially Migrated (need SSA equivalent before retire)

| Legacy | SSA Status | Gap |
|--------|-----------|-----|
| `opt_memory.c` (sl_forward, global_sl_fwd) | No SSA equivalent | Store-load forwarding is pre-SSA; needs SSA-aware version |
| `opt_knownbits.c` | No SSA equivalent | Known-bits analysis; could run as SSA generator or before SSA |
| `opt_neg_chain.c` | No SSA equivalent | Negation chain CSE; simple enough for SSA generator |
| `opt_pack64.c` | No SSA equivalent | 64-bit packing; backend-specific, could be target gen |
| `opt_promote.c` | No SSA equivalent | Type promotion; could be SSA generator |

### Utility Files (keep, not retired)

| File | Role |
|------|------|
| `opt_alias.c` / `opt_du.c` / `opt_utils.c` | Shared utilities used by both systems |
| `opt_engine.c` / `opt_pipeline.c` | Legacy pipeline orchestration |
| `opt_loop_utils.c` / `opt_loop_const_sim.c` | Shared loop engine code (used by both) |
| `opt_bitfield.c` / `opt_cmp_fuse.c` / `opt_const_aggregate.c` | Domain utilities |
| `opt_switch_data.c` / `opt_setif_or_taut.c` | Domain utilities |
| `opt_dead_vla.c` / `opt_dead_lea_store.c` | Pre-SSA dead code (no SSA equivalent needed if legacy goes) |

---

## Plan

### Phase 1: Audit & Test (Week 1)

1. **Map every legacy pass to its SSA equivalent** (or gap)
2. **Run fuzz sweeps** (`scripts/diff_olevels.py --seeds 0-5000`) to establish
   baseline divergence counts between O-levels
3. **Identify passes that have NO SSA equivalent** and decide: port, skip, or
   make target-specific

### Phase 2: Gate Legacy Pipeline (Week 2)

1. Add a compile-time flag to disable the legacy pipeline entirely
2. Run the SSA-only pipeline and measure:
   - Code size (binary size)
   - Optimization quality (fuzz divergence count)
   - Compilation speed
3. Fix any regressions by either:
   - Adding missing SSA equivalents
   - Moving necessary legacy passes to run before SSA (e.g., known_bits)

### Phase 3: Retire Legacy Code (Week 3-4)

1. Remove legacy pass files that have SSA equivalents
2. Remove cascade wrappers (`tcc_ir_opt_known_bits_cascade_ex`, etc.)
3. Remove `opt_pipeline.c` group definitions
4. Keep only shared utilities (`opt_utils.c`, `opt_alias.c`, `opt_du.c`, etc.)
5. Update `Makefile` to drop legacy object files
6. Run full fuzz sweep to confirm zero regressions

### Phase 4: Optimize SSA Pipeline (Week 5-6)

1. **Remove redundant passes within SSA pipeline**:
   - `cprop` runs twice per iteration — merge into single pass with fixpoint
   - `branch_cleanup` runs twice in legacy `late_cleanup` — not applicable to SSA
2. **Consolidate cascade wrappers**: Replace nested fixpoint loops with
   a single generic fixpoint driver
3. **Merge similar passes**: `stack_nonnull` + `setif_fuse` + `setif_or_taut`
   could be a single "branch simplification" pass
4. **Parallelize where possible**: SSA passes that don't depend on each other
   (e.g., `strength` and `narrow`) could run in parallel on different instruction ranges

### Phase 5: Benchmark & Document (Week 7)

1. Measure compilation time improvement
2. Measure code size improvement
3. Document the final pipeline structure
4. Update `docs/optimizations/ssa_passes.md`

---

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| Missing SSA equivalent for memory passes | Performance regression | Port sl_forward to SSA or keep as pre-SSA pass |
| Known-bits analysis not ported | Fold opportunities lost | Run as SSA generator or pre-SSA |
| Legacy pipeline has edge cases not covered by SSA | Miscompilations | Aggressive fuzz testing after each phase |
| Shared engine code (`opt_loop_utils.c`) used by both systems | Breakage during retire | Audit all callers before removing |

---

## Expected Outcomes

- **~35,000 lines removed** from legacy system
- **~40% reduction** in optimization compilation time (fewer passes, no dual-system)
- **Simpler pipeline**: single SSA-driven pipeline replaces 4-group legacy + SSA
- **Easier maintenance**: one optimization system instead of two

---

## Related Documents

- `docs/plan_legacy_flat_ir_ssa_retire.md` — High-level retirement plan
- `docs/plan_legacy_loop_*.md` — Per-pass migration plans
- `docs/optimizations/legacy_passes.md` — Legacy pipeline documentation
- `docs/optimizations/ssa_passes.md` — SSA pipeline documentation
- `docs/optimizations/duplication_analysis.md` — Detailed duplication map
