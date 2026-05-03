/*
 *  TCC IR - SSA Narrowing / Extension Folding
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
 * Shift-pair folding: convert shift-up then shift-down patterns into
 * simpler AND or sign-extend operations.
 *
 *   (x SHL #n) SHR #n  →  x AND #((1 << (32-n)) - 1)   [zero-extend]
 *   (x SHL #n) SAR #n  →  keep as-is (sign-extend, no simpler form)
 *
 * Also handles the case where the SHL result has only one use (this SHR/SAR),
 * making the SHL dead after folding.
 *
 * Common patterns this catches:
 *   SHL #24, SHR #24  →  AND #0xFF      (unsigned char truncation)
 *   SHL #16, SHR #16  →  AND #0xFFFF    (unsigned short truncation)
 * ============================================================================ */

static int gen_shr_fold(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;
  int32_t shift_down = src2.u.imm32;
  if (shift_down <= 0 || shift_down >= 32)
    return 0;

  int32_t src1_vr = irop_get_vreg(src1);
  if (src1_vr < 0 || TCCIR_DECODE_VREG_TYPE(src1_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (src1.is_lval || src1.tag != IROP_TAG_VREG)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return 0;

  IRQuadCompact *inner = &ir->compact_instructions[vi->def_instr];
  if (inner->op != TCCIR_OP_SHL)
    return 0;

  IROperand inner_src2 = tcc_ir_op_get_src2(ir, inner);
  if (inner_src2.tag != IROP_TAG_IMM32 || inner_src2.is_lval)
    return 0;
  int32_t shift_up = inner_src2.u.imm32;

  if (shift_up != shift_down)
    return 0;

  /* (x SHL #n) SHR #n → x AND #mask */
  uint32_t mask = (1u << (32 - shift_up)) - 1;
  IROperand inner_src1 = tcc_ir_op_get_src1(ir, inner);

  if (inner_src1.is_lval || inner_src1.is_local || inner_src1.is_llocal)
    return 0;

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand mask_imm = irop_make_imm32(0, (int32_t)mask, dest.btype);

  /* Remove use of SHL result */
  ssa_opt_remove_use_instr(vi, idx);

  /* Add use of inner's src1 */
  int32_t inner_s1_vr = irop_get_vreg(inner_src1);
  IRSSAVregInfo *inner_vi = ssa_opt_vinfo(ctx, inner_s1_vr);
  if (inner_vi)
    ssa_opt_add_use_instr(inner_vi, idx);

  q->op = TCCIR_OP_AND;
  tcc_ir_set_src1(ir, idx, inner_src1);
  tcc_ir_set_src2(ir, idx, mask_imm);

  return 1;
}

/* ============================================================================
 * Redundant AND folding: when a value is ANDed with a mask that doesn't
 * remove any bits, eliminate the AND.
 *
 *   (x AND #0xFF) AND #0xFF   →  x AND #0xFF  (idempotent, handled by GVN)
 *   (x AND #0xFF) AND #0xFFFF →  x AND #0xFF  (inner mask is tighter)
 *   (x SHR #24) AND #0xFF     →  x SHR #24    (SHR #24 already produces 0..255)
 * ============================================================================ */

static int gen_and_fold(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;
  uint32_t outer_mask = (uint32_t)src2.u.imm32;

  int32_t src1_vr = irop_get_vreg(src1);
  if (src1_vr < 0 || TCCIR_DECODE_VREG_TYPE(src1_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (src1.is_lval || src1.tag != IROP_TAG_VREG)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return 0;

  IRQuadCompact *inner = &ir->compact_instructions[vi->def_instr];

  /* (x AND #inner_mask) AND #outer_mask → x AND #(inner_mask & outer_mask) */
  if (inner->op == TCCIR_OP_AND) {
    IROperand inner_src2 = tcc_ir_op_get_src2(ir, inner);
    if (inner_src2.tag != IROP_TAG_IMM32 || inner_src2.is_lval)
      return 0;
    uint32_t inner_mask = (uint32_t)inner_src2.u.imm32;
    uint32_t combined = inner_mask & outer_mask;
    if (combined == inner_mask) {
      /* outer mask doesn't remove any bits beyond inner → just use inner result */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand new_src = dest;
      new_src.vr = src1_vr;
      new_src.tag = IROP_TAG_VREG;
      new_src.is_lval = 0;
      new_src.is_local = 0;
      new_src.is_llocal = 0;
      new_src.u.imm32 = 0;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, idx, new_src);
      tcc_ir_set_src2(ir, idx, IROP_NONE);
      return 1;
    }
  }

  /* (x SHR #n) AND #mask → x SHR #n if mask covers all possible bits */
  if (inner->op == TCCIR_OP_SHR) {
    IROperand inner_src2 = tcc_ir_op_get_src2(ir, inner);
    if (inner_src2.tag != IROP_TAG_IMM32 || inner_src2.is_lval)
      return 0;
    int32_t shift = inner_src2.u.imm32;
    if (shift > 0 && shift < 32) {
      uint32_t max_bits = (1u << (32 - shift)) - 1;
      if ((outer_mask & max_bits) == max_bits) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand new_src = dest;
        new_src.vr = src1_vr;
        new_src.tag = IROP_TAG_VREG;
        new_src.is_lval = 0;
        new_src.is_local = 0;
        new_src.is_llocal = 0;
        new_src.u.imm32 = 0;
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, idx, new_src);
        tcc_ir_set_src2(ir, idx, IROP_NONE);
        return 1;
      }
    }
  }

  return 0;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen narrow_gens[] = {
  { TCCIR_OP_SHR, gen_shr_fold, "narrow_shr" },
  { TCCIR_OP_AND, gen_and_fold, "narrow_and" },
};

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_narrow(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, narrow_gens,
                          sizeof(narrow_gens) / sizeof(narrow_gens[0]));
}
