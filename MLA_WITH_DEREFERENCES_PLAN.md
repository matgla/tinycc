# MLA with Dereferences - Implementation Plan

## Executive Summary

Currently, TCC can fuse `MUL + ADD → MLA` for simple cases like `acc + a * b` where all operands are registers. However, for array access patterns like `sum += a[i] * b[i]`, the MUL has **dereferenced operands** (`T3***DEREF*** MUL T5***DEREF***`) which blocks fusion. This plan addresses enabling MLA fusion when MUL operands require memory loads.

## Current State Analysis

### Working Case: Simple MLA
```c
int simple_mla(int a, int b, int acc) {
    return acc + a * b;
}
```

**IR Before Optimization:**
```
0000: T0 <-- P0 MUL P1
0001: T1 <-- P2 ADD T0
0002: RETURNVALUE T1
```

**IR After Optimization:**
```
0000: R3(T1) <-- R0(P0) MLA R1(P1) + R2(P2)
0001: NOP
0002: RETURNVALUE R3(T1)
```

**Generated Assembly:**
```asm
mla     r3, r0, r1, r2
mov     r0, r3
bx      lr
```

### Failing Case: Array Dereferences
```c
int dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
```

**IR After Optimization (no MLA fusion):**
```
0008: R5(T2) <-- R4(V1) SHL #2
0009: R6(T3) <-- R0(P0) ADD R5(T2)
0010: R8(T4) <-- R5(T2) [ASSIGN]
0011: R5(T5) <-- R1(P1) ADD R8(T4)
0012: R8(T6) <-- R6(T3)***DEREF*** MUL R5(T5)***DEREF***  ← DEREF blocks fusion
0013: R3(V0) <-- R3(V0) ADD R8(T6)
0014: JMP to 5
```

**Generated Assembly (suboptimal):**
```asm
ldr.w   ip, [r6]         ; load a[i]
ldr.w   lr, [r5]         ; load b[i]
mul.w   r8, ip, lr       ; mul.w = 4 bytes
add     r3, r8           ; add = 2 bytes (6 bytes total)
```

**GCC Output (optimal):**
```asm
ldr     ip, [r3, #4]!    ; load a[i] with pre-increment
ldr     lr, [r1, #4]!    ; load b[i] with pre-increment
mla     r0, lr, ip, r0   ; mla = 4 bytes (saves 2 bytes)
```

## Root Cause

In `ir/opt.c`, the MLA fusion check at line 3044-3050:

```c
/* Check 4: Skip if MUL operands require memory dereference or are immediates. */
IROperand mul_src1 = tcc_ir_op_get_src1(ir, mul_q);
IROperand mul_src2 = tcc_ir_op_get_src2(ir, mul_q);
int src1_needs_deref = mul_src1.is_lval && !mul_src1.is_local && !mul_src1.is_llocal;
int src2_needs_deref = mul_src2.is_lval && !mul_src2.is_local && !mul_src2.is_llocal;
if (src1_needs_deref || src2_needs_deref || src1_is_immediate || src2_is_immediate)
{
  continue;  // ← Fusion blocked!
}
```

The fusion is blocked because the backend code generator (`thumb_emit_regonly_binop32`) already handles DEREF operands by emitting loads to scratch registers. The fusion skip was added to avoid complications, but it's overly conservative.

## Why MLA with DEREF Should Work

The ARM MLA instruction requires all operands in registers. However, **the load instructions for DEREF operands must happen anyway**. The key insight:

### Current Behavior (MUL + ADD)
```
ldr     rX, [addr1]      ; load for DEREF src1
ldr     rY, [addr2]      ; load for DEREF src2
mul     rT, rX, rY       ; mul result in rT
add     rD, rD, rT       ; add to accumulator
```
**Total: 4 instructions** (2 LDR + MUL + ADD)

### With MLA Fusion
```
ldr     rX, [addr1]      ; load for DEREF src1
ldr     rY, [addr2]      ; load for DEREF src2
mla     rD, rX, rY, rD   ; multiply-accumulate
```
**Total: 3 instructions** (2 LDR + MLA) — **saves 1 instruction**

## Implementation Options

### Option A: Allow DEREF in IR-Level MLA Fusion (Recommended)

**Location:** `ir/opt.c`, function `tcc_ir_opt_mla_fusion()`

**Change:** Remove the DEREF check that blocks fusion

```c
// BEFORE (lines 3044-3050):
int src1_needs_deref = mul_src1.is_lval && !mul_src1.is_local && !mul_src1.is_llocal;
int src2_needs_deref = mul_src2.is_lval && !mul_src2.is_local && !mul_src2.is_llocal;
if (src1_needs_deref || src2_needs_deref || src1_is_immediate || src2_is_immediate)
{
  continue;
}

// AFTER:
// Remove the DEREF check, keep only immediate check
int src1_is_immediate = irop_is_immediate(mul_src1);
int src2_is_immediate = irop_is_immediate(mul_src2);
if (src1_is_immediate || src2_is_immediate)
{
  continue;  // MLA doesn't support immediate operands
}
// NOTE: DEREF operands are OK - codegen will emit loads to scratch regs
```

**Codegen Already Handles This:** The `thumb_emit_mul32()` → `thumb_emit_regonly_binop32()` path already materializes DEREF operands:

```c
// arm-thumb-gen.c lines 4108-4114
if (rn == PREG_REG_NONE || src1.is_lval || thumb_irop_needs_value_load(src1) || ...) {
  rn_alloc = get_scratch_reg_with_save(exclude);
  rn = rn_alloc.reg;
  load_to_reg_ir(rn, PREG_NONE, src1_tmp);  // Loads DEREF to register
}
```

**Required Change in MLA Codegen:** Update `arm-thumb-gen.c` MLA handler (lines 4553-4605) to handle DEREF operands similar to MUL handler:

```c
case TCCIR_OP_MLA:
{
  /* MLA: dest = src1 * src2 + accum */
  TCCIRState *ir_state = tcc_state->ir;
  int instr_idx = ir_state->codegen_instruction_idx;
  IRQuadCompact *mla_q = &ir_state->compact_instructions[instr_idx];
  IROperand accum = tcc_ir_op_get_accum_inline(ir_state, mla_q);

  int src1_reg = src1.pr0_reg;
  int src2_reg = src2.pr0_reg;
  int dest_reg = dest.pr0_reg;

  /* Handle DEREF operands - load to scratch registers if needed */
  ScratchRegAlloc src1_alloc = {0};
  ScratchRegAlloc src2_alloc = {0};
  uint32_t exclude = (1u << dest_reg);

  /* Check if src1 needs loading (DEREF or missing register) */
  if (src1_reg == PREG_REG_NONE || src1.is_lval ||
      thumb_irop_needs_value_load(src1) || thumb_irop_has_immediate_value(src1)) {
    src1_alloc = get_scratch_reg_with_save(exclude);
    src1_reg = src1_alloc.reg;
    exclude |= (1u << src1_reg);
    load_to_reg_ir(src1_reg, PREG_NONE, src1);
  }

  /* Check if src2 needs loading (DEREF or missing register) */
  if (src2_reg == PREG_REG_NONE || src2.is_lval ||
      thumb_irop_needs_value_load(src2) || thumb_irop_has_immediate_value(src2)) {
    src2_alloc = get_scratch_reg_with_save(exclude);
    src2_reg = src2_alloc.reg;
    exclude |= (1u << src2_reg);
    load_to_reg_ir(src2_reg, PREG_NONE, src2);
  }

  /* Get accumulator register */
  int accum_reg = accum.pr0_reg;
  int32_t accum_vr = irop_get_vreg(accum);
  if (accum_vr >= 0) {
    IRLiveInterval *accum_li = tcc_ir_get_live_interval(ir_state, accum_vr);
    if (accum_li && accum_li->allocation.r0 != PREG_REG_NONE) {
      accum_reg = accum_li->allocation.r0;
    }
  }

  /* Emit MLA instruction */
  ot_check(th_mla((uint32_t)dest_reg, (uint32_t)src1_reg,
                  (uint32_t)src2_reg, (uint32_t)accum_reg));

  restore_scratch_reg(&src2_alloc);
  restore_scratch_reg(&src1_alloc);
  return;
}
```

### Option B: Pre-Load DEREF Operands Before MLA Fusion

**Alternative Approach:** Insert explicit LOAD instructions before MUL when it has DEREF operands, converting:
```
T6 <-- T3***DEREF*** MUL T5***DEREF***
```
To:
```
T7 <-- T3 [LOAD]       ; explicit load of *T3
T8 <-- T5 [LOAD]       ; explicit load of *T5
T6 <-- T7 MUL T8       ; now MUL has register operands
```

**Pros:**
- MLA fusion logic unchanged
- Cleaner IR representation

**Cons:**
- Increases instruction count in IR
- May affect register allocation
- More complex transformation

## Recommended Implementation: Option A

### Step 1: Update IR Optimization (ir/opt.c)

```c
int tcc_ir_opt_mla_fusion(TCCIRState *ir)
{
  // ... existing code ...

  // At line ~3044, REMOVE the DEREF check:
  // OLD:
  // int src1_needs_deref = mul_src1.is_lval && !mul_src1.is_local && !mul_src1.is_llocal;
  // int src2_needs_deref = mul_src2.is_lval && !mul_src2.is_local && !mul_src2.is_llocal;
  // if (src1_needs_deref || src2_needs_deref || src1_is_immediate || src2_is_immediate)

  // NEW:
  int src1_is_immediate = irop_is_immediate(mul_src1);
  int src2_is_immediate = irop_is_immediate(mul_src2);
  if (src1_is_immediate || src2_is_immediate)
  {
    continue;  // MLA can't use immediates, but DEREF is OK
  }
```

### Step 2: Update Code Generation (arm-thumb-gen.c)

Modify the `TCCIR_OP_MLA` case to handle DEREF operands by loading them to scratch registers before emitting MLA:

1. Check if `src1.is_lval` or `src1.pr0_reg == PREG_REG_NONE` → load to scratch
2. Check if `src2.is_lval` or `src2.pr0_reg == PREG_REG_NONE` → load to scratch
3. Emit MLA with the loaded registers
4. Restore scratch registers

### Step 3: Preserve DEREF Flags During Fusion

Ensure that when we transform MUL to MLA, the DEREF flags on src1/src2 operands are preserved so codegen knows to load them:

```c
/* In tcc_ir_opt_mla_fusion(): */
/* Transform MUL + ADD into MLA */
/* 1. Change MUL opcode to MLA - operands including DEREF flags preserved */
mul_q->op = TCCIR_OP_MLA;
/* src1, src2 operands (at operand_base+1, +2) keep their DEREF flags */
```

## Expected Results

### Before (Current TCC -O1)
```asm
dot_product:
  ...
  ldr.w   ip, [r6]         ; 4 bytes
  ldr.w   lr, [r5]         ; 4 bytes
  mul.w   r8, ip, lr       ; 4 bytes
  add     r3, r8           ; 2 bytes
  ...
```
**Loop body: 14 bytes**

### After (With MLA+DEREF)
```asm
dot_product:
  ...
  ldr.w   ip, [r6]         ; 4 bytes
  ldr.w   lr, [r5]         ; 4 bytes
  mla     r3, ip, lr, r3   ; 4 bytes
  ...
```
**Loop body: 12 bytes** (saves 2 bytes per iteration)

## Combined Optimization: MLA + Post-Increment

The ultimate optimization would combine MLA with post-increment addressing:

### GCC -O1 Output (Gold Standard)
```asm
ldr     ip, [r3, #4]!     ; load with pre-increment: 4 bytes
ldr     lr, [r1, #4]!     ; load with pre-increment: 4 bytes
mla     r0, lr, ip, r0    ; multiply-accumulate: 4 bytes
```
**Loop body: 12 bytes** (same as basic MLA, but also advances pointers)

This would require:
1. Detect pattern: `sum += a[i] * b[i]; i++`
2. Convert to: `sum += *pa++ * *pb++` (pointer-based iteration)
3. Apply post-increment fusion to both loads
4. Apply MLA fusion to the multiply-accumulate

**This is a more complex optimization and should be a separate enhancement.**

## Test Cases

### Test 1: Basic MLA with DEREF
```c
int test_mla_deref(int *a, int *b, int acc) {
    return acc + (*a) * (*b);
}
```
Expected IR: `MLA` with two DEREF operands
Expected ASM: 2 LDR + 1 MLA (not 2 LDR + MUL + ADD)

### Test 2: Loop with Array Access
```c
int test_dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += a[i] * b[i];
    return sum;
}
```
Expected: MLA in loop body

### Test 3: Mixed DEREF and Register
```c
int test_mixed(int *a, int b, int acc) {
    return acc + (*a) * b;  // Only one DEREF
}
```
Expected: MLA with one DEREF operand

## Implementation Checklist

- [ ] **ir/opt.c**: Remove DEREF check in `tcc_ir_opt_mla_fusion()`
- [ ] **arm-thumb-gen.c**: Update `TCCIR_OP_MLA` case to load DEREF operands to scratch registers
- [ ] **Tests**: Add test cases for MLA with dereferences
- [ ] **Verify**: Run `make test -j16` to ensure no regressions
- [ ] **Benchmark**: Run comparison script to measure improvement

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Register pressure increase | Low | Medium | Scratch regs already available in MLA handler |
| Incorrect codegen for DEREF | Medium | High | Test thoroughly with existing test suite |
| Performance regression | Low | Low | MLA is strictly better than MUL+ADD |
| Breaking existing MLA cases | Low | High | Keep existing MLA tests, add new ones |

## Timeline Estimate

- **Step 1 (IR opt change):** 15 minutes
- **Step 2 (Codegen change):** 30 minutes
- **Step 3 (Testing):** 20 minutes
- **Total:** ~1 hour

## References

- ARM MLA instruction: `MLA{S}{cond} Rd, Rn, Rm, Ra` → `Rd = Rn * Rm + Ra`
- Current MLA fusion: `ir/opt.c` lines 2900-3165
- MLA codegen: `arm-thumb-gen.c` lines 4553-4605
- MUL with DEREF handling: `thumb_emit_regonly_binop32()` lines 4086-4131
