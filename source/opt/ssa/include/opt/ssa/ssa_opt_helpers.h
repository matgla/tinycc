/*
 *  TCC SSA opt - generic operand/immediate helpers shared across SSA passes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_HELPERS_H
#define TCC_OPT_SSA_HELPERS_H

#include <stdint.h>

/* Requires ir.h (IROperand, IROP_TAG_IMM32) to be included first. */

/* Non-lval inline 32-bit immediate: tag is IMM32 specifically (not I64/F32/F64)
 * and not an lval.  Narrower than the DSL IR_CONSTRAINT_IMM, which also matches
 * those wider tags — use this when a rewrite reads the raw .u.imm32 field. */
static inline int is_imm32(IROperand op)
{
  return op.tag == IROP_TAG_IMM32 && !op.is_lval;
}

/* Operand is a 64-bit int or double — masks/shifts assuming 32-bit width must bail. */
static inline int is_i64f64(IROperand op)
{
  return op.btype == IROP_BTYPE_INT64 || op.btype == IROP_BTYPE_FLOAT64;
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
static inline int is_power_of_2(uint32_t v, int *shift)
{
  if (v == 0 || (v & (v - 1)) != 0)
    return 0;
  int s = 0;
  while ((v >> s) != 1)
    s++;
  *shift = s;
  return 1;
}

#endif /* TCC_OPT_SSA_HELPERS_H */
