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
  switch (op) {
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_PREFETCH:
    return 1;
  default:
    return 0;
  }
}

void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_NOP)
    return;

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(a));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
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

  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  return irop_get_vreg(src2) == old_vr;
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

