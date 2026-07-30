/*
 *  TCC IR - 64-bit Register Pair Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"



/* tcc_ir_opt_shl32_or_chain: collapse `((X SHL 32) OR Y) SHL 32` and
 * `((X SHL 32) OR Y) AND 0xFFFFFFFF` chains from the i32->i64 widening idiom.
 *
 * The final SHL 32 shifts out (or AND 0xFFFFFFFF masks out) the high half
 * produced by `T_shl1 OR Y`, so T_shl1 and the SAR feeding it are dead:
 * rewrite the consumer to read the kept OR operand directly, NOP the OR and
 * its SHL, and let DCE remove the rest.
 *
 * T_shl1 and T_or must be single-use TEMPs for the NOPs to be safe. */
int tcc_ir_opt_shl32_or_chain(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 3)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Looking for the consumer: SHL #32 or AND #0xFFFFFFFF on a TEMP src1. */
    int is_shl32 = 0, is_and_low = 0;
    if (q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_AND)
    {
      IROperand q_src2 = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(q_src2))
        continue;
      int64_t imm = irop_get_imm64_ex(ir, q_src2);
      if (q->op == TCCIR_OP_SHL && imm == 32)
        is_shl32 = 1;
      else if (q->op == TCCIR_OP_AND && (uint32_t)imm == 0xFFFFFFFFu)
        /* Low 32 bits only: irop_get_imm64_ex sign-extends a 32-bit mask to
           int64_t -1, so a 0x00000000FFFFFFFF test would never match. */
        is_and_low = 1;
      else
        continue;
    }
    else
    {
      continue;
    }
    IROperand q_dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_btype(q_dest) != IROP_BTYPE_INT64)
      continue;

    IROperand q_src1 = tcc_ir_op_get_src1(ir, q);
    int32_t or_vr = irop_get_vreg(q_src1);
    if (TCCIR_DECODE_VREG_TYPE(or_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (q_src1.is_lval || q_src1.is_sym)
      continue;
    if (ir_opt_du_uses(&du, or_vr) != 1 || !ir_opt_du_is_single_def(&du, or_vr))
      continue;
    int or_def = ir_opt_du_def(&du, or_vr, n);
    if (or_def < 0)
      continue;

    IRQuadCompact *or_q = &ir->compact_instructions[or_def];
    if (or_q->op != TCCIR_OP_OR)
      continue;
    IROperand or_dest = tcc_ir_op_get_dest(ir, or_q);
    if (irop_get_btype(or_dest) != IROP_BTYPE_INT64)
      continue;

    /* One of OR's operands must be `something SHL #32` (the dead-bits half). */
    IROperand or_a = tcc_ir_op_get_src1(ir, or_q);
    IROperand or_b = tcc_ir_op_get_src2(ir, or_q);

    int chosen = -1; /* 0 -> a is shl, b is keep; 1 -> b is shl, a is keep */
    int shl1_def = -1;
    IROperand keep_op = IROP_NONE;

    for (int s = 0; s < 2; s++)
    {
      IROperand shl_cand = (s == 0) ? or_a : or_b;
      IROperand keep_cand = (s == 0) ? or_b : or_a;
      int32_t shl_vr = irop_get_vreg(shl_cand);
      if (TCCIR_DECODE_VREG_TYPE(shl_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (shl_cand.is_lval || shl_cand.is_sym)
        continue;
      if (ir_opt_du_uses(&du, shl_vr) != 1 || !ir_opt_du_is_single_def(&du, shl_vr))
        continue;
      int def = ir_opt_du_def(&du, shl_vr, n);
      if (def < 0)
        continue;
      IRQuadCompact *shl_q = &ir->compact_instructions[def];
      if (shl_q->op != TCCIR_OP_SHL)
        continue;
      IROperand shl_amt = tcc_ir_op_get_src2(ir, shl_q);
      if (!irop_is_immediate(shl_amt) || irop_get_imm64_ex(ir, shl_amt) != 32)
        continue;
      IROperand shl_dest_chk = tcc_ir_op_get_dest(ir, shl_q);
      if (irop_get_btype(shl_dest_chk) != IROP_BTYPE_INT64)
        continue;
      chosen = s;
      shl1_def = def;
      keep_op = keep_cand;
      break;
    }
    if (chosen < 0)
      continue;

    LOG_IR_GEN("OPTIMIZE: SHL32_OR_CHAIN %s at i=%d (or_def=%d, shl1_def=%d)",
               is_shl32 ? "SHL32" : "AND_low", i, or_def, shl1_def);
    (void)is_and_low;

    tcc_ir_set_src1(ir, i, keep_op);
    /* OR and its feeding SHL are now dead (both single-use). */
    ir->compact_instructions[or_def].op = TCCIR_OP_NOP;
    ir->compact_instructions[shl1_def].op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(du.def);
  return changes;
}
int tcc_ir_opt_shl32_or_chain_ex(IROptCtx *ctx) { return tcc_ir_opt_shl32_or_chain(ctx->ir); }
