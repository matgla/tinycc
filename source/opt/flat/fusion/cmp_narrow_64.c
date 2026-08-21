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




/* The high 32 bits of `op` are provably zero.
 * Returns 0 = not proven, 1 = proven (a value), 2 = proven and `op` is a
 * constant, whose value lands in *imm_out so the caller can re-encode it. */
static int cn64_hi_is_zero(TCCIRState *ir, const int *def_idx, int max_tmp_pos,
                           IROperand op, uint64_t *imm_out)
{
  if (irop_is_immediate(op))
  {
    uint64_t v = (uint64_t)irop_get_imm64_ex(ir, op);
    if ((v >> 32) != 0)
      return 0;
    if (imm_out)
      *imm_out = v;
    return 2;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int di = -1;
  const int type = TCCIR_DECODE_VREG_TYPE(vr);
  if (type == TCCIR_VREG_TYPE_TEMP)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_tmp_pos)
      di = def_idx[pos];
  }
  else if (type == TCCIR_VREG_TYPE_VAR)
  {
    /* A named local needs more care than a temp: several definitions, or an
     * address handed out to something this scan cannot follow, and the def
     * found here is not the one reaching the compare. */
    if (!tcc_ir_vreg_has_single_def(ir, vr))
      return 0;
    for (int k = 0; k < ir->next_instruction_index; k++)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (qk->op == TCCIR_OP_LEA && irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == vr)
        return 0;
      if (irop_config[qk->op].has_dest && di < 0 &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, qk)) == vr)
        di = k;
    }
  }
  if (di < 0)
    return 0;

  IRQuadCompact *qd = &ir->compact_instructions[di];
  if (qd->op == TCCIR_OP_ZEXT)
    return 1; /* u32 -> u64 always zeroes the high half */
  if (qd->op == TCCIR_OP_SHR)
  {
    IROperand amt = tcc_ir_op_get_src2(ir, qd);
    IROperand val = tcc_ir_op_get_src1(ir, qd);
    if (irop_is_immediate(amt) && irop_get_imm64_ex(ir, amt) >= 32 &&
        irop_get_btype(val) == IROP_BTYPE_INT64)
      return 1;
  }
  if (qd->op == TCCIR_OP_AND)
  {
    /* A mask whose high word is zero zeroes the result's, whatever the other
     * operand holds.  `rem = mant & 7` then `rem > 4` is how
     * sfp_round_pack_double writes its rounding decision, and without this it
     * is a 64-bit compare: two instructions to build the 64-bit 4, a CMP and
     * an SBCS, for what `cmp rn,#4` says on its own. */
    for (int k = 0; k < 2; k++)
    {
      IROperand m = k ? tcc_ir_op_get_src2(ir, qd) : tcc_ir_op_get_src1(ir, qd);
      if (irop_is_immediate(m) && ((uint64_t)irop_get_imm64_ex(ir, m) >> 32) == 0)
        return 1;
    }
  }
  return 0;
}

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

    /* Both operands' high words must be provably zero; either one may be the
     * constant, since the frontend swaps the operands of a UGT/ULE compare to
     * reach the condition codes the backend has. */
    uint64_t imm1 = 0, imm2 = 0;
    int z1 = cn64_hi_is_zero(ir, def_idx, max_tmp_pos, src1, &imm1);
    int z2 = cn64_hi_is_zero(ir, def_idx, max_tmp_pos, src2, &imm2);

    /* A VAR holding a constant only reaches the compare through its STORE --
     * the shape printf argument locals keep after const-prop. */
    if (!z2 && var_def_idx)
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
            if (irop_is_immediate(store_src) &&
                ((uint64_t)irop_get_imm64_ex(ir, store_src) >> 32) == 0)
            {
              imm2 = (uint64_t)irop_get_imm64_ex(ir, store_src);
              z2 = 2;
            }
          }
        }
      }
    }
    if (!z1 || !z2)
      continue;

    /* The 64-bit lowering has no GT/LE form, so the frontend reaches those by
     * exchanging the operands -- which leaves the constant in src1, where the
     * backend has to put it in a register first.  Once the compare is 32 bits
     * that constraint is gone: exchange them back and mirror the condition on
     * every flag reader, and `mov rX,#4; cmp rX,rn` becomes `cmp rn,#4`. */
    int swap = (z1 == 2 && z2 != 2);
    if (swap)
    {
      int ok = 1;
      for (int j = i + 1; j < n; j++)
      {
        IRQuadCompact *qj = &ir->compact_instructions[j];
        if (qj->op == TCCIR_OP_NOP)
          continue;
        if (qj->op != TCCIR_OP_SETIF && qj->op != TCCIR_OP_JUMPIF)
          break;
        if (qj->is_jump_target)
        {
          ok = 0;
          break;
        }
        IROperand c = tcc_ir_op_get_src1(ir, qj);
        if (!irop_is_immediate(c) || swap_cond_token((int)irop_get_imm64_ex(ir, c)) < 0)
        {
          ok = 0;
          break;
        }
      }
      if (ok)
      {
        for (int j = i + 1; j < n; j++)
        {
          IRQuadCompact *qj = &ir->compact_instructions[j];
          if (qj->op == TCCIR_OP_NOP)
            continue;
          if (qj->op != TCCIR_OP_SETIF && qj->op != TCCIR_OP_JUMPIF)
            break;
          IROperand c = tcc_ir_op_get_src1(ir, qj);
          int st = swap_cond_token((int)irop_get_imm64_ex(ir, c));
          tcc_ir_set_src1(ir, j, irop_make_imm32(-1, st, irop_get_btype(c)));
        }
      }
      else
        swap = 0;
    }

    /* Narrow both CMP operands to INT32 by patching its operand-pool entries;
     * each source's own def stays u64, so other consumers still see u64. */
    LOG_IR_GEN("OPTIMIZE: cmp_narrow_64 at i=%d (both operands hi=0, swap=%d)", i, swap);
    IROperand new_src1 = (z1 == 2)
                             ? irop_make_imm32(-1, (int32_t)(uint32_t)imm1, IROP_BTYPE_INT32)
                             : src1;
    IROperand new_src2 = (z2 == 2)
                             ? irop_make_imm32(-1, (int32_t)(uint32_t)imm2, IROP_BTYPE_INT32)
                             : src2;
    new_src1.btype = IROP_BTYPE_INT32;
    new_src2.btype = IROP_BTYPE_INT32;
    if (z1 == 2)
      new_src1.is_unsigned = src1.is_unsigned;
    if (z2 == 2)
      new_src2.is_unsigned = src2.is_unsigned;
    if (swap)
    {
      IROperand t = new_src1;
      new_src1 = new_src2;
      new_src2 = t;
    }
    tcc_ir_set_src1(ir, i, new_src1);
    tcc_ir_set_src2(ir, i, new_src2);
    changes++;
  }
  if (var_def_idx)
    tcc_free(var_def_idx);

  tcc_free(def_idx);
  return changes;
}
int tcc_ir_opt_cmp_narrow_64_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_narrow_64(ctx->ir); }
