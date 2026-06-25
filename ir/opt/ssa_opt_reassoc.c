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

  /* Record the new use of inner's src1 at this instruction.  The inner
   * instruction still reads it too — its use record must stay until the
   * inner is actually NOPed (ssa_opt_nop_instr removes it then).  Removing
   * it here while the inner is live lets a later pass (e.g. GVN CSEing the
   * outer back onto the still-live inner) drop the count to zero and DCE
   * then kills the operand's def out from under the live inner. */
  int32_t inner_src1_vr = irop_get_vreg(inner_src1);
  IRSSAVregInfo *inner_vi = ssa_opt_vinfo(ctx, inner_src1_vr);
  if (inner_vi)
    ssa_opt_add_use_instr(inner_vi, idx);

  /* Remove use of src1_vr (was the link between inner and outer) */
  ssa_opt_remove_use_instr(vi, idx);

  return 1;
}

/* ============================================================================
 * reassoc_add_cancel_const: (x + c) + (x - c) → x + x
 *
 * Pattern: outer ADD whose two operands are single-use TEMPs defined by
 * ADD(a, c) and SUB(a, c) respectively, where a is the same vreg and c
 * is the same immediate.  Rewrite as `a + a` (the backend can emit a
 * single ADD or LSL #1 depending on register/encoding).
 *
 * Also handles the symmetric (a + c) + (a + (-c)) and SUB/ADD orderings.
 *
 * Constraints: inner defs are single-use (only this outer ADD reads them),
 * so we can let DCE clean them up afterwards.
 * ============================================================================ */
static int reassoc_add_cancel_const(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_ADD)
    return 0;

  IROperand os1 = tcc_ir_op_get_src1(ir, q);
  IROperand os2 = tcc_ir_op_get_src2(ir, q);
  if (os1.tag != IROP_TAG_VREG || os2.tag != IROP_TAG_VREG)
    return 0;
  if (os1.is_lval || os2.is_lval)
    return 0;

  int32_t v1 = irop_get_vreg(os1);
  int32_t v2 = irop_get_vreg(os2);
  if (v1 < 0 || v2 < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(v1) != TCCIR_VREG_TYPE_TEMP ||
      TCCIR_DECODE_VREG_TYPE(v2) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  IRSSAVregInfo *vi1 = ssa_opt_vinfo(ctx, v1);
  IRSSAVregInfo *vi2 = ssa_opt_vinfo(ctx, v2);
  if (!vi1 || !vi2 || vi1->def_count != 1 || vi2->def_count != 1)
    return 0;
  if (vi1->use_count != 1 || vi2->use_count != 1)
    return 0;

  IRQuadCompact *d1 = &ir->compact_instructions[vi1->def_instr];
  IRQuadCompact *d2 = &ir->compact_instructions[vi2->def_instr];

  /* Match (a OP1 c) and (a OP2 c) where OP1/OP2 are {ADD, SUB} and the
   * constants cancel (same value with opposite signs in the combined sum). */
  if ((d1->op != TCCIR_OP_ADD && d1->op != TCCIR_OP_SUB) ||
      (d2->op != TCCIR_OP_ADD && d2->op != TCCIR_OP_SUB))
    return 0;

  IROperand d1s1 = tcc_ir_op_get_src1(ir, d1);
  IROperand d1s2 = tcc_ir_op_get_src2(ir, d1);
  IROperand d2s1 = tcc_ir_op_get_src1(ir, d2);
  IROperand d2s2 = tcc_ir_op_get_src2(ir, d2);

  if (d1s1.tag != IROP_TAG_VREG || d2s1.tag != IROP_TAG_VREG)
    return 0;
  if (d1s1.is_lval || d2s1.is_lval)
    return 0;
  if (!irop_is_immediate(d1s2) || !irop_is_immediate(d2s2))
    return 0;

  int32_t a1 = irop_get_vreg(d1s1);
  int32_t a2 = irop_get_vreg(d2s1);
  if (a1 != a2)
    return 0;

  int32_t c1 = irop_get_imm32(d1s2);
  int32_t c2 = irop_get_imm32(d2s2);
  int sign1 = (d1->op == TCCIR_OP_ADD) ? 1 : -1;
  int sign2 = (d2->op == TCCIR_OP_ADD) ? 1 : -1;
  /* The constants cancel when c1*sign1 + c2*sign2 == 0. */
  if ((int64_t)c1 * sign1 + (int64_t)c2 * sign2 != 0)
    return 0;

  /* Type of the outer dest must match the inner sources so we don't
   * accidentally change semantics through implicit narrowing. */
  int outer_btype = irop_get_btype(tcc_ir_op_get_dest(ir, q));
  if (irop_get_btype(d1s1) != outer_btype)
    return 0;

  /* Rewrite outer as a + a. */
  IROperand a_op = irop_make_vreg(a1, outer_btype);
  tcc_ir_op_set_src1(ir, q, a_op);
  tcc_ir_op_set_src2(ir, q, a_op);

  /* Remove the old uses of v1, v2 from the outer ADD. */
  ssa_opt_remove_use_instr(vi1, idx);
  ssa_opt_remove_use_instr(vi2, idx);

  /* Add two uses of `a` at the outer ADD. */
  IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, a1);
  if (avi) {
    ssa_opt_add_use_instr(avi, idx);
    ssa_opt_add_use_instr(avi, idx);
  }

  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static int reassoc_add_dispatch(IRSSAOptCtx *ctx, int idx)
{
  int r = reassoc_add_cancel_const(ctx, idx);
  if (r)
    return r;
  return reassoc_binary(ctx, idx);
}

static const IRSSAOptGen reassoc_gens[] = {
  { TCCIR_OP_ADD, reassoc_add_dispatch, "reassoc_add" },
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
