/*
 *  TCC IR - SSA CMP constant-offset fold (ssa:cmp_offset_fold)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt/ssa/cmp_offset_fold.h"
#include <limits.h>

/* Resolve a single-def TEMP to `base ± K` with a single-def TEMP base and int32 K. */
static int co_resolve_base(IRSSAOptCtx *ctx, int32_t vr, int32_t *base_vr, int64_t *k_out)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
    return 0;
  if (tcc_ir_barrel_shift_at(ir, dq))
    return 0;
  /* The `A-B = K` substitution assumes the offset add can't wrap (signed-overflow
   * is UB).  An unsigned add wraps by definition (pr28651: `(int)(u+4) < (int)u`,
   * u=INT_MAX), so reject an unsigned-typed offset. */
  if (tcc_ir_op_get_dest(ir, dq).is_unsigned)
    return 0;

  IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
  IROperand ds2 = tcc_ir_op_get_src2(ir, dq);
  int64_t k;
  int32_t bvr;
  if (irop_get_vreg(ds1) >= 0 && !ds1.is_lval && irop_is_immediate(ds2))
  {
    k = irop_get_imm64_ex(ir, ds2);
    bvr = irop_get_vreg(ds1);
  }
  else if (dq->op == TCCIR_OP_ADD && irop_get_vreg(ds2) >= 0 && !ds2.is_lval &&
           irop_is_immediate(ds1))
  {
    k = irop_get_imm64_ex(ir, ds1);
    bvr = irop_get_vreg(ds2);
  }
  else
    return 0;

  if (dq->op == TCCIR_OP_SUB)
    k = -k;
  if (k > (int64_t)INT32_MAX || k < (int64_t)INT32_MIN)
    return 0;

  *base_vr = bvr;
  *k_out = k;
  return 1;
}

/* Whether any instruction directly writes vr (as a non-lval dest). */
static int co_vreg_ever_written(TCCIRState *ir, int32_t vr)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!d.is_lval && irop_get_vreg(d) == vr)
      return 1;
  }
  return 0;
}

/* A base that holds ONE value at every use, so `A-B = K1-K2` is exact: a
 * single-def TEMP (SSA value, stable by dominance), or a PARAM that is never
 * written and whose address is never taken (a constant function input — the
 * flat pass's extra reach, admitted here soundly without a reaching-def scan).
 * A VAR is memory and rejected. */
static int co_stable(IRSSAOptCtx *ctx, int32_t vr)
{
  int ty = TCCIR_DECODE_VREG_TYPE(vr);
  if (ty == TCCIR_VREG_TYPE_TEMP)
  {
    /* Single SSA def (stable by dominance) or zero defs (never written =
     * constant); only a multi-def (reused, e.g. regalloc-fallback) TEMP varies. */
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    return vi && vi->def_count <= 1;
  }
  if (ty == TCCIR_VREG_TYPE_PARAM)
  {
    TCCIRState *ir = ctx->ir;
    int n = ir->next_instruction_index;
    return !co_vreg_ever_written(ir, vr) &&
           !ir_opt_vreg_address_taken_between(ir, vr, -1, n);
  }
  return 0;
}

/* delta = value(vr1) - value(vr2) as a known int32 constant, or return 0. */
static int co_find_delta(IRSSAOptCtx *ctx, int32_t vr1, int32_t vr2, int64_t *delta)
{
  int32_t base;
  int64_t k;
  /* Direct: vr1 = vr2 + K  (vr2 is the base, and must be single-valued). */
  if (co_resolve_base(ctx, vr1, &base, &k) && base == vr2 && k != 0 && co_stable(ctx, vr2))
  {
    *delta = k;
    return 1;
  }
  if (co_resolve_base(ctx, vr2, &base, &k) && base == vr1 && k != 0 && co_stable(ctx, vr1))
  {
    *delta = -k;
    return 1;
  }
  /* Common base: vr1 = X+K1, vr2 = X+K2 with the same single-valued X. */
  int32_t b1, b2;
  int64_t k1, k2;
  if (co_resolve_base(ctx, vr1, &b1, &k1) && co_resolve_base(ctx, vr2, &b2, &k2) &&
      b1 == b2 && co_stable(ctx, b1))
  {
    int64_t d = k1 - k2;
    if (d != 0 && d >= (int64_t)INT32_MIN && d <= (int64_t)INT32_MAX)
    {
      *delta = d;
      return 1;
    }
  }
  return 0;
}

int ssa_opt_cmp_offset_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i + 1 < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    int is_jumpif = (next->op == TCCIR_OP_JUMPIF);
    int is_select = (next->op == TCCIR_OP_SELECT);
    int is_setif = (next->op == TCCIR_OP_SETIF);
    if (!is_jumpif && !is_select && !is_setif)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (src1.is_lval || src2.is_lval)
      continue;
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);
    if (vr1 < 0 || vr2 < 0 || vr1 == vr2)
      continue;

    int64_t delta;
    if (!co_find_delta(ctx, vr1, vr2, &delta))
      continue;

    IROperand cond_op = is_select ? ir->iroperand_pool[next->operand_base + 3]
                                  : tcc_ir_op_get_src1(ir, next);
    int tok = (int)irop_get_imm64_ex(ir, cond_op);
    int is_signed_cmp = (tok == 0x9c || tok == 0x9d || tok == 0x9e || tok == 0x9f);
    int is_eq_ne = (tok == 0x94 || tok == 0x95);
    if (!is_signed_cmp && !is_eq_ne)
      continue;
    int result = evaluate_compare_condition(delta, 0, tok);
    if (result < 0)
      continue;

    if (is_jumpif)
    {
      IROperand jdst = tcc_ir_op_get_dest(ir, next);
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jdst);
        if (cfg && i + 2 < cfg->num_instrs)
        {
          int ft = i + 2;
          while (ft < cfg->num_instrs && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
            ft++;
          if (ft < cfg->num_instrs)
          {
            int jdst_idx = (int)jdst.u.imm32;   /* target may index past instr_to_block */
            int tgt_block = (jdst_idx >= 0 && jdst_idx < cfg->num_instrs)
                              ? cfg->instr_to_block[jdst_idx] : -1;
            int ft_block = cfg->instr_to_block[ft];
            if (ft_block != tgt_block)
              ssa_drop_phi_edge(ctx, cfg->instr_to_block[i + 1], ft_block);
          }
        }
      }
      else
      {
        int t = (int)jdst.u.imm32;
        int tblk = (cfg && t >= 0 && t < cfg->num_instrs) ? cfg->instr_to_block[t] : -1;
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
        if (cfg && tblk >= 0)
          ssa_drop_phi_edge(ctx, cfg->instr_to_block[i + 1], tblk);
      }
    }
    else if (is_select) /* dest = cond ? then : else */
    {
      IROperand chosen = result ? tcc_ir_op_get_src1(ir, next)
                                : tcc_ir_op_get_src2(ir, next);
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
    }
    else /* SETIF: dest = (cond) as 0/1 */
    {
      IROperand set_dest = tcc_ir_op_get_dest(ir, next);
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
      tcc_ir_op_set_dest(ir, next, set_dest);
    }

    IRSSAVregInfo *v1 = ssa_opt_vinfo(ctx, vr1);
    if (v1)
      ssa_opt_remove_use_instr(v1, i);
    IRSSAVregInfo *v2 = ssa_opt_vinfo(ctx, vr2);
    if (v2)
      ssa_opt_remove_use_instr(v2, i);
    changes++;
  }

  return changes;
}
