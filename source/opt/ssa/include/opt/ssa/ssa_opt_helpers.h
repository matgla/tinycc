/*
 *  TCC SSA opt - generic operand/immediate helpers shared across SSA passes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

/* Requires ir.h (IROperand, IROP_TAG_IMM32) to be included first. */

/* Non-lval IMM32 (not I64/F32/F64, unlike the DSL's BIND_IMM): safe to read .u.imm32. */
static inline int is_imm32(IROperand op)
{
  return op.tag == IROP_TAG_IMM32 && !op.is_lval;
}

/* Ops that may clobber arbitrary memory / re-enter control flow, invalidating a
 * backward value-forwarding scan: calls (__builtin_apply included), inline asm
 * and its operand markers, VLA growth and SP save/restore, the setjmp family. */
static inline int ssa_op_is_call_barrier(int op)
{
  return ir_op_has(op, IR_HZ_FROM_OP & ~(IR_HZ_MEM_READ | IR_HZ_MEM_WRITE | IR_HZ_UPDATES_SRC | IR_HZ_FLAGS_SET |
                                          IR_HZ_FLAGS_READ | IR_HZ_BRANCH | IR_HZ_RETURN | IR_HZ_TRAP | IR_HZ_HINT |
                                          IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_CHAIN));
}

/* The operand-carrier ops of an inline-asm block; some scans treat them as
 * barriers in addition to ssa_op_is_call_barrier's INLINE_ASM. */
static inline int ssa_op_is_asm_operand(int op)
{
  return op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT;
}

/* Operand is a 64-bit int or double — masks/shifts assuming 32-bit width must bail. */
static inline int is_i64f64(IROperand op)
{
  return op.btype == IROP_BTYPE_INT64 || op.btype == IROP_BTYPE_FLOAT64;
}

/* Does instruction `q` read vreg `vr` as a value source (src1, src2, or MLA
 * accumulator)?  Ignores the dest, so a def of `vr` is not a read. */
static inline int ssa_op_reads_vreg(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  if (irop_config[q->op].has_src1 &&
      tcc_ir_op_src1_vreg(ir, q) == vr)
    return 1;
  if (irop_config[q->op].has_src2 &&
      tcc_ir_op_src2_vreg(ir, q) == vr)
    return 1;
  if (q->op == TCCIR_OP_MLA &&
      tcc_ir_op_accum_vreg(ir, q) == vr)
    return 1;
  return 0;
}

/* Operand carries a TEMP vreg — a single-def SSA value. */
static inline int is_temp_vreg(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  return vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* Non-lval operand carrying any vreg (TEMP/VAR/PARAM) — a foldable dest. */
static inline int is_value_dest(IROperand op)
{
  return irop_get_vreg(op) >= 0 && !op.is_lval;
}

/* v == 2^s: returns 1 and writes *shift on success, 0 otherwise (v==0 or
 * non-power-of-two). */
static inline int is_power_of_2_shift(uint32_t v, int *shift)
{
  if (v == 0 || (v & (v - 1)) != 0)
    return 0;
  int s = 0;
  while ((v >> s) != 1)
    s++;
  *shift = s;
  return 1;
}

/* Build an immediate operand with the same width as `use` (for forwarding a
 * constant into a use site that may differ in btype from the defining ASSIGN). */
static inline IROperand ssa_cprop_imm_for_use(IROperand imm, IROperand use)
{
  IROperand out = imm;
  out.is_lval = 0;
  out.is_llocal = 0;
  out.is_local = 0;
  out.btype = use.btype;
  out.is_unsigned = use.is_unsigned;
  out.is_static = use.is_static;
  return out;
}

/* True when the *other* operand of `q` is a constant (or the same vreg as
 * `old_vr`). Used to gate VAR → immediate forwarding in CMP/BOOL_AND/BOOL_OR
 * where the other side must also be foldable. */
static inline int ssa_cprop_imm_other_operand_const(TCCIRState *ir, IRQuadCompact *q,
                                                     int32_t old_vr, int src_slot)
{
  IROperand other = tcc_ir_op_get_src1_or_2(ir, q, src_slot == 1);
  if (irop_get_vreg(other) == old_vr)
    return 1;
  return irop_is_immediate(other) && !other.is_lval;
}

