/*
 *  TCC IR - SSA instruction / operand rewriting
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

int ssa_opt_has_side_effects(int op)
{
  /* Everything but reading memory and the flags. */
  return ir_op_has(op, IR_HZ_FROM_OP & ~(IR_HZ_MEM_READ | IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ)) &&
         !ir_opset_has(IR_LEGACY_GAP_OPS(TCCIR_OP_LOAD_POSTINC), op);
}

void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_NOP)
    return;

  if (irop_config[q->op].has_src1) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, tcc_ir_op_src1_vreg(ir, q));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (irop_config[q->op].has_src2) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, tcc_ir_op_src2_vreg(ir, q));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_MLA) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, tcc_ir_op_accum_vreg(ir, q));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, tcc_ir_op_dest_vreg(ir, q));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }

  q->op = TCCIR_OP_NOP;
}

static void ssa_opt_rewrite_operand(IRSSAOptCtx *ctx, int instr_idx,
                                    int32_t old_vr, int32_t new_vr)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(s) == old_vr) {
      irop_set_vreg(&s, new_vr);
      tcc_ir_op_set_src1(ir, q, s);
    }
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s) == old_vr) {
      irop_set_vreg(&s, new_vr);
      tcc_ir_op_set_src2(ir, q, s);
    }
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    if (irop_get_vreg(a) == old_vr) {
      irop_set_vreg(&a, new_vr);
      tcc_ir_op_set_accum(ir, q, a);
    }
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) == old_vr) {
      irop_set_vreg(&d, new_vr);
      tcc_ir_op_set_dest(ir, q, d);
    }
  }
}

static int ssa_opt_use_is_barrel_shift_src2(IRSSAOptCtx *ctx, IRSSAUse use,
                                            int32_t old_vr)
{
  if (use.kind != SSA_USE_INSTR)
    return 0;

  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[use.idx];
  if (tcc_ir_barrel_shift_at(ir, q) == 0 || !irop_config[q->op].has_src2)
    return 0;

  return tcc_ir_op_src2_vreg(ir, q) == old_vr;
}

static void ssa_opt_rewrite_phi_operand(IRSSAOptCtx *ctx, int block,
                                        int slot, int32_t old_vr,
                                        int32_t new_vr)
{
  IRSSAState *ssa = ctx->ssa;
  for (IRPhiNode *phi = ssa->block_phis[block]; phi; phi = phi->next) {
    if (slot < phi->num_operands && phi->operands[slot].vreg == old_vr) {
      phi->operands[slot].vreg = new_vr;
      return;
    }
  }
}

int ssa_opt_can_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, old_vr);
  if (!vi)
    return 0;

  for (int i = 0; i < vi->use_count; i++) {
    if (ssa_opt_use_is_barrel_shift_src2(ctx, vi->uses[i], old_vr))
      return 0;
  }
  return 1;
}

int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr)
{
  if (old_vr == new_vr)
    return 0;
  IRSSAVregInfo *old_vi = ssa_opt_vinfo(ctx, old_vr);
  IRSSAVregInfo *new_vi = ssa_opt_vinfo(ctx, new_vr);
  if (!old_vi)
    return 0;

  /* A src2 carrying a fused barrel shift must keep its exact vreg identity. */
  if (!ssa_opt_can_replace_all_uses(ctx, old_vr))
    return 0;

  int count = 0;
  while (old_vi->use_count > 0) {
    IRSSAUse use = old_vi->uses[--old_vi->use_count];

    if (use.kind == SSA_USE_INSTR)
      ssa_opt_rewrite_operand(ctx, use.idx, old_vr, new_vr);
    else
      ssa_opt_rewrite_phi_operand(ctx, use.idx, use.slot, old_vr, new_vr);

    if (new_vi) {
      if (use.kind == SSA_USE_INSTR)
        ssa_opt_add_use_instr(new_vi, use.idx);
      else
        ssa_opt_add_use_phi(new_vi, use.idx, use.slot);
    }
    count++;
  }

  return count;
}

