/*
 *  TCC IR - Fold CMP+JUMPIF/SELECT when one operand is the other plus a
 *  known integer constant (pre-SSA engine)
 *
 *  Substituting A = B + K reduces `CMP A,B` to `K cond 0`, which folds
 *  unconditionally.  Catches the `for (i = opnum+1; i < opnum; ...)` shape
 *  (gcc.c-torture/compile/pr31703.c) where GCC collapses the loop body via
 *  the signed-overflow-is-UB rule.  Signed / EQ / NE conditions only —
 *  unsigned needs an overflow proof.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/cmp_const_offset.h"

/* Resolve a CMP operand to `base ± K` (single-def ADD/SUB, int32 K).  Returns
 * the defining instruction index (>= 0) and fills base_vreg/base_lval/k on
 * success, or -1 when the operand is not a constant offset of a base vreg. */
static int cmp_offset_resolve_base(TCCIRState *ir, IROperand op, int at,
                                   int32_t *base_vreg, int *base_lval, int64_t *k_out)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return -1;
  int def = tcc_ir_find_defining_instruction(ir, vr, at);
  if (def < 0 || !tcc_ir_vreg_has_single_def(ir, vr))
    return -1;
  IRQuadCompact *dq = &ir->compact_instructions[def];
  if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
    return -1;

  IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
  IROperand ds2 = tcc_ir_op_get_src2(ir, dq);
  int64_t k;
  int32_t bvr;
  int blval;
  if (irop_get_vreg(ds1) >= 0 && irop_is_immediate(ds2))
  {
    k = irop_get_imm64_ex(ir, ds2);
    bvr = irop_get_vreg(ds1);
    blval = ds1.is_lval;
  }
  else if (dq->op == TCCIR_OP_ADD && irop_get_vreg(ds2) >= 0 && irop_is_immediate(ds1))
  {
    k = irop_get_imm64_ex(ir, ds1);
    bvr = irop_get_vreg(ds2);
    blval = ds2.is_lval;
  }
  else
    return -1;

  if (dq->op == TCCIR_OP_SUB)
    k = -k;
  if (k > (int64_t)INT32_MAX || k < (int64_t)INT32_MIN)
    return -1;

  *base_vreg = bvr;
  *base_lval = blval;
  *k_out = k;
  return def;
}

OPT_GEN_FLAT(cmp_const_offset_fold, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_ANY });

  int n = ir->next_instruction_index;
  if (n < 2 || n > 4000)
    return 0;
  if (i + 1 >= n)
    return 0;

  IRQuadCompact *next = &ir->compact_instructions[i + 1];
  int is_jumpif = (next->op == TCCIR_OP_JUMPIF);
  int is_select = (next->op == TCCIR_OP_SELECT);
  if (!is_jumpif && !is_select)
    return 0;

  int32_t vr1 = irop_get_vreg(src1);
  int32_t vr2 = irop_get_vreg(src2);
  if (vr1 < 0 || vr2 < 0 || vr1 == vr2)
    return 0;

  /* Search both orientations: vr1 = vr2 ± K, then vr2 = vr1 ± K. */
  int64_t delta = 0;
  int found = 0;
  for (int swap = 0; swap < 2 && !found; swap++)
  {
    int32_t a = swap ? vr2 : vr1;
    int32_t b = swap ? vr1 : vr2;

    int def_a = tcc_ir_find_defining_instruction(ir, a, i);
    if (def_a < 0)
      continue;
    /* Single-def only: a back-edge redef of a multi-def `a` reaches the CMP
     * with a different value than the linear-scan def, breaking the offset. */
    if (!tcc_ir_vreg_has_single_def(ir, a))
      continue;
    IRQuadCompact *dq = &ir->compact_instructions[def_a];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
      continue;

    IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
    IROperand ds2 = tcc_ir_op_get_src2(ir, dq);

    /* The ADD base must match the CMP operand in lval-ness: `*(V)` and `V`
     * share a vreg but differ in value, so `a = *(V)+K` is not `a == V+K`. */
    IROperand b_op = swap ? src1 : src2;

    int64_t k = 0;
    if (irop_get_vreg(ds1) == b && ds1.is_lval == b_op.is_lval && irop_is_immediate(ds2))
      k = irop_get_imm64_ex(ir, ds2);
    else if (dq->op == TCCIR_OP_ADD && irop_get_vreg(ds2) == b && ds2.is_lval == b_op.is_lval &&
             irop_is_immediate(ds1))
      k = irop_get_imm64_ex(ir, ds1);
    else
      continue;

    if (dq->op == TCCIR_OP_SUB)
      k = -k;
    if (k == 0)
      continue;
    /* int32-fitting only: a 32-bit-truncated ADD/SUB with a 64-bit K whose low
     * half is zero would mod-wrap on the 32-bit CMP, breaking EQ/NE. */
    if (k > (int64_t)INT32_MAX || k < (int64_t)INT32_MIN)
      continue;

    /* B must hold the same value at the CMP as at def_a.  Multi-def (not
     * single-def) so the common zero-def parameter case still folds. */
    if (tcc_ir_vreg_has_multi_def(ir, b))
      continue;
    int b_def_at_use = tcc_ir_find_defining_instruction(ir, b, i);
    int b_def_at_def = tcc_ir_find_defining_instruction(ir, b, def_a);
    if (b_def_at_use != b_def_at_def)
      continue;
    if (ir_opt_vreg_address_taken_between(ir, b, def_a, i))
      continue;

    /* delta = vr1 - vr2. swap=0 → delta = k; swap=1 → delta = -k. */
    delta = swap ? -k : k;
    found = 1;
  }

  /* Common-base fold: neither operand is the other's base, but both are
   * constant offsets of the SAME base X.  A = X+K1, B = X+K2 ⇒ A-B = K1-K2
   * under the same signed-overflow-UB assumption.  Only tried when the direct
   * search above failed, so existing folds are untouched. */
  if (!found)
  {
    int32_t base1, base2;
    int lval1, lval2;
    int64_t k1, k2;
    int def_a = cmp_offset_resolve_base(ir, src1, i, &base1, &lval1, &k1);
    int def_b = cmp_offset_resolve_base(ir, src2, i, &base2, &lval2, &k2);
    if (def_a >= 0 && def_b >= 0 && base1 == base2 && lval1 == lval2)
    {
      int32_t x = base1;
      int lo = def_a < def_b ? def_a : def_b;
      /* X must hold one value across both defs (and the CMP): reject multi-def,
       * a differing reaching def, or an aliasing mutation over the span. */
      int xd_use = tcc_ir_find_defining_instruction(ir, x, i);
      if (!tcc_ir_vreg_has_multi_def(ir, x) &&
          xd_use == tcc_ir_find_defining_instruction(ir, x, def_a) &&
          xd_use == tcc_ir_find_defining_instruction(ir, x, def_b) &&
          !ir_opt_vreg_address_taken_between(ir, x, lo, i))
      {
        int64_t d = k1 - k2;
        if (d != 0 && d >= (int64_t)INT32_MIN && d <= (int64_t)INT32_MAX)
        {
          delta = d;
          found = 1;
        }
      }
    }
  }

  if (!found)
    return 0;

  IROperand cond_op = is_jumpif
    ? tcc_ir_op_get_src1(ir, next)
    : ir->iroperand_pool[next->operand_base + 3];
  int tok = (int)irop_get_imm64_ex(ir, cond_op);

  int is_signed_cmp = (tok == 0x9c || tok == 0x9d || tok == 0x9e || tok == 0x9f);
  int is_eq_ne = (tok == 0x94 || tok == 0x95);
  if (!is_signed_cmp && !is_eq_ne)
    return 0;

  int result = evaluate_compare_condition(delta, 0, tok);
  if (result < 0)
    return 0;

  if (is_jumpif)
  {
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
    if (result)
    {
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, i + 1, jmp_dest);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_NOP;
    }
  }
  else /* SELECT */
  {
    IROperand then_val = tcc_ir_op_get_src1(ir, next);
    IROperand else_val = tcc_ir_op_get_src2(ir, next);
    IROperand chosen = result ? then_val : else_val;
    q->op = TCCIR_OP_NOP;
    next->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i + 1, chosen);
    tcc_ir_set_src2(ir, i + 1, IROP_NONE);
  }

  return 1;
}

const IROptGen cmp_const_offset_gens[] = {
    OPT_GEN_ENTRY_FLAT(cmp_const_offset_fold, TCCIR_OP_CMP),
};

const int cmp_const_offset_gens_count =
    sizeof(cmp_const_offset_gens) / sizeof(cmp_const_offset_gens[0]);

/* run_gens is a single forward pass equivalent to the original loop (the fold
 * never inserts/deletes); the whole-function DCE the original ran when it made
 * changes is kept here in the driver, since the per-CMP gen can't do it. */
static int cmp_const_offset_run(IROptCtx *ctx)
{
  int changes = tcc_ir_opt_run_gens(ctx, cmp_const_offset_gens, cmp_const_offset_gens_count);
  if (changes)
    changes += tcc_ir_opt_dce(ctx->ir);
  return changes;
}

int tcc_ir_opt_cmp_const_offset_fold_ex(IROptCtx *ctx)
{
  return cmp_const_offset_run(ctx);
}

int tcc_ir_opt_cmp_const_offset_fold(TCCIRState *ir)
{
  if (!ir)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = cmp_const_offset_run(&ctx);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}
