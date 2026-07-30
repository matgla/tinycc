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



int tcc_ir_opt_cmp_narrow_64(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* Build def-idx for TEMPs and VAR-STOREs. */
  int max_tmp_pos = 0;
  int max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_pos)
      max_tmp_pos = pos;
    if (type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
      max_var_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  int stride = max_tmp_pos + 1;
  int *def_idx = tcc_malloc(stride * sizeof(int));
  for (int i = 0; i < stride; i++)
    def_idx[i] = -1;
  /* Track VAR STORE definitions: last STORE to V at this position. */
  int var_stride = max_var_pos + 1;
  int *var_def_idx = NULL;
  if (var_stride > 0)
  {
    var_def_idx = tcc_malloc(var_stride * sizeof(int));
    for (int i = 0; i < var_stride; i++)
      var_def_idx[i] = -1;
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE && irop_config[q->op].has_dest)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && var_def_idx)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var_pos)
          var_def_idx[pos] = i;
      }
      continue;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_tmp_pos)
      def_idx[pos] = i;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (irop_get_btype(src1) != IROP_BTYPE_INT64)
      continue;
    if (irop_get_btype(src2) != IROP_BTYPE_INT64)
      continue;

    /* Narrowing is only safe when the condition treats both widths
     * identically: EQ/NE (bitwise), and unsigned <,<=,>,>= (both hi=0, so
     * unsigned order is width-invariant). SIGNED order is NOT safe: a u64
     * with hi=0 can be positive at 64-bit yet negative as i32. Look up the
     * consuming SETIF/JUMPIF condition to decide. */
    int cond_ok = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (qj->op != TCCIR_OP_SETIF && qj->op != TCCIR_OP_JUMPIF)
        break;
      IROperand cond_op = tcc_ir_op_get_src1(ir, qj);
      if (!irop_is_immediate(cond_op))
        break;
      int tok = (int)irop_get_imm64_ex(ir, cond_op);
      if (tok == TOK_EQ || tok == TOK_NE ||
          tok == TOK_ULT || tok == TOK_ULE || tok == TOK_UGT || tok == TOK_UGE)
        cond_ok = 1;
      break;
    }
    if (!cond_ok)
      continue;

    /* src1 must be a TEMP whose def proves hi=0. */
    int32_t s1_vr = irop_get_vreg(src1);
    if (TCCIR_DECODE_VREG_TYPE(s1_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
    if (s1_pos > max_tmp_pos || def_idx[s1_pos] < 0)
      continue;
    IRQuadCompact *q_def = &ir->compact_instructions[def_idx[s1_pos]];
    int s1_hi_zero = 0;
    if (q_def->op == TCCIR_OP_ZEXT)
    {
      /* ZEXT from u32 → u64 always zeros the high half. */
      s1_hi_zero = 1;
    }
    else if (q_def->op == TCCIR_OP_SHR)
    {
      IROperand shr_amt = tcc_ir_op_get_src2(ir, q_def);
      if (irop_is_immediate(shr_amt) && irop_get_imm64_ex(ir, shr_amt) >= 32)
      {
        IROperand shr_src = tcc_ir_op_get_src1(ir, q_def);
        if (irop_get_btype(shr_src) == IROP_BTYPE_INT64)
          s1_hi_zero = 1;
      }
    }
    if (!s1_hi_zero)
      continue;

    /* src2 must be a u64 constant with high 32 bits == 0. Two forms:
     *   (a) inline immediate
     *   (b) VAR whose STORE def wrote a u64 constant — printf arg locals
     *       stay VAR-stored after const-prop, so walk the def chain. */
    uint64_t imm;
    int got_imm = 0;
    if (irop_is_immediate(src2))
    {
      imm = (uint64_t)irop_get_imm64_ex(ir, src2);
      got_imm = 1;
    }
    else if (var_def_idx)
    {
      int32_t s2_vr = irop_get_vreg(src2);
      if (TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
        if (s2_pos <= max_var_pos && var_def_idx[s2_pos] >= 0)
        {
          IRQuadCompact *q_vdef = &ir->compact_instructions[var_def_idx[s2_pos]];
          if (q_vdef->op == TCCIR_OP_STORE)
          {
            IROperand store_src = tcc_ir_op_get_src1(ir, q_vdef);
            if (irop_is_immediate(store_src))
            {
              imm = (uint64_t)irop_get_imm64_ex(ir, store_src);
              got_imm = 1;
            }
          }
        }
      }
    }
    if (!got_imm)
      continue;
    if ((imm >> 32) != 0)
      continue;

    /* Narrow both CMP operands to INT32 by patching its operand-pool
     * entries; T's def stays u64, so other consumers still see u64. */
    LOG_IR_GEN("OPTIMIZE: cmp_narrow_64 at i=%d (T%d hi=0, imm=%llu)", i, s1_pos, (unsigned long long)imm);
    IROperand new_src1 = src1;
    new_src1.btype = IROP_BTYPE_INT32;
    tcc_ir_set_src1(ir, i, new_src1);
    IROperand new_src2 = irop_make_imm32(-1, (int32_t)(uint32_t)imm, IROP_BTYPE_INT32);
    new_src2.is_unsigned = src2.is_unsigned;
    tcc_ir_set_src2(ir, i, new_src2);
    changes++;
  }
  if (var_def_idx)
    tcc_free(var_def_idx);

  tcc_free(def_idx);
  return changes;
}
int tcc_ir_opt_cmp_narrow_64_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_narrow_64(ctx->ir); }
