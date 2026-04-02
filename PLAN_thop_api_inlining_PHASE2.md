# Phase 2 Detailed Plan: Eliminate Function-Pointer Dispatch in `arm-thumb-gen.c`

## Document Status
This is the actionable implementation plan for **Phase 2** of `PLAN_thop_api_inlining.md`. It depends on **Phase 1** (tables+wrappers in headers) being complete.

---

## 1. Objective

Replace all indirect function calls via `thumb_imm_handler_t`, `thumb_reg_handler_t`, `thumb_regonly3_handler_t`, and `ThumbDataProcessingHandler` with direct calls or `switch (op)` dispatches. After Phase 1 made the `th_*` wrappers inlineable, these function pointers are the **last remaining barrier** that prevents the compiler from inlining the entire instruction-emit path.

### Success Criteria
- Zero occurrences of `thumb_imm_handler_t`, `thumb_reg_handler_t`, `ThumbDataProcessingHandler` in `arm-thumb-gen.c`.
- `thumb_call_reg_handler` is deleted.
- All call sites emit direct calls (`bl th_add_reg`, `bl th_add_imm`, etc.) or have them inlined.
- The `thumb_shift` struct-passing ABI bug (§2.2) no longer manifests — verified by the shift-related IR tests.
- All existing tests pass (`make test -j16`, `make test-asm -j16`).

---

## 2. Problem Analysis

### 2.1. Current Dispatch Chains

Even after Phase 1 inlines the `th_*` wrappers, the compiler still sees this in `arm-thumb-gen.c`:

```c
ThumbDataProcessingHandler handler;
handler.imm_handler = th_add_imm;   // function pointer store
handler.reg_handler = th_add_reg;   // function pointer store
...
thumb_emit_data_processing_mop32(..., handler, flags);
```

Inside `thumb_emit_data_processing_mop32`:
```c
handler.imm_handler(rd, rn, imm, flags, enc);   // indirect call → blocks inlining
thumb_call_reg_handler(handler.reg_handler, ...); // indirect call → blocks inlining
```

The compiler **cannot** de-virtualize these calls because `handler` is a mutable local struct. Even when the assignments are right above the call, GCC/Clang without LTO will emit indirect calls via the stored pointers.

### 2.2. Why `thumb_call_reg_handler` Exists

Lines 4117-4147 already contain a workaround:
```c
static thumb_opcode thumb_call_reg_handler(thumb_reg_handler_t fn, ...)
{
  if (fn == th_add_reg) return th_add_reg(...);
  if (fn == th_sub_reg) return th_sub_reg(...);
  ...
  return fn(...); // fallback indirect call
}
```

This was added to fix a **struct-passing bug** (`thumb_shift` is 12 bytes, `thumb_opcode` is 8 bytes) when passing through indirect calls with `sret`. By comparing the function pointer and branching to a direct call, the cross-compiler generates correct ABI code.

**Phase 2 makes this workaround unnecessary:** if we never use function pointers, we never hit the bug.

### 2.3. Inventory of Function-Pointer Types

| Type | Line | Used By | Call Sites |
|------|------|---------|-----------|
| `thumb_imm_handler_t` | 343 | `ThumbDataProcessingHandler`, `mach_ensure_imm_or_reg`, `thumb_emit_shift64_mop` | `thumb_emit_op_imm_fallback`, `thumb_emit_data_processing_mop32`, `thumb_emit_data_processing_mop64`, `thumb_emit_shift64_mop` |
| `thumb_reg_handler_t` | 4102 | `ThumbDataProcessingHandler`, `thumb_call_reg_handler` | `thumb_emit_data_processing_mop32`, `thumb_emit_data_processing_mop64` |
| `thumb_regonly3_handler_t` | 4198 | `mach_regonly_binop_mop`, `mach_mod_mop` | `tcc_gen_machine_mul_div_mod_mop` (MUL, SDIV, UDIV, IMOD, UMOD) |
| `thumb_longmul_handler_t` | 4215 | **Dead code** — defined but never assigned or called. | None |

---

## 3. Step-by-Step Implementation

### 3.1. Remove Dead Code

**File:** `arm-thumb-gen.c`

Delete the unused typedef and any associated comments:
```c
// Line 4215 — delete
typedef thumb_opcode (*thumb_longmul_handler_t)(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);
```

### 3.2. Replace `ThumbDataProcessingHandler` with `TccIrOp` Dispatch

This is the largest change. We eliminate the struct and pass the IR opcode down the call stack. Every former call site of `handler.imm_handler` or `handler.reg_handler` becomes a `switch (op)`.

#### 3.2.1. Delete Types and Workaround

Delete lines 4102-4147:
```c
typedef thumb_opcode (*thumb_reg_handler_t)(...);
typedef struct ThumbDataProcessingHandler { ... } ThumbDataProcessingHandler;
static thumb_opcode thumb_call_reg_handler(...) { ... }
```

Also delete `thumb_imm_handler_t` at line 343.

#### 3.2.2. Replace `thumb_emit_op_imm_fallback`

**Current signature (line 4175):**
```c
static void thumb_emit_op_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags,
                                       ThumbDataProcessingHandler handler)
```

**New signature:**
```c
static void thumb_emit_op_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags, TccIrOp op)
```

**New body:**
```c
{
  thumb_opcode sub_low;
  switch (op) {
  case TCCIR_OP_ADD: sub_low = th_add_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_SUB: sub_low = th_sub_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN: sub_low = th_adc_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_AND: sub_low = th_and_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_OR:  sub_low = th_orr_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_XOR: sub_low = th_eor_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_CMP: sub_low = th_cmp_imm_handler(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_SHL: sub_low = th_lsl_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_SHR: sub_low = th_lsr_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  case TCCIR_OP_SAR: sub_low = th_asr_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE); break;
  default: tcc_error("compiler_error: thumb_emit_op_imm_fallback: unhandled op %d", (int)op); return;
  }

  if (sub_low.size == 0) { ... /* fallback to scratch + reg form */ ... }
  else { ot_check(sub_low); }
}
```

> **Note:** The fallback path currently calls `thumb_call_reg_handler(handler.reg_handler, ...)`. After the change, it calls the reg form directly via another `switch (op)`.

#### 3.2.3. Replace `thumb_emit_data_processing_mop64`

**Current signature (line 4355):**
```c
static void thumb_emit_data_processing_mop64(..., TccIrOp op,
                                             ThumbDataProcessingHandler regular,
                                             ThumbDataProcessingHandler carry_h, bool uses_carry)
```

**New signature:**
```c
static void thumb_emit_data_processing_mop64(..., TccIrOp op, bool uses_carry)
```

**Changes inside the function:**

1. **Immediate fallback calls (lines 4435-4436):**
   ```c
   thumb_emit_op_imm_fallback(rd_lo, rn_lo, imm_lo, lo_flags, op);
   // For high word: if uses_carry, dispatch to carry variant; else same op
   thumb_emit_op_imm_fallback(rd_hi, rn_hi, imm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              uses_carry ? carry_op_for(op) : op);
   ```
   **Carry dispatch:** There is no `TCCIR_OP_SBC_GEN` or `TCCIR_OP_SBC_USE` in `TccIrOp` (verified). The carry mapping is: ADD→`th_adc_imm`/`th_adc_reg`, SUB→`th_sbc_imm`/`th_sbc_reg`. Since there's no IR-level SBC opcode, the carry dispatch cannot use a simple `carry_op_for(op)` helper. Instead, use explicit if/else in the high-word path:
   ```c
   // High word
   if (op == TCCIR_OP_ADD)
     thumb_emit_op_imm_fallback(rd_hi, rn_hi, imm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, TCCIR_OP_ADC_GEN);
   else if (op == TCCIR_OP_SUB)
     // Call th_sbc_imm directly since there's no TCCIR_OP_SBC_*:
     { thumb_opcode r = th_sbc_imm(rd_hi, rn_hi, imm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
       if (!r.size) /* fallback to reg form via th_sbc_reg */ ...
       else ot_check(r); }
   else
     thumb_emit_op_imm_fallback(rd_hi, rn_hi, imm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, op);
   ```
   This avoids inventing a non-existent IR opcode.

2. **Reg-handler calls (lines 4456-4459):**
   Replace `thumb_call_reg_handler(regular.reg_handler, ...)` and `thumb_call_reg_handler(carry_h.reg_handler, ...)` with direct `switch (op)` dispatch:
   ```c
   // Low word (regular)
   switch (op) {
   case TCCIR_OP_ADD: ot_check(th_add_reg(rd_lo, rn_lo, rm_lo, lo_flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_SUB: ot_check(th_sub_reg(rd_lo, rn_lo, rm_lo, lo_flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   ...
   }

   // High word (carry)
   if (uses_carry) {
     switch (op) {
     case TCCIR_OP_ADD: ot_check(th_adc_reg(rd_hi, rn_hi, rm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
     case TCCIR_OP_SUB: ot_check(th_sbc_reg(rd_hi, rn_hi, rm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
     }
   } else {
     // Same as low word
     switch (op) { ... }
   }
   ```

#### 3.2.4. Replace `thumb_emit_data_processing_mop32`

**Current signature (line 4745):**
```c
static void thumb_emit_data_processing_mop32(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op,
                                             ThumbDataProcessingHandler handler,
                                             thumb_flags_behaviour flags)
```

**New signature:**
```c
static void thumb_emit_data_processing_mop32(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op,
                                             thumb_flags_behaviour flags)
```

**Changes inside the function:**

1. **`mach_ensure_imm_or_reg` call (line 4771):**
   Currently passes `handler.imm_handler`. Instead, create a thin wrapper that dispatches on `op`:
   ```c
   static inline thumb_opcode emit_alu_imm_for_op(TccIrOp op, int rd, int rn, uint32_t imm,
                                                   thumb_flags_behaviour flags, thumb_enforce_encoding enc)
   {
     switch (op) {
     case TCCIR_OP_ADD: return th_add_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_SUB: return th_sub_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_ADC_USE:
     case TCCIR_OP_ADC_GEN: return th_adc_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_AND: return th_and_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_OR:  return th_orr_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_XOR: return th_eor_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_CMP: return th_cmp_imm_handler(rd, rn, imm, flags, enc);
     case TCCIR_OP_SHL: return th_lsl_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_SHR: return th_lsr_imm(rd, rn, imm, flags, enc);
     case TCCIR_OP_SAR: return th_asr_imm(rd, rn, imm, flags, enc);
     default: tcc_error("compiler_error: emit_alu_imm_for_op: unhandled op %d", (int)op);
              return (thumb_opcode){0,0};
     }
   }
   ```
   Then:
   ```c
   int src2_reg = mach_ensure_imm_or_reg_for_op(&mctx, src2, excl, op, dest_reg, src1_reg, flags, &imm_emitted);
   ```
   Where `mach_ensure_imm_or_reg_for_op` is `mach_ensure_imm_or_reg` with the `op` parameter threaded through.

2. **Reg-handler call (line 4775):**
   Replace `thumb_call_reg_handler(handler.reg_handler, ...)` with:
   ```c
   switch (op) {
   case TCCIR_OP_ADD: ot_check(th_add_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_SUB: ot_check(th_sub_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_ADC_USE:
   case TCCIR_OP_ADC_GEN: ot_check(th_adc_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_AND: ot_check(th_and_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_OR:  ot_check(th_orr_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_XOR: ot_check(th_eor_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_CMP: ot_check(th_cmp_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_SHL: ot_check(th_lsl_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_SHR: ot_check(th_lsr_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   case TCCIR_OP_SAR: ot_check(th_asr_reg(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)); break;
   default: tcc_error("..."); break;
   }
   ```

#### 3.2.5. Rewrite `tcc_gen_machine_data_processing_mop`

**Current code (lines 4798-4880):**
Builds `handler` and `carry_handler` structs in a `switch (op)`, then passes them to `thumb_emit_data_processing_mop32` / `mop64`.

**New code:**
```c
void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  bool uses_carry = false;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;

  switch (op) {
  case TCCIR_OP_ADD: uses_carry = true; break;
  case TCCIR_OP_SUB: uses_carry = true; break;
  case TCCIR_OP_ADC_GEN: flags = FLAGS_BEHAVIOUR_SET; break;
  case TCCIR_OP_CMP: flags = FLAGS_BEHAVIOUR_SET; break;
  default: break;
  }

  if (dest.is_64bit) {
    if (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR)
      thumb_emit_shift64_mop(&src1, &src2, &dest, op);
    else
      thumb_emit_data_processing_mop64(&src1, &src2, &dest, op, uses_carry);
    return;
  }

  thumb_emit_data_processing_mop32(&src1, &src2, &dest, op, flags);
}
```

The inner `switch (op)` that assigned function pointers is **completely eliminated**.

### 3.3. Replace `thumb_regonly3_handler_t` Dispatch

**Current helper (line 4918):**
```c
static void mach_regonly_binop_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                                   const MachineOperand *dest, thumb_regonly3_handler_t emitter)
{
  ...
  ot_check(emitter(dest_reg, src1_reg, src2_reg));
  ...
}
```

**Approach:** Keep the `mach_regonly_binop_mop` helper (it handles register allocation boilerplate) but replace the function-pointer parameter with an enum and a direct `switch` for the single emitter call. This avoids duplicating 7 lines of reg-allocation logic at 3 call sites.

```c
typedef enum { REGONLY_MUL, REGONLY_SDIV, REGONLY_UDIV } RegonlyOp;

static void mach_regonly_binop_mop(MachineCodegenContext *ctx, const MachineOperand *src1,
                                   const MachineOperand *src2, const MachineOperand *dest,
                                   RegonlyOp regonly_op)
{
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
  int src1_reg = mach_ensure_in_reg(ctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg)) excl |= (1u << (uint32_t)src1_reg);
  int src2_reg = mach_ensure_in_reg(ctx, src2, excl);

  switch (regonly_op) {
  case REGONLY_MUL:  ot_check(th_mul(dest_reg, src1_reg, src2_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)); break;
  case REGONLY_SDIV: ot_check(th_sdiv(dest_reg, src1_reg, src2_reg)); break;
  case REGONLY_UDIV: ot_check(th_udiv(dest_reg, src1_reg, src2_reg)); break;
  }
  mach_writeback_dest(dest, dest_reg);
}
```

Similarly update `mach_mod_mop` to take `RegonlyOp` instead of `thumb_regonly3_handler_t`.

**Delete:**
- `thumb_regonly3_handler_t` typedef (line 4198)
- `thumb_mul_regonly`, `thumb_sdiv_regonly`, `thumb_udiv_regonly` wrappers (lines 4200-4213)

### 3.4. Replace `thumb_imm_handler_t` in `thumb_emit_shift64_mop`

**Current code (lines 4499-4517):**
```c
thumb_imm_handler_t dst_lo_shift, dst_hi_shift, cross_shift;
if (is_left)       { dst_lo_shift = th_lsl_imm; dst_hi_shift = th_lsl_imm; cross_shift = th_lsr_imm; }
else if (arith_right) { dst_lo_shift = th_lsr_imm; dst_hi_shift = th_asr_imm; cross_shift = th_lsl_imm; }
else               { dst_lo_shift = th_lsr_imm; dst_hi_shift = th_lsr_imm; cross_shift = th_lsl_imm; }
```

**Replace all `dst_lo_shift(...)`, `dst_hi_shift(...)`, `cross_shift(...)` call sites with direct conditional calls.**

There are ~6 call sites inside `thumb_emit_shift64_mop` (lines 4599, 4605, 4607, 4613, 4623, 4625, etc.). Each becomes:

```c
// cross_shift(tmp.reg, src_lo, 32 - sh, ...)
if (is_left)
  ot_check(th_lsr_imm(tmp.reg, src_lo, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
else
  ot_check(th_lsl_imm(tmp.reg, src_lo, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

// dst_lo_shift(dst_lo, src_lo, sh, ...)
if (is_left)
  ot_check(th_lsl_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
else
  ot_check(th_lsr_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

// dst_hi_shift(dst_hi, src_hi, sh, ...)
if (is_left)
  ot_check(th_lsl_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
else if (arith_right)
  ot_check(th_asr_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
else
  ot_check(th_lsr_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
```

Then delete `thumb_imm_handler_t` typedef (line 343).

### 3.5. Update `mach_ensure_imm_or_reg`

**Current signature (line 525):**
```c
static int mach_ensure_imm_or_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl,
                                  thumb_imm_handler_t imm_handler, int dest_reg, int src1_reg,
                                  thumb_flags_behaviour flags, bool *imm_emitted)
```

**New signature:**
```c
static int mach_ensure_imm_or_reg_for_op(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl,
                                         TccIrOp alu_op, int dest_reg, int src1_reg,
                                         thumb_flags_behaviour flags, bool *imm_emitted)
```

**New body:**
Replace the single indirect call `imm_handler(dest_reg, src1_reg, imm_val, flags, ENFORCE_ENCODING_NONE)` with a call to the new `emit_alu_imm_for_op(alu_op, dest_reg, src1_reg, imm_val, flags, ENFORCE_ENCODING_NONE)` helper defined in §3.2.4.

---

## 4. Call-Site Summary Table

| Function | Old Indirect Call | New Direct Call / Switch |
|----------|------------------|--------------------------|
| `thumb_emit_op_imm_fallback` | `handler.imm_handler(...)` | `switch(op)` → `th_add_imm`/`th_sub_imm`/etc |
| `thumb_emit_op_imm_fallback` | `thumb_call_reg_handler(handler.reg_handler,...)` | `switch(op)` → `th_add_reg`/`th_sub_reg`/etc |
| `thumb_emit_data_processing_mop64` | `thumb_emit_op_imm_fallback(..., regular)` | `thumb_emit_op_imm_fallback(..., op)` |
| `thumb_emit_data_processing_mop64` | `thumb_emit_op_imm_fallback(..., carry_h)` | `thumb_emit_op_imm_fallback(..., carry_op_for(op))` |
| `thumb_emit_data_processing_mop64` | `thumb_call_reg_handler(regular.reg_handler,...)` | `switch(op)` → direct reg call |
| `thumb_emit_data_processing_mop64` | `thumb_call_reg_handler(carry_h.reg_handler,...)` | `switch(op)` → direct carry-reg call |
| `thumb_emit_data_processing_mop32` | `mach_ensure_imm_or_reg(..., handler.imm_handler,...)` | `mach_ensure_imm_or_reg_for_op(..., op,...)` |
| `thumb_emit_data_processing_mop32` | `thumb_call_reg_handler(handler.reg_handler,...)` | `switch(op)` → direct reg call |
| `thumb_emit_shift64_mop` | `cross_shift(...)` | `if (is_left) th_lsr_imm(...) else th_lsl_imm(...)` |
| `thumb_emit_shift64_mop` | `dst_lo_shift(...)` | `if (is_left) th_lsl_imm(...) else th_lsr_imm(...)` |
| `thumb_emit_shift64_mop` | `dst_hi_shift(...)` | `if/else` chain → `th_lsl_imm`/`th_asr_imm`/`th_lsr_imm` |
| `mach_regonly_binop_mop` | `emitter(...)` | Inlined: `th_mul`/`th_sdiv`/`th_udiv` |
| `mach_mod_mop` | `div_emitter(...)` | Inlined: `th_sdiv`/`th_udiv` |

---

## 5. Helper Function Design

To avoid massive code duplication, introduce these helpers in `arm-thumb-gen.c` (near the deleted `thumb_call_reg_handler`).

**Important:** Mark these as `static` (not `static inline` or `always_inline`). Since `op` is a runtime variable, each helper is an 11-case switch. If forced-inline, the switch body (with each case containing an inlined `th_*` wrapper) would be duplicated at every call site — potentially 11 × ~200 bytes × 4 call sites = ~8.8KB. Let the compiler decide whether to inline based on call-site context.

```c
static thumb_opcode emit_alu_imm_for_op(TccIrOp op, int rd, int rn, uint32_t imm,
                                        thumb_flags_behaviour flags, thumb_enforce_encoding enc)
{
  switch (op) {
  case TCCIR_OP_ADD: return th_add_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_SUB: return th_sub_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN: return th_adc_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_AND: return th_and_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_OR:  return th_orr_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_XOR: return th_eor_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_CMP: return th_cmp_imm_handler(rd, rn, imm, flags, enc);
  case TCCIR_OP_SHL: return th_lsl_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_SHR: return th_lsr_imm(rd, rn, imm, flags, enc);
  case TCCIR_OP_SAR: return th_asr_imm(rd, rn, imm, flags, enc);
  default:
    tcc_error("compiler_error: emit_alu_imm_for_op: unhandled op %d", (int)op);
    return (thumb_opcode){0, 0};
  }
}

static thumb_opcode emit_alu_reg_for_op(TccIrOp op, int rd, int rn, int rm,
                                       thumb_flags_behaviour flags, thumb_shift shift,
                                       thumb_enforce_encoding enc)
{
  switch (op) {
  case TCCIR_OP_ADD: return th_add_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_SUB: return th_sub_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN: return th_adc_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_AND: return th_and_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_OR:  return th_orr_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_XOR: return th_eor_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_CMP: return th_cmp_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_SHL: return th_lsl_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_SHR: return th_lsr_reg(rd, rn, rm, flags, shift, enc);
  case TCCIR_OP_SAR: return th_asr_reg(rd, rn, rm, flags, shift, enc);
  default:
    tcc_error("compiler_error: emit_alu_reg_for_op: unhandled op %d", (int)op);
    return (thumb_opcode){0, 0};
  }
}

```

**Note on carry dispatch:** There is no `TCCIR_OP_SBC_GEN` in `TccIrOp`. The carry mapping (ADD→ADC, SUB→SBC) must be handled by calling `th_adc_*`/`th_sbc_*` directly rather than through `emit_alu_*_for_op`. See §3.2.3 for the carry dispatch pattern.

These helpers consolidate the `switch` logic. Because they are `static` and call `static inline` `th_*` functions (from Phase 1), the compiler can choose to inline the chain into direct calls at each call site when profitable.

---

## 6. Build & Verification

### 6.1. Incremental Build
```bash
make cross -j$(nproc)
```

### 6.2. Compile-Time Inspection
```bash
$(CC) -O2 -S -o /tmp/atg.s arm-thumb-gen.c $(DEFINES) $(CFLAGS) -I. -Iir -fverbose-asm

# Verify no indirect calls to thop functions remain
grep -E 'bl\s+\*' /tmp/atg.s | grep -E 'th_(add|sub|adc|sbc|and|orr|eor|cmp|lsl|lsr|asr|mov|mul|div)_' && echo "FAIL" || echo "OK"
```

### 6.3. Test Suite
```bash
make test -j16
make test-asm -j16
make test-legacy -j16
```

### 6.4. Size Check
```bash
size bin/armv8m-tcc
# Expect text segment to decrease slightly (no thumb_call_reg_handler blob)
# or stay flat (switches are small).
```

---

## 7. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **Code size increase** from duplicated `switch` bodies | Low | Low | The `switch` bodies are small (~12 cases, 2 instructions each). `emit_alu_imm_for_op` / `emit_alu_reg_for_op` are `static` (compiler-decided inlining), not forced inline. |
| **Missed opcode in switch** | Medium | High | Add a `default: tcc_error(...)` case in every switch. The compiler will warn if `TccIrOp` values are not exhaustively covered (with `-Wswitch`). |
| **Regressed struct-passing bug** on cross-compiler | Low | High | The bug only manifests when passing `thumb_shift` through **indirect** calls with `sret`. Phase 2 removes all indirect calls. Add targeted IR tests that exercise shift-related codegen paths (e.g., `a << 3`, `a >> 1`, `a + (b << 2)`) to verify no regression. |
| **Broken carry handling** in 64-bit ADD/SUB | Medium | High | No `TCCIR_OP_SBC_*` exists — carry-SUB must dispatch to `th_sbc_*` directly, not through `emit_alu_*_for_op`. Easy to get wrong. Add targeted IR test `tests/ir_tests/64bit_add_sub.c` covering: `int64_t` add, sub, add-with-overflow, sub-with-borrow. |
| **Build failure on older GCC** | Low | Medium | `static inline` is C99. TCC already requires C11. No issue. |

---

## 8. Post-Phase-2 State

After Phase 2, the call chain from `tcc_gen_machine_data_processing_mop` to the opcode encoding is **fully direct**:

```
tcc_gen_machine_data_processing_mop
  └── thumb_emit_data_processing_mop32
        ├── mach_ensure_imm_or_reg_for_op
        │     └── emit_alu_imm_for_op
        │           └── th_add_imm  ← inlined (Phase 1)
        │                 └── thop_emit ← unrolled (Phase 1 + 3)
        └── emit_alu_reg_for_op
              └── th_add_reg  ← inlined (Phase 1)
                    └── thop_emit ← unrolled (Phase 1 + 3)
```

There are **zero function pointers** in the hot path. The compiler can now:
1. Inline `th_add_imm` / `th_add_reg` (Phase 1 enabler)
2. Inline `emit_alu_imm_for_op` / `emit_alu_reg_for_op` (Phase 2)
3. Unroll `thop_emit` loop with constant-folded shape fields (Phase 1)
4. Potentially constant-fold feature checks if `target_feat` becomes compile-time known (Phase 3)

---

## 9. Estimation

| Task | Estimated Effort |
|------|-----------------|
| Delete dead types + `thumb_call_reg_handler` | 15 min |
| Rewrite `thumb_emit_op_imm_fallback` | 30 min |
| Rewrite `thumb_emit_data_processing_mop64` (incl. carry dispatch without SBC opcode) | 1 hr |
| Rewrite `thumb_emit_data_processing_mop32` + `mach_ensure_imm_or_reg` | 45 min |
| Rewrite `tcc_gen_machine_data_processing_mop` | 15 min |
| Rewrite `thumb_emit_shift64_mop` | 45 min |
| Refactor `mach_regonly_binop_mop` / `mach_mod_mop` to use `RegonlyOp` enum | 30 min |
| Add targeted IR tests for 64-bit carry + shift paths | 45 min |
| Build fixes + testing | 2 hrs |
| **Total** | **~7 hrs** |

---

## 10. Related Documents

- `PLAN_thop_api_inlining.md` — Parent plan
- `PLAN_thop_api_inlining_PHASE1.md` — Phase 1 (prerequisite)
- `THOP_GENERIC_DESIGN.md` — `thop_emit` design
