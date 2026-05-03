/*
 *  TCC IR - SSA Reassociation
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
 * Reassociation: reorder associative/commutative operations so that constant
 * operands bubble together, enabling the fold pass to collapse them.
 *
 *   (x + c1) + c2  →  x + (c1 + c2)   [fold handles the c1+c2 part]
 *   (x + c1) - c2  →  x + (c1 - c2)
 *   (x * c1) * c2  →  x * (c1 * c2)
 *   (x & c1) & c2  →  x & (c1 & c2)
 *   (x | c1) | c2  →  x | (c1 | c2)
 *   (x ^ c1) ^ c2  →  x ^ (c1 ^ c2)
 *   (x << c1) << c2 → x << (c1 + c2)
 *
 * Implementation: for each instruction `dest = src1 OP #imm2`, check if
 * src1 is defined by the same (or compatible) OP with an immediate operand.
 * If so, rewrite to use the non-immediate operand of the inner OP and combine
 * the two immediates.
 *
 * Only applied when the inner def has exactly one use (this instruction),
 * so we don't increase register pressure.
 * ============================================================================ */

static int reassoc_binary(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  /* Outer op must have immediate src2 */
  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;

  /* src1 must be a single-use TEMP vreg */
  int32_t src1_vr = irop_get_vreg(src1);
  if (src1_vr < 0 || TCCIR_DECODE_VREG_TYPE(src1_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (src1.is_lval || src1.tag != IROP_TAG_VREG)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
  if (!vi || vi->use_count != 1 || vi->def_instr < 0)
    return 0;

  IRQuadCompact *inner = &ir->compact_instructions[vi->def_instr];

  /* Inner op must also have an immediate in src2 */
  IROperand inner_src1 = tcc_ir_op_get_src1(ir, inner);
  IROperand inner_src2 = tcc_ir_op_get_src2(ir, inner);
  if (inner_src2.tag != IROP_TAG_IMM32 || inner_src2.is_lval)
    return 0;

  /* Skip if inner src1 is a memory/local operand */
  if (inner_src1.is_lval || inner_src1.is_local || inner_src1.is_llocal)
    return 0;

  int32_t c1 = inner_src2.u.imm32;
  int32_t c2 = src2.u.imm32;
  int32_t combined;
  int outer_op = q->op;
  int inner_op = inner->op;

  /* Determine the combined constant based on the operation pair */
  if (outer_op == TCCIR_OP_ADD && inner_op == TCCIR_OP_ADD) {
    combined = c1 + c2;
  } else if (outer_op == TCCIR_OP_ADD && inner_op == TCCIR_OP_SUB) {
    /* (x - c1) + c2 = x + (c2 - c1) → x + combined, or x - combined */
    combined = c2 - c1;
    outer_op = combined >= 0 ? TCCIR_OP_ADD : TCCIR_OP_SUB;
    if (combined < 0) combined = -combined;
  } else if (outer_op == TCCIR_OP_SUB && inner_op == TCCIR_OP_ADD) {
    /* (x + c1) - c2 = x + (c1 - c2) */
    combined = c1 - c2;
    outer_op = combined >= 0 ? TCCIR_OP_ADD : TCCIR_OP_SUB;
    if (combined < 0) combined = -combined;
  } else if (outer_op == TCCIR_OP_SUB && inner_op == TCCIR_OP_SUB) {
    /* (x - c1) - c2 = x - (c1 + c2) */
    combined = c1 + c2;
    outer_op = TCCIR_OP_SUB;
  } else if (outer_op == inner_op) {
    switch (outer_op) {
    case TCCIR_OP_MUL:
      combined = c1 * c2;
      break;
    case TCCIR_OP_AND:
      combined = c1 & c2;
      break;
    case TCCIR_OP_OR:
      combined = c1 | c2;
      break;
    case TCCIR_OP_XOR:
      combined = c1 ^ c2;
      break;
    case TCCIR_OP_SHL:
      if (c1 + c2 >= 32) return 0;
      combined = c1 + c2;
      break;
    case TCCIR_OP_SHR:
      if (c1 + c2 >= 32) return 0;
      combined = c1 + c2;
      break;
    case TCCIR_OP_SAR:
      if (c1 + c2 >= 32) return 0;
      combined = c1 + c2;
      break;
    case TCCIR_OP_ROR:
      combined = (c1 + c2) & 31;
      break;
    default:
      return 0;
    }
  } else {
    return 0;
  }

  /* Rewrite: outer instruction uses inner's src1 with combined constant */
  q->op = outer_op;
  tcc_ir_op_set_src1(ir, q, inner_src1);
  IROperand new_imm = irop_make_imm32(0, combined, dest.btype);
  tcc_ir_op_set_src2(ir, q, new_imm);

  /* Transfer inner's src1 use from inner to this instruction */
  int32_t inner_src1_vr = irop_get_vreg(inner_src1);
  IRSSAVregInfo *inner_vi = ssa_opt_vinfo(ctx, inner_src1_vr);
  if (inner_vi) {
    ssa_opt_remove_use_instr(inner_vi, vi->def_instr);
    ssa_opt_add_use_instr(inner_vi, idx);
  }

  /* Remove use of src1_vr (was the link between inner and outer) */
  ssa_opt_remove_use_instr(vi, idx);

  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen reassoc_gens[] = {
  { TCCIR_OP_ADD, reassoc_binary, "reassoc_add" },
  { TCCIR_OP_SUB, reassoc_binary, "reassoc_sub" },
  { TCCIR_OP_MUL, reassoc_binary, "reassoc_mul" },
  { TCCIR_OP_AND, reassoc_binary, "reassoc_and" },
  { TCCIR_OP_OR,  reassoc_binary, "reassoc_or" },
  { TCCIR_OP_XOR, reassoc_binary, "reassoc_xor" },
  { TCCIR_OP_SHL, reassoc_binary, "reassoc_shl" },
  { TCCIR_OP_SHR, reassoc_binary, "reassoc_shr" },
  { TCCIR_OP_SAR, reassoc_binary, "reassoc_sar" },
  { TCCIR_OP_ROR, reassoc_binary, "reassoc_ror" },
};

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_reassoc(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, reassoc_gens,
                          sizeof(reassoc_gens) / sizeof(reassoc_gens[0]));
}
