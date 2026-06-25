/*
 *  TCC IR - SSA Strength Reduction
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"

/* ============================================================================
 * Strength Reduction: replace expensive operations with cheaper equivalents.
 *
 *   MUL x, #(2^n)  →  SHL x, #n
 *   UDIV x, #(2^n) →  SHR x, #n
 *   UMOD x, #(2^n) →  AND x, #(2^n - 1)
 *   MUL x, #(2^n + 1)  →  ADD x, (SHL x, #n)  [deferred — needs extra temp]
 *   MUL x, #(2^n - 1)  →  SUB (SHL x, #n), x  [deferred — needs extra temp]
 *
 * Only the single-instruction rewrites are done here. Multi-instruction
 * patterns are left to the pre-SSA strength reduction pass.
 * ============================================================================ */

static int is_power_of_2(uint32_t v, int *shift)
{
  if (v == 0 || (v & (v - 1)) != 0)
    return 0;
  int s = 0;
  while ((v >> s) != 1)
    s++;
  *shift = s;
  return 1;
}

static int gen_mul_strength(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  /* Find the immediate operand and the non-immediate operand */
  IROperand imm_op, val_op;
  int swapped = 0;
  if (src2.tag == IROP_TAG_IMM32 && !src2.is_lval) {
    imm_op = src2;
    val_op = src1;
  } else if (src1.tag == IROP_TAG_IMM32 && !src1.is_lval) {
    imm_op = src1;
    val_op = src2;
    swapped = 1;
  } else {
    return 0;
  }

  uint32_t c = (uint32_t)imm_op.u.imm32;
  int shift;

  if (!is_power_of_2(c, &shift))
    return 0;

  /* MUL x, 2^n → SHL x, n */
  IROperand shift_imm = irop_make_imm32(0, shift, IROP_BTYPE_INT32);
  q->op = TCCIR_OP_SHL;
  tcc_ir_op_set_src1(ir, q, val_op);
  tcc_ir_op_set_src2(ir, q, shift_imm);

  /* Update use-def: remove use of immediate (no vreg) */
  if (swapped) {
    /* src1 was imm, src2 was val — val is now src1, shift is src2 */
    /* No use-def change needed: imm has no vreg, val stays */
  }

  return 1;
}

static int gen_udiv_strength(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;

  uint32_t c = (uint32_t)src2.u.imm32;
  int shift;
  if (!is_power_of_2(c, &shift))
    return 0;

  /* UDIV x, 2^n → SHR x, n */
  IROperand shift_imm = irop_make_imm32(0, shift, IROP_BTYPE_INT32);
  q->op = TCCIR_OP_SHR;
  tcc_ir_op_set_src2(ir, q, shift_imm);
  return 1;
}

static int gen_umod_strength(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;

  uint32_t c = (uint32_t)src2.u.imm32;
  int shift;
  if (!is_power_of_2(c, &shift))
    return 0;

  /* UMOD x, 2^n → AND x, 2^n - 1 */
  IROperand mask = irop_make_imm32(0, (int32_t)(c - 1), IROP_BTYPE_INT32);
  q->op = TCCIR_OP_AND;
  tcc_ir_op_set_src2(ir, q, mask);
  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen strength_gens[] = {
  { TCCIR_OP_MUL,  gen_mul_strength,  "sr_mul" },
  { TCCIR_OP_UDIV, gen_udiv_strength, "sr_udiv" },
  { TCCIR_OP_UMOD, gen_umod_strength, "sr_umod" },
};

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_strength(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, strength_gens,
                          sizeof(strength_gens) / sizeof(strength_gens[0]));
}
