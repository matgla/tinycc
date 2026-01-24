# IROperand Migration Plan: SValue → IROperand

## Summary

**Goal**: Remove SValues from IR storage and codegen; keep only in parser vstack.

**Current State**: Dual-pool system in place (`svalue_pool` + `iroperand_pool`), sync functions exist.

**Strategy**: Migrate one function at a time, starting from simplest, with writeback sync.

**Immediate Next Step**: Migrate `tcc_gen_machine_jump_op()` - simplest function (1 param, no writeback needed).

---

## Goal

Remove SValues from everything except the parser's vstack. Code generation and optimization passes should work exclusively with IROperands, eliminating the 56-byte SValue overhead in favor of 8-byte IROperands.

## Current State

### Dual-Pool Architecture (Already In Place)
- `svalue_pool[]` - stores full SValue structs (56 bytes each)
- `iroperand_pool[]` - stores IROperand structs (8 bytes each), parallel indices
- `pool_i64[]`, `pool_f64[]`, `pool_symref[]` - backing pools for IROperand values
- Synchronization functions exist: `tcc_ir_sync_operand()`, `tcc_ir_sync_quad()`

### Conversion Functions (Already Implemented)
- `svalue_to_iroperand()` - converts SValue → IROperand (adds to pools)
- `iroperand_to_svalue()` - expands IROperand → SValue (for backward compat)

### Current Accessor Pattern
```c
// Old pattern (reads from svalue_pool):
SValue *src1 = tcc_ir_op_get_src1(ir, q);

// New pattern (reads IROperand, expands to local SValue):
IROperand op = tcc_ir_op_get_src1_irop(ir, q);
SValue src1_local;
iroperand_to_svalue(ir, op, &src1_local);
```

---

## Migration Strategy: Function-by-Function

### Key Principle
1. **Pick ONE function** to migrate
2. **Change its signature** to accept `IROperand` instead of `SValue*`
3. **Add synchronization** where results are written back
4. **Test thoroughly** before moving to the next function
5. **Repeat**

### Synchronization Pattern

When a codegen function writes back to IR state, use:
```c
// If function modified operands:
tcc_ir_sync_operand(ir, instr_idx, OPERAND_DEST, &local_sv);
// Or for full quad:
tcc_ir_sync_quad(ir, instr_idx, &quad);
```

---

## Phase 1: Codegen Entry Points (arm-thumb-gen.c)

These are the main `tcc_gen_machine_*` functions called from `tcc_ir_generate_code()`.

### Function Migration Order (by complexity, simplest first):

| # | Function | SValue Params | Complexity | Notes |
|---|----------|---------------|------------|-------|
| 1 | `tcc_gen_machine_jump_op` | dest | Simple | Jump target only |
| 2 | `tcc_gen_machine_end_instruction` | none | Trivial | Already no SValues |
| 3 | `tcc_gen_machine_indirect_jump_op` | src1 | Simple | Single operand |
| 4 | `tcc_gen_machine_return_value_op` | src1 | Simple | Single operand |
| 5 | `tcc_gen_machine_load_op` | src1, dest | Medium | Read value |
| 6 | `tcc_gen_machine_store_op` | src, dest | Medium | Write value |
| 7 | `tcc_gen_machine_assign_op` | src1, dest | Medium | Value copy |
| 8 | `tcc_gen_machine_lea_op` | src1, dest | Medium | Address of |
| 9 | `tcc_gen_machine_conditional_jump_op` | cond, dest | Medium | Branch |
| 10 | `tcc_gen_machine_setif_op` | src1, src2, dest | Medium | Comparison |
| 11 | `tcc_gen_machine_bool_op` | src1, src2, dest | Medium | Logic |
| 12 | `tcc_gen_machine_data_processing_op` | src1, src2, dest | Complex | Most ALU ops |
| 13 | `tcc_gen_machine_fp_op` | src1, src2, dest | Complex | FP operations |
| 14 | `tcc_gen_machine_func_call_op` | func, call_id, dest | Complex | Function calls |
| 15 | `tcc_gen_machine_func_parameter_op` | src1, src2 | Complex | Call setup |
| 16 | `tcc_gen_machine_vla_op` | src1, src2, dest | Complex | VLA handling |

---

## Phase 2: Helper Functions

Internal helpers that operate on SValues:

| Function | Current Signature | Action |
|----------|------------------|--------|
| `load_to_dest` | `(SValue *dest, SValue *sv)` | Convert to IROperand |
| `load_to_register` | `(int reg, int reg_from, SValue *src)` | Convert to IROperand |
| `load_vt_local` | `(int r, SValue *sv, int base)` | Convert to IROperand |
| `load_vt_lval_vt_local` | `(int r, int r1, SValue *sv, ...)` | Convert to IROperand |
| `store_ex` | `(int r, SValue *sv, uint32_t extra)` | Convert to IROperand |
| `load` | `(int r, SValue *sv)` | Convert to IROperand |
| `store` | `(int r, SValue *sv)` | Convert to IROperand |

---

## Phase 3: Optimization Passes (tccir.c)

These read/modify IR instructions:

| Pass | SValue Access | Migration Notes |
|------|--------------|-----------------|
| `tcc_ir_dead_code_elimination` | Read-only | Easy - just change accessors |
| `tcc_ir_constant_propagation` | Read + Write | Use sync after writes |
| `tcc_ir_copy_propagation` | Read + Write | Use sync after writes |
| `tcc_ir_dead_store_elimination` | Read + Write | Use sync after writes |
| `tcc_ir_store_load_forwarding` | Read + Write | Use sync after writes |
| `tcc_ir_arithmetic_cse` | Read + Write | Use sync after writes |
| `tcc_ir_bool_*` | Read + Write | Use sync after writes |

---

## Detailed Migration Steps per Function

### Step Template (for each function):

#### 1. Create IROperand Wrapper
```c
// Before (old signature):
ST_FUNC void tcc_gen_machine_load_op(SValue *src1, SValue *dest, TccIrOp op);

// After (new signature):
ST_FUNC void tcc_gen_machine_load_op_ir(TCCIRState *ir, int instr_idx, IROperand src1_op, IROperand dest_op, TccIrOp op);

// Transitional shim (keeps old callers working):
ST_FUNC void tcc_gen_machine_load_op(SValue *src1, SValue *dest, TccIrOp op)
{
  // Old callers go through expansion path - this will be removed
  tcc_gen_machine_load_op_legacy(src1, dest, op);
}
```

#### 2. Implement IROperand Version
```c
ST_FUNC void tcc_gen_machine_load_op_ir(TCCIRState *ir, int instr_idx, IROperand src1_op, IROperand dest_op, TccIrOp op)
{
  // Expand to local SValues for current codegen
  SValue src1, dest;
  iroperand_to_svalue(ir, src1_op, &src1);
  iroperand_to_svalue(ir, dest_op, &dest);

  // ... existing codegen logic using src1, dest ...

  // If dest was modified, sync back
  if (modified_dest) {
    tcc_ir_sync_operand(ir, instr_idx, OPERAND_DEST, &dest);
  }
}
```

#### 3. Update Caller in tcc_ir_generate_code()
```c
// Before:
SValue *src1 = tcc_ir_op_get_src1(ir, q);
SValue *dest = tcc_ir_op_get_dest(ir, q);
tcc_gen_machine_load_op(src1, dest, op);

// After:
IROperand src1_op = tcc_ir_op_get_src1_irop(ir, q);
IROperand dest_op = tcc_ir_op_get_dest_irop(ir, q);
tcc_gen_machine_load_op_ir(ir, i, src1_op, dest_op, op);
```

#### 4. Test & Validate
- Run test suite
- Compare generated code (should be identical)
- Validate both pools remain in sync

#### 5. Remove Legacy Path
Once all callers are migrated, remove the shim and old signature.

---

## Implementation Order (Recommended)

### Week 1: Foundation
1. **Add `OPERAND_DEST=0, OPERAND_SRC1=1, OPERAND_SRC2=2` constants**
2. **Migrate `tcc_gen_machine_jump_op`** (simplest, single operand)
3. **Migrate `tcc_gen_machine_indirect_jump_op`**
4. **Migrate `tcc_gen_machine_return_value_op`**

### Week 2: Load/Store Path
5. **Migrate `tcc_gen_machine_load_op`**
6. **Migrate `tcc_gen_machine_store_op`**
7. **Migrate `tcc_gen_machine_assign_op`**
8. **Migrate `tcc_gen_machine_lea_op`**

### Week 3: Control Flow
9. **Migrate `tcc_gen_machine_conditional_jump_op`**
10. **Migrate `tcc_gen_machine_setif_op`**
11. **Migrate `tcc_gen_machine_bool_op`**

### Week 4: Data Processing
12. **Migrate `tcc_gen_machine_data_processing_op`** (largest)
13. **Migrate `tcc_gen_machine_fp_op`**

### Week 5: Function Calls
14. **Migrate `tcc_gen_machine_func_parameter_op`**
15. **Migrate `tcc_gen_machine_func_call_op`**
16. **Migrate `tcc_gen_machine_vla_op`**

### Week 6: Optimization Passes
17. **Migrate DCE** (read-only, easy)
18. **Migrate constant propagation**
19. **Migrate copy propagation**
20. **Migrate remaining passes**

### Week 7: Helper Functions & Cleanup
21. **Migrate `load`, `store`, `load_to_dest`** etc.
22. **Remove svalue_pool** (final step)
23. **Rename `iroperand_pool` to canonical name**

---

## Validation Checklist (per function)

- [ ] Function compiles without errors
- [ ] Function signature updated in header
- [ ] Caller(s) updated to pass IROperand
- [ ] Sync calls added where operands modified
- [ ] Test case compiles correctly
- [ ] Generated assembly matches pre-migration
- [ ] No memory leaks (pools properly managed)

---

## Risk Mitigation

1. **Keep Both Pools**: During migration, always maintain both `svalue_pool` and `iroperand_pool` in sync
2. **Incremental Testing**: Test after each function migration
3. **Feature Flags**: Can add `#ifdef USE_IROPERAND_CODEGEN` to toggle between paths
4. **Bisectable**: Each migration step should be a separate, small commit

---

## Success Metrics

- [ ] All `tcc_gen_machine_*` functions use IROperand parameters
- [ ] `svalue_pool` removed from `TCCIRState`
- [ ] 80%+ memory reduction in IR storage (204 bytes → ~24 bytes per instruction)
- [ ] No performance regression (< 5% compilation speed change)
- [ ] All existing tests pass
- [ ] Binary output identical to pre-migration

---

## Files Modified (Summary)

| File | Changes |
|------|---------|
| `tccir.h` | Add operand slot constants, update function signatures |
| `tccir.c` | Update callers in `tcc_ir_generate_code()`, migrate optimization passes |
| `arm-thumb-gen.c` | Migrate all `tcc_gen_machine_*` functions |
| `tccir_operand.h` | Add any new helper macros |
| `tccir_operand.c` | Add any new conversion utilities |

---

## First Migration: tcc_gen_machine_jump_op

**File**: [arm-thumb-gen.c](arm-thumb-gen.c#L7399)

**Current signature**:
```c
ST_FUNC void tcc_gen_machine_jump_op(SValue *dest, TccIrOp op);
```

**New signature**:
```c
ST_FUNC void tcc_gen_machine_jump_op(TCCIRState *ir, IROperand dest_op, TccIrOp op);
```

**Why start here**:
- Single operand (dest only)
- No complex logic
- No writes back to IR (jump target is read-only by this point)
- Fast validation

**Implementation**:
```c
ST_FUNC void tcc_gen_machine_jump_op(TCCIRState *ir, IROperand dest_op, TccIrOp op)
{
  SValue dest;
  iroperand_to_svalue(ir, dest_op, &dest);

  // Existing logic - emit unconditional branch
  int target = dest.c.i;
  // ... rest of current implementation ...
}
```

Ready to begin?
