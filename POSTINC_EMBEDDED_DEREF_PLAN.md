# Post-Increment Fusion for Embedded Dereference Patterns

## Executive Summary

The current post-increment optimization in TCC only handles standalone `LOAD`/`STORE` operations followed by pointer increment. However, C's `*p++` operator often generates IR where the dereference is **embedded** in another operation (like `ADD`), not as a standalone `LOAD`. This plan addresses extending post-increment fusion to handle these embedded dereference patterns.

## Problem Analysis

### Pattern 1: Explicit Dereference (Currently Working)
```c
int val = *p;
p++;
sum += val;
```

**IR Generated:**
```
0007: V1 <-- T2***DEREF*** [LOAD]    ← Standalone LOAD ✓
0010: P0 <-- T5 [STORE]              ← Pointer update
0011: V0 <-- V0 ADD V1               ← Uses loaded value
```

**After Optimization (post-increment applied):**
```
0007: R4(V1) <-- R0(P0) [LOAD_POSTINC #4]
0011: R2(V0) <-- R2(V0) ADD R4(V1)
```

### Pattern 2: `*p++` in Expression (NOT Working)
```c
sum += *p++;
```

**IR Generated:**
```
0006: T2 <-- P0 [ASSIGN]             ← Save old pointer
0007: T3 <-- T2 ADD #4               ← Compute new pointer
0008: P0 <-- T3 [STORE]              ← Update pointer
0009: V0 <-- V0 ADD T2***DEREF***    ← DEREF embedded in ADD!
```

**Current Output (no fusion):**
```asm
mov     r4, r0           ; save old pointer
adds    r5, r0, #4       ; compute new pointer
mov     r0, r5           ; update p
ldr.w   ip, [r4]         ; load *old_ptr
add     r2, ip           ; sum += val
```
**Total: 5 instructions (10+ bytes)**

**Desired Output:**
```asm
ldr.w   ip, [r0], #4     ; load *p with post-increment
add     r2, ip           ; sum += val
```
**Total: 2 instructions (6 bytes)**

## Root Cause

The current `tcc_ir_opt_postinc_fusion()` in `ir/opt.c` only looks for:
- `TCCIR_OP_LOAD` followed by pointer ADD
- `TCCIR_OP_STORE` followed by pointer ADD

It doesn't handle the case where the dereference is **embedded in another operation** like:
- `V0 <-- V0 ADD T2***DEREF***` (deref in ADD)
- `T6 <-- T3***DEREF*** MUL T5***DEREF***` (deref in MUL - handled by MLA now)

## Solution Approaches

### Approach A: IR Lowering - Extract Embedded DEREF to Explicit LOAD (Recommended)

**Idea:** Before running post-increment fusion, run a pass that extracts embedded dereferences into explicit LOAD operations.

**Transform:**
```
BEFORE:
0006: T2 <-- P0 [ASSIGN]
0007: T3 <-- T2 ADD #4
0008: P0 <-- T3 [STORE]
0009: V0 <-- V0 ADD T2***DEREF***

AFTER EXTRACTION:
0006: T2 <-- P0 [ASSIGN]
0007: T3 <-- T2 ADD #4
0008: P0 <-- T3 [STORE]
0009a: T_loaded <-- T2***DEREF*** [LOAD]   ← NEW explicit LOAD
0009b: V0 <-- V0 ADD T_loaded              ← Uses loaded value

AFTER POST-INC FUSION:
0006: T_loaded <-- P0 [LOAD_POSTINC #4]    ← Combined!
0009b: V0 <-- V0 ADD T_loaded
```

**Pros:**
- Reuses existing post-increment fusion logic
- Clean separation of concerns
- Easy to understand and maintain

**Cons:**
- Adds an extra optimization pass
- May slightly increase IR instruction count temporarily

### Approach B: Extend Post-Increment Fusion to Handle Embedded DEREF

**Idea:** Modify `tcc_ir_opt_postinc_fusion()` to detect when a DEREF operand in any operation matches the pointer-copy + increment pattern.

**Pattern to detect:**
```
i:   T2 <-- P0 [ASSIGN]           ; ptr_copy = ptr
i+1: T3 <-- T2 ADD #imm           ; new_ptr = ptr_copy + imm
i+2: P0 <-- T3 [STORE]            ; ptr = new_ptr
...
i+k: V0 <-- V0 ADD T2***DEREF***  ; use *ptr_copy (DEREF embedded)
```

**Transform to:**
```
i:   LOAD_POSTINC temp, P0, #imm  ; temp = *ptr; ptr += imm
...
i+k: V0 <-- V0 ADD temp           ; use loaded value (no DEREF)
```

**Pros:**
- More direct transformation
- Single pass handles both patterns

**Cons:**
- More complex pattern matching
- Needs to handle multiple uses of DEREF operand
- Risk of missing edge cases

### Approach C: Frontend Change - Emit Explicit LOAD for Post-Increment

**Idea:** Modify `tccgen.c` to always emit explicit LOAD for post-increment patterns, even when embedded in expressions.

**Pros:**
- Fixes issue at source
- Simplifies IR patterns

**Cons:**
- Frontend changes are risky
- May affect other optimizations

## Recommended Implementation: Approach A

### Step 1: Add DEREF Extraction Pass

Create new function `tcc_ir_opt_extract_embedded_deref()` in `ir/opt.c`:

```c
/* ============================================================================
 * Embedded Dereference Extraction
 * ============================================================================
 *
 * Extracts embedded dereference operands into explicit LOAD instructions.
 * This enables other optimizations (like post-increment fusion) to work.
 *
 * Pattern:  dest = op1 OP op2***DEREF***
 * Becomes:  temp = op2***DEREF*** [LOAD]
 *           dest = op1 OP temp
 *
 * Only extracts DEREF when:
 * 1. The DEREF operand is part of a ptr++ pattern (ASSIGN + ADD + STORE)
 * 2. The extraction enables post-increment fusion
 */
int tcc_ir_opt_extract_embedded_deref(TCCIRState *ir);
```

### Step 2: Pattern Detection

Detect the `*p++` pattern:
```c
/* Look for: ASSIGN ptr_copy, ptr; ADD new_ptr, ptr_copy, #imm; STORE ptr, new_ptr
 * Where ptr_copy has DEREF usage later */

for each instruction i:
  if i is ASSIGN and dest is TEMP:
    ptr_copy = dest
    ptr = src1

    look for ADD at i+1 or i+2:
      if ADD uses ptr_copy and immediate:
        look for STORE of result back to ptr

    if pattern found:
      search for uses of ptr_copy***DEREF*** in later instructions
      for each DEREF use:
        insert explicit LOAD before the using instruction
        replace DEREF operand with loaded temp
```

### Step 3: Insert Explicit LOAD

When pattern detected, transform:
```c
/* Original: */
0009: V0 <-- V0 ADD T2***DEREF***

/* Insert LOAD before, change ADD to use loaded value: */
0009a: T_new <-- T2***DEREF*** [LOAD]
0009b: V0 <-- V0 ADD T_new
```

### Step 4: Let Existing Post-Inc Fusion Handle It

After extraction, the pattern becomes:
```
T2 <-- P0 [ASSIGN]
T3 <-- T2 ADD #4
P0 <-- T3 [STORE]
T_new <-- T2***DEREF*** [LOAD]    ← Now explicit LOAD!
V0 <-- V0 ADD T_new
```

The existing `tcc_ir_opt_postinc_fusion()` can then fuse the LOAD + pointer update.

## Implementation Details

### New VReg Allocation

Need to allocate new temporary vregs for the explicit LOADs:
```c
int32_t new_vreg = tcc_ir_alloc_vreg(ir, TCCIR_VREG_TYPE_TEMP);
```

### Operand Pool Management

When inserting new instructions, need to:
1. Add operands to the operand pool
2. Possibly shift existing instruction indices

**Alternative:** Rewrite the instruction in-place by:
- Keeping the original instruction slot
- Adding a new instruction after it
- Using the compact instruction's NOP slots

### Register Allocation Considerations

The new temporary will need register allocation. Since we're extracting from an existing DEREF operand:
- The DEREF already requires a load (codegen does this)
- We're just making it explicit in IR
- Should not significantly impact register pressure

## Test Cases

### Test 1: Simple `*p++` in ADD
```c
int test1(int *p, int n) {
    int sum = 0;
    while (n-- > 0)
        sum += *p++;
    return sum;
}
```
Expected: `ldr.w rX, [rP], #4` + `add sum, rX`

### Test 2: Multiple `*p++` in Expression
```c
void test2(int *dst, int *src1, int *src2, int n) {
    for (int i = 0; i < n; i++)
        *dst++ = *src1++ + *src2++;
}
```
Expected: 3 post-increment loads/stores

### Test 3: `*p++` in MUL (with MLA)
```c
int test3(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += *a++ * *b++;
    return sum;
}
```
Expected: 2 post-increment loads + MLA

### Test 4: Pre-decrement `*--p`
```c
int test4(int *p, int n) {
    p += n;
    int sum = 0;
    while (n-- > 0)
        sum += *--p;
    return sum;
}
```
Expected: `ldr.w rX, [rP, #-4]!` (pre-decrement)

## Expected Code Size Improvements

| Function | Current TCC | With Fix | GCC | Improvement |
|----------|-------------|----------|-----|-------------|
| sum_array | 36 bytes | ~24 bytes | 30 bytes | -33% |
| copy_sum | 60 bytes | ~40 bytes | 36 bytes | -33% |
| copy | 40 bytes | ~28 bytes | 28 bytes | -30% |

## Implementation Checklist

- [ ] **ir/opt.c**: Add `tcc_ir_opt_extract_embedded_deref()` function
- [ ] **ir/opt.c**: Add helper to detect `*p++` pattern (ASSIGN+ADD+STORE)
- [ ] **ir/opt.c**: Add helper to find DEREF uses of a temp vreg
- [ ] **ir/opt.c**: Implement LOAD extraction logic
- [ ] **ir/opt.h**: Declare new function
- [ ] **tccir.c or tccopt.c**: Call new pass before post-increment fusion
- [ ] **Tests**: Add test cases for embedded DEREF patterns
- [ ] **Verify**: Run `make test -j16`
- [ ] **Benchmark**: Run comparison script

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Wrong DEREF extraction | Medium | High | Only extract when ptr++ pattern detected |
| Double load | Medium | High | Track which DEREFs already extracted |
| Register pressure | Low | Medium | Reuses same value that was being loaded anyway |
| Breaking existing code | Medium | High | Extensive test suite run |

## Timeline Estimate

- **Step 1 (Pattern detection):** 30 minutes
- **Step 2 (LOAD extraction):** 45 minutes
- **Step 3 (Integration):** 30 minutes
- **Step 4 (Testing):** 30 minutes
- **Total:** ~2-2.5 hours

## Alternative: Codegen-Level Post-Increment

Instead of IR transformation, could detect the pattern at codegen time in `arm-thumb-gen.c`:

1. When generating code for `ADD T2***DEREF***`:
2. Look back for the ASSIGN+ADD+STORE pattern
3. Emit `LDR rX, [rP], #imm` instead of separate instructions

**Pros:** No IR changes needed
**Cons:** Architecture-specific, harder to maintain, may miss optimization opportunities

## References

- Current post-inc fusion: `ir/opt.c` lines 3467-3800
- ARM post-increment: `LDR Rt, [Rn], #imm` (T4 encoding, 4 bytes)
- ARM pre-increment: `LDR Rt, [Rn, #imm]!` (T4 encoding, 4 bytes)
- Existing DEREF handling: `arm-thumb-gen.c` `load_to_reg_ir()` function
