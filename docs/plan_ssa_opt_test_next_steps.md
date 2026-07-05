# Next Steps for SSA Optimizer Unit Tests

## Current Status
- **Phase 0 (Substrate)**: ✅ Complete
  - `test_ssa_build.c`: 6 tests passing (ctx lifecycle, CFG, SSA, vinfo, use-def)
  - Hand-built fixture layer (`ssa_build.h`) working
  - UT11 binary links real `ir/opt/ssa_opt*.c` files

- **Phase 2 (Leaf Passes)**: 🟡 Partially Complete
  - **Passing**: 12/16 suites (usedef, strength, narrow, reassoc, cmp_eq, fold, gvn, branch, load_cse, sccp, dead_loop, metamorphic)
  - **Failing**: 4/16 suites
    - `ssa_opt_phi`: Segfault (trivial phi elimination)
    - `ssa_opt_dce`: Segfault (dead code elimination)
    - `ssa_opt_cprop`: Test failure (`test_cprop_unknown_const` expects no change)

## Immediate Fixes Required

### 1. Fix `test_ssa_opt_phi.c` Segfault
**Problem**: `ssa_opt_phi_simplify()` crashes when called on hand-built fixture
**Likely Cause**: Missing initialization in `ssa_ctx_rebuild()` or `tcc_ir_ssa_opt_init()`
**Action**: 
- Add debug output to trace where it crashes
- Check if `ctx->ssa` and `ctx->cfg` are properly initialized
- Verify `ssa_opt_build_chains()` completes successfully

### 2. Fix `test_ssa_opt_dce.c` Segfault
**Problem**: `ssa_opt_dce()` crashes when called on hand-built fixture
**Likely Cause**: Similar to phi - missing initialization or invalid state
**Action**:
- Add debug output to trace where it crashes
- Check if DCE pass expects additional state beyond what's built

### 3. Fix `test_ssa_opt_cprop.c` Test Failure
**Problem**: `test_cprop_unknown_const` expects `changed == 0` but gets `changed == 1`
**Likely Cause**: Test expectation wrong or constant propagation is working differently than expected
**Action**:
- Add debug output to see what changed
- Verify test fixture matches expected behavior
- Update test expectation if propagation is correct

## Then: Phase 3-6 Tests

### Phase 3: Pass Integration Tests (3 UTs)
- `test_ssa_opt_pipeline_order`: Verify pass execution order
- `test_ssa_opt_pipeline_all_enabled`: Run full pipeline with all passes
- `test_ssa_opt_pipeline_one_disabled`: Run pipeline with one pass disabled

### Phase 4: Address Resolver Tests (5 UTs)
- `test_resolve_lea_stackloc_simple`: Basic LEA → stack location
- `test_resolve_lea_stackloc_chain`: Chained LEA assignments
- `test_resolve_lea_stackloc_unresolved`: Unresolvable LEA chain
- `test_resolve_temp_to_base_off_var`: Temp → base+off var
- `test_resolve_indirect_stack_offset`: Indirect stack offset

### Phase 5: Golden IR Construction (Layer B) (15 UTs)
- Build real SSA state via `tcc_ir_ssa_construct` + `tcc_ir_ssa_rename`
- Test passes on genuine SSA with phis, VAR promotions
- Covers: phi simplification, strength reduction, narrowing, etc. on real IR

### Phase 6: Metamorphic Fuzz (1 UT)
- `test_metamorphic_ssa_random_snippet`: Already written, passes
- Random snippet generation + pass application
- Verify no crashes, reasonable transformations

## Implementation Order
1. Fix the 3 failing tests (phi, dce, cprop)
2. Write Phase 3 integration tests
3. Write Phase 4 address resolver tests
4. Write Phase 5 golden IR tests
5. Verify all 53 UTs pass

## Notes
- All tests use hand-built fixtures (Layer A) for Phase 2-4
- Phase 5 requires Layer B (real SSA construction)
- Keep tests isolated - each suite tests one pass/family
- Use `UT_ASSERT`, `UT_ASSERT_EQ` from ut.h
- Follow existing patterns in `test_ssa_opt_arm.c`
