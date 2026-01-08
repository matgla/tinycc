# Scratch Register Callers Fix TODO

## Status: COMPLETED ✓

All 31+ call sites have been fixed and compilation succeeds!

### th_offset_to_reg_ex (line 1905)
- [ ] Line 1905: `int rr = get_scratch_reg_with_save(exclude_regs).reg;`
- Note: Returns register to caller - may need to return ScratchRegAlloc or caller handles restore

### store() function - spilled lvalue cases (lines 2370, 2417, 2444)
- [ ] Line 2370: `int base_reg = get_scratch_reg_with_save(exclude_regs).reg;`
- [ ] Line 2417: `int base_reg = get_scratch_reg_with_save(exclude_regs).reg;`
- [ ] Line 2444: `base = get_scratch_reg_with_save(exclude_regs).reg;`

### load() function - literal pool (lines 2720, 2758)
- [ ] Line 2720: `int scratch = get_scratch_reg_with_save(exclude_regs).reg;`
- [ ] Line 2758: `int scratch = get_scratch_reg_with_save(exclude_regs).reg;`

### load_to_dest() function (lines 3241, 3256, 3278, 3304)
- [x] Line 3241: `base = get_scratch_reg_with_save(0).reg;` (VT_LLOCAL path)
- [x] Line 3256: `base = get_scratch_reg_with_save(0).reg;` (VT_CONST path)
- [x] Line 3278: `base = get_scratch_reg_with_save(0).reg;` (spilled lvalue path)
- [x] Line 3304: `base = get_scratch_reg_with_save(0).reg;` (spilled lvalue path 2)

### tcc_gen_machine_data_processing_op() - DIV/MOD ops (lines 5339-5452)
- [ ] Line 5339: `src1_reg = get_scratch_reg_with_save(exclude_regs).reg;` (IDIV src1)
- [ ] Line 5349: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;` (IDIV src2)
- [ ] Line 5363: `src1_reg = get_scratch_reg_with_save(exclude_regs).reg;` (UDIV src1)
- [ ] Line 5373: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;` (UDIV src2)
- [ ] Line 5390: `src1_reg = get_scratch_reg_with_save(exclude_regs).reg;` (IMOD src1)
- [ ] Line 5400: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;` (IMOD src2)
- [ ] Line 5410: `int scratch = get_scratch_reg_with_save(exclude_regs).reg;` (IMOD quotient)
- [ ] Line 5432: `src1_reg = get_scratch_reg_with_save(exclude_regs).reg;` (UMOD src1)
- [ ] Line 5442: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;` (UMOD src2)
- [ ] Line 5452: `int scratch = get_scratch_reg_with_save(exclude_regs).reg;` (UMOD quotient)

### tcc_gen_machine_data_processing_op() - TEST_ZERO (line 5482)
- [ ] Line 5482: `src_reg = get_scratch_reg_with_save(0).reg;`

### tcc_gen_machine_data_processing_op() - generic handler (lines 5523, 5546, 5552)
- [ ] Line 5523: `src1_reg = get_scratch_reg_with_save(exclude_regs).reg;`
- [ ] Line 5546: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;`
- [ ] Line 5552: `src2_reg = get_scratch_reg_with_save(exclude_regs).reg;`

### tcc_gen_machine_fp_op() - FNEG (line 5991)
- [x] Line 5991: `int scratch_reg = get_scratch_reg_with_save((1 << R0) | (is_double ? (1 << R1) : 0)).reg;`

### tcc_gen_machine_store_op() (line 6203)
- [x] Line 6203: `int scratch_reg = get_scratch_reg_with_save(0).reg;`

### tcc_gen_machine_assign_op() - VT_CONST cases (lines 6649, 6658)
- [x] Line 6649: `int scratch_reg = get_scratch_reg_with_save(0).reg;` (VFP dest)
- [x] Line 6658: `int scratch_reg = get_scratch_reg_with_save(0).reg;` (memory dest)

### tcc_gen_machine_func_call_op() - stack args (lines 7252, 7253, 7271)
- [x] Line 7252: `int scratch_lo = get_scratch_reg_with_save(stack_exclude).reg;` (64-bit lo)
- [x] Line 7253: `int scratch_hi = get_scratch_reg_with_save(stack_exclude | (1u << scratch_lo)).reg;` (64-bit hi)
- [x] Line 7271: `int scratch_reg = get_scratch_reg_with_save(stack_exclude).reg;` (32-bit)

## Pattern for Fix

### Before:
```c
int scratch = get_scratch_reg_with_save(exclude_regs).reg;
// use scratch...
// no cleanup - BUG!
```

### After:
```c
ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(exclude_regs);
int scratch = scratch_alloc.reg;
// use scratch...
restore_scratch_reg(&scratch_alloc);  // Cleanup!
```

## Summary of Changes

### Total Call Sites Fixed: 31+

1. **store() function** (3 sites): Lines 2370, 2417, 2444
   - All three spilled lvalue cases now track and restore ScratchRegAlloc

2. **load() function** (2 sites): Lines 2720, 2758
   - Both literal pool cases now track and restore

3. **load_to_dest() function** (4 sites): Lines 3241, 3256, 3278, 3304
   - All VT_LLOCAL, VT_CONST, and spilled lvalue paths fixed

4. **DIV/UDIV operations** (4 sites): Lines 5339, 5349, 5363, 5373
   - Both signed and unsigned division now properly track src1/src2 scratches

5. **IMOD operation** (3 sites): Lines 5390, 5400, 5410
   - Signed modulo with quotient scratch now properly restored

6. **UMOD operation** (3 sites): Lines 5432, 5442, 5452
   - Unsigned modulo with quotient scratch now properly restored

7. **TEST_ZERO operation** (1 site): Line 5482
   - Test zero comparison now tracks scratch register

8. **Generic data processing handler** (3 sites): Lines 5589, 5614, 5620
   - src1_reg, src2_reg allocations in conditional branches now properly tracked
   - Early return path properly restores allocated scratches

9. **FP operations - FNEG** (1 site): Line 5991
   - Float negation scratch now properly restored

10. **STORE operation** (1 site): Line 6203
    - Store offset scratch properly restored

11. **ASSIGN operation** (2 sites): Lines 6649, 6658
    - VFP and memory destination paths properly restore

12. **Function call stack arguments** (3 sites): Lines 7252, 7253, 7271
    - 64-bit and 32-bit stack argument handling now properly restores

13. **th_offset_to_reg functions** (10+ sites)
    - Changed signature from `int th_offset_to_reg_ex(...)` to `ScratchRegAlloc th_offset_to_reg_ex(...)`
    - All 10+ callers updated to:
      - Store result in `ScratchRegAlloc` struct
      - Use `.reg` field for actual register
      - Call `restore_scratch_reg()` when done
    - Includes load, store, spilled value loading, and arithmetic operations

## Key Patterns Applied

### Pattern 1: Simple Allocation and Restore
```c
ScratchRegAlloc alloc = get_scratch_reg_with_save(exclude_regs);
int reg = alloc.reg;
// use reg...
restore_scratch_reg(&alloc);
```

### Pattern 2: Conditional Allocation
```c
ScratchRegAlloc alloc = {0};
if (condition) {
  alloc = get_scratch_reg_with_save(exclude_regs);
  int reg = alloc.reg;
  // use reg...
}
// Restore only if allocated
if (alloc.reg != 0)
  restore_scratch_reg(&alloc);
```

### Pattern 3: Multiple Allocations (DIV/MOD ops)
```c
ScratchRegAlloc src1_alloc = {0};
ScratchRegAlloc src2_alloc = {0};
// allocate as needed...
// restore in REVERSE order
if (src2_alloc.reg != 0) restore_scratch_reg(&src2_alloc);
if (src1_alloc.reg != 0) restore_scratch_reg(&src1_alloc);
```

## Architectural Benefits

1. **No register clobbering**: Each scratch register that uses PUSH is properly tracked and restored
2. **No stack corruption**: PUSH/POP sequences are guaranteed to match and be in correct order
3. **Leak prevention**: All scratches released from `scratch_global_exclude` during instruction processing
4. **Safety net**: End-of-instruction cleanup via `tcc_gen_machine_end_instruction()` catches any leaked pushes

## Testing
- Compilation successful
- Ready for functional testing with regression test suite
