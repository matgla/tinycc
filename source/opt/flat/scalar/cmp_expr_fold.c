/*
 *  TCC IR - CMP expression-equality fold (flat, pre-SSA)
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
#include "opt_du.h"

static int ir_opt_vreg_use_count(TCCIRState *ir, int32_t vreg)
{
  if (!ir || vreg < 0)
    return -1;
  int n = ir->next_instruction_index;
  int count = 0;
  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg ||
        irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg ||
        ir_opt_mla_accum_vreg(ir, q) == vreg)
      count++;
  }
  return count;
}

static void ir_opt_setif_chain_cleanup(TCCIRState *ir, int def1, int def2, int32_t vr1, int32_t vr2)
{
  if (def1 < 0 || def2 < 0)
    return;
  IRQuadCompact *dq1 = &ir->compact_instructions[def1];
  IRQuadCompact *dq2 = &ir->compact_instructions[def2];
  if (dq1->op != TCCIR_OP_SETIF || dq2->op != TCCIR_OP_SETIF)
    return;
  if (ir_opt_vreg_use_count(ir, vr1) != 0 || ir_opt_vreg_use_count(ir, vr2) != 0)
    return;

  int cmp_a_idx = def1 - 1;
  while (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_NOP)
    cmp_a_idx--;
  int cmp_b_idx = def2 - 1;
  while (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_NOP)
    cmp_b_idx--;

  dq1->op = TCCIR_OP_NOP;
  dq2->op = TCCIR_OP_NOP;
  if (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_a_idx].is_jump_target)
    ir->compact_instructions[cmp_a_idx].op = TCCIR_OP_NOP;
  if (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_b_idx].is_jump_target)
    ir->compact_instructions[cmp_b_idx].op = TCCIR_OP_NOP;
}
/* Volatile local operands must not fold: each read is a mandated access. */
static int cef_operand_is_volatile_local(TCCIRState *ir, IROperand s)
{
  int32_t v = irop_get_vreg(s);
  if (v < 0 || !tcc_ir_vreg_is_valid(ir, v))
    return 0;
  IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v);
  return iv && iv->is_volatile;
}

static int cef_symref_same(TCCIRState *ir, IROperand a, IROperand b)
{
  IRPoolSymref *a_ref = irop_get_symref_ex(ir, a);
  IRPoolSymref *b_ref = irop_get_symref_ex(ir, b);
  return a_ref && b_ref && a_ref->sym == b_ref->sym && a_ref->addend == b_ref->addend;
}

/* The instruction that consumes this CMP's flags, or NULL. */
static IRQuadCompact *cef_flag_consumer(TCCIRState *ir, int cmp_idx)
{
  int n = ir->next_instruction_index;
  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return NULL;
  IRQuadCompact *use = &ir->compact_instructions[j];
  if (use->is_jump_target)
    return NULL;
  if (use->op != TCCIR_OP_SELECT && use->op != TCCIR_OP_SETIF && use->op != TCCIR_OP_JUMPIF)
    return NULL;

  /* A second consumer would read flags this fold does not preserve. */
  int k = j + 1;
  while (k < n && ir->compact_instructions[k].op == TCCIR_OP_NOP)
    k++;
  if (k < n)
  {
    TccIrOp next = ir->compact_instructions[k].op;
    if (next == TCCIR_OP_SELECT || next == TCCIR_OP_SETIF || next == TCCIR_OP_JUMPIF)
      return NULL;
  }
  return use;
}

/* No memory write, call or control-flow join between two points, so a value
 * read at `from` still reads the same at `to`. */
static int cef_memory_stable_between(TCCIRState *ir, int from, int to, IROperand moved)
{
  int32_t moved_vr = irop_get_vreg(moved);
  for (int k = from + 1; k < to; k++)
  {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    TccIrOp kop = kq->op;
    if (kop == TCCIR_OP_NOP)
      continue;
    if (kq->is_jump_target)
      return 0;
    if (kop == TCCIR_OP_STORE || kop == TCCIR_OP_STORE_INDEXED ||
        kop == TCCIR_OP_STORE_POSTINC || kop == TCCIR_OP_BLOCK_COPY ||
        kop == TCCIR_OP_INLINE_ASM || kop == TCCIR_OP_VLA_ALLOC ||
        kop == TCCIR_OP_FUNCCALLVOID || kop == TCCIR_OP_FUNCCALLVAL ||
        kop == TCCIR_OP_JUMP || kop == TCCIR_OP_JUMPIF || kop == TCCIR_OP_IJUMP)
      return 0;
    if (!irop_config[kop].has_dest)
      continue;
    IROperand kd = tcc_ir_op_get_dest(ir, kq);
    /* A destination naming memory mutates it like a STORE. */
    if (kd.is_lval || irop_get_tag(kd) == IROP_TAG_STACKOFF)
      return 0;
    if (moved_vr >= 0 && irop_get_vreg(kd) == moved_vr)
      return 0;
  }
  return 1;
}

/* `(x ^ y) == y`  ==>  `x == 0`  (and the mirrored/`!=` forms).
 *
 * Exact for equality because only Z is consulted: for a == (x^y) and b == y,
 * Z(a-b) is set exactly when x is zero.  It does NOT hold for the relational
 * predicates — N/C/V differ — hence the EQ/NE gate on the flag consumer.
 * Dropping the XOR usually kills the cancelled operand's load as well, which
 * is where the win comes from (element-wise `(*p ^ *q) == *q` vector masks). */
static int cef_xor_cancel(TCCIRState *ir, const uint8_t *dc, int dc_stride)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IRQuadCompact *use = cef_flag_consumer(ir, i);
    if (!use)
      continue;
    IROperand cond = (use->op == TCCIR_OP_SELECT) ? ir->iroperand_pool[use->operand_base + 3]
                                                  : tcc_ir_op_get_src1(ir, use);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    if (tok != TOK_EQ && tok != TOK_NE)
      continue;

    IROperand cs[2];
    cs[0] = tcc_ir_op_get_src1(ir, q);
    cs[1] = tcc_ir_op_get_src2(ir, q);
    if (cef_operand_is_volatile_local(ir, cs[0]) || cef_operand_is_volatile_local(ir, cs[1]))
      continue;

    /* Either CMP operand may name the XOR; the other is the cancelling term. */
    for (int side = 0; side < 2; side++)
    {
      IROperand xor_op = cs[side];
      IROperand other_op = cs[!side];

      /* The XOR result must reach the CMP as a plain single-def value. */
      int32_t xor_vr = irop_get_vreg(xor_op);
      if (xor_op.is_lval || xor_vr < 0 || irop_get_btype(xor_op) != IROP_BTYPE_INT32)
        continue;
      if (!DC_IS_SINGLE_DEF(dc, dc_stride, xor_vr))
        continue;

      int xor_def = tcc_ir_find_defining_instruction(ir, xor_vr, i);
      if (xor_def < 0)
        continue;
      IRQuadCompact *xq = &ir->compact_instructions[xor_def];
      if (xq->op != TCCIR_OP_XOR)
        continue;

      IROperand xs[2];
      xs[0] = tcc_ir_op_get_src1(ir, xq);
      xs[1] = tcc_ir_op_get_src2(ir, xq);
      /* Uniform 32-bit width throughout: a narrower view on either side would
       * make `x == 0` ask about different bits than `(x^y) == y` did. */
      if (irop_get_btype(xs[0]) != IROP_BTYPE_INT32 || irop_get_btype(xs[1]) != IROP_BTYPE_INT32 ||
          irop_get_btype(other_op) != IROP_BTYPE_INT32)
        continue;
      if (cef_operand_is_volatile_local(ir, xs[0]) || cef_operand_is_volatile_local(ir, xs[1]))
        continue;

      /* Which XOR operand does the CMP's other side re-read?  The operands are
       * usually embedded derefs of two distinct address temps, so this is an
       * expression comparison (which also proves memory stayed stable). */
      int matched = -1;
      for (int k = 0; k < 2 && matched < 0; k++)
        if (ir_opt_pure_expr_equal(ir, other_op, i, xs[k], xor_def, 0))
          matched = k;
      if (matched < 0)
        continue;

      IROperand keep = xs[!matched];
      /* Moving `keep` down from the XOR to the CMP re-reads it there, so
       * nothing in between may write memory or redefine its address. */
      if (!cef_memory_stable_between(ir, xor_def, i, keep))
        continue;

      tcc_ir_set_src1(ir, i, keep);
      tcc_ir_set_src2(ir, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
      changes++;
      LOG_IR_GEN("OPTIMIZE: (x^y) cmp y -> x cmp 0 at i=%d", i);
      break;
    }
  }

  return changes;
}

/* Standalone entry: the fusion group is what turns the embedded derefs of
 * `(*p ^ *q) == *q` into the plain-register XOR/CMP form this fold matches, so
 * it must also run after that group, not only inside cmp_expr_fold. */
int tcc_ir_opt_cmp_xor_cancel(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2 || n > 4000)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);
  int changes = cef_xor_cancel(ir, dc, dc_stride);
  tcc_free(dc);

  if (changes)
    changes += tcc_ir_opt_dce(ir);
  return changes;
}

int tcc_ir_opt_cmp_xor_cancel_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_xor_cancel(ctx->ir); }

int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  changes += cef_xor_cancel(ir, dc, dc_stride);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);

    if (cef_operand_is_volatile_local(ir, src1) ||
        cef_operand_is_volatile_local(ir, src2))
      continue;

    /* Deciding the comparison does not remove the reads it is made of.  The
     * volatile check further down only guards the branch that proves two
     * symref derefs equal; ir_opt_nonvreg_expr_equal above it reaches the same
     * conclusion first and never asked, so `g == g` on a volatile global folded
     * to 1 with both loads gone.  Ask once, for the whole instruction. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;

    int def1 = -1, def2 = -1;
    int is_equal = 0;
    int both_nonvreg = (vr1 < 0 && vr2 < 0);

    if (both_nonvreg)
    {
      is_equal = ir_opt_nonvreg_expr_equal(ir, src1, src2);
      /* Floats excluded (NaN != NaN). */
      if (!is_equal && irop_is_immediate(src1) && irop_is_immediate(src2) &&
          !src1.is_sym && !src2.is_sym &&
          irop_get_btype(src1) != IROP_BTYPE_FLOAT32 && irop_get_btype(src1) != IROP_BTYPE_FLOAT64 &&
          irop_get_btype(src2) != IROP_BTYPE_FLOAT32 && irop_get_btype(src2) != IROP_BTYPE_FLOAT64)
        is_equal = irop_get_imm64_ex(ir, src1) == irop_get_imm64_ex(ir, src2);
      if (!is_equal && src1.is_sym && src2.is_sym && !src1.is_lval && !src2.is_lval)
      {
        if (cef_symref_same(ir, src1, src2))
          is_equal = 1;
      }
      /* Non-volatile globals only; skip floats (NaN != NaN). */
      if (!is_equal && src1.is_sym && src2.is_sym && src1.is_lval && src2.is_lval)
      {
        if (cef_symref_same(ir, src1, src2))
        {
          IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
          Sym *sym = a_ref->sym;
          int ttype = sym->type.t;
          int btype = ttype & VT_BTYPE;
          if (!(ttype & VT_VOLATILE) &&
              btype != VT_FLOAT && btype != VT_DOUBLE && btype != VT_LDOUBLE)
            is_equal = 1;
        }
      }
      if (!is_equal)
        continue;
    }
    else if ((vr1 >= 0) != (vr2 >= 0))
    {
      /* Skip address-taken VARs: a store-through-pointer may change the value. */
      int32_t v_vr = (vr1 >= 0) ? vr1 : vr2;
      IROperand other = (vr1 >= 0) ? src2 : src1;
      if (DC_IS_SINGLE_DEF(dc, dc_stride, v_vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, v_vr);
        if (!interval || !interval->addrtaken)
        {
          int vdef = tcc_ir_find_defining_instruction(ir, v_vr, i);
          if (vdef >= 0)
          {
            IRQuadCompact *vdq = &ir->compact_instructions[vdef];
            if (vdq->op == TCCIR_OP_ASSIGN)
            {
              IROperand vs = tcc_ir_op_get_src1(ir, vdq);
              if (irop_get_vreg(vs) < 0)
              {
                is_equal = ir_opt_nonvreg_expr_equal(ir, vs, other);
                if (!is_equal && vs.is_sym && other.is_sym &&
                    !vs.is_lval && !other.is_lval)
                {
                  if (cef_symref_same(ir, vs, other))
                    is_equal = 1;
                }
                if (!is_equal && irop_is_immediate(vs) && irop_is_immediate(other) &&
                    !vs.is_sym && !other.is_sym)
                {
                  is_equal = irop_get_imm64_ex(ir, vs) == irop_get_imm64_ex(ir, other);
                }
              }
            }
          }
        }
      }
      if (!is_equal)
        continue;
    }
    else
    {
      if (vr1 < 0 || vr2 < 0)
        continue;

      /* Value-identity requires matching lval-ness: *(p) and p differ. */
      if (src1.is_lval != src2.is_lval)
        continue;

      if (vr1 == vr2)
      {
        /* Require non-lval and matching width/signedness for x OP x. */
        if (src1.is_lval ||
            irop_get_btype(src1) != irop_get_btype(src2) ||
            src1.is_unsigned != src2.is_unsigned)
          continue;
        is_equal = 1;
      }
      else
      {
        def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
        def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
        if (def1 < 0 || def2 < 0 || def1 == def2)
          continue;

        if (DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
          is_equal = ir_opt_pure_def_equal(ir, def1, def2, 0);
      }
    }

    if (!is_equal)
    {
      IRQuadCompact *dq1 = &ir->compact_instructions[def1];
      IRQuadCompact *dq2 = &ir->compact_instructions[def2];
      if (dq1->op == dq2->op && (dq1->op == TCCIR_OP_ADD || dq1->op == TCCIR_OP_SUB))
      {
        IROperand ds2_1 = tcc_ir_op_get_src2(ir, dq1);
        IROperand ds2_2 = tcc_ir_op_get_src2(ir, dq2);
        if (irop_is_immediate(ds2_1) && irop_is_immediate(ds2_2) &&
            irop_get_imm64_ex(ir, ds2_1) == irop_get_imm64_ex(ir, ds2_2))
        {
          IROperand base1 = tcc_ir_op_get_src1(ir, dq1);
          IROperand base2 = tcc_ir_op_get_src1(ir, dq2);
          int32_t bvr1 = irop_get_vreg(base1);
          int32_t bvr2 = irop_get_vreg(base2);

          /* Base is_lval must match: *(V) and V differ. */
          if (base1.is_lval == base2.is_lval && bvr1 >= 0 && bvr2 >= 0)
          {
            if (bvr1 == bvr2)
              is_equal = 1;
            if (!is_equal)
            {
              int bd1 = tcc_ir_find_defining_instruction(ir, bvr1, def1);
              int bd2 = tcc_ir_find_defining_instruction(ir, bvr2, def2);
              if (bd1 >= 0 && bd2 >= 0)
              {
                IRQuadCompact *bdq1 = &ir->compact_instructions[bd1];
                IRQuadCompact *bdq2 = &ir->compact_instructions[bd2];
                if ((bdq1->op == TCCIR_OP_ASSIGN || bdq1->op == TCCIR_OP_LOAD) &&
                    (bdq2->op == TCCIR_OP_ASSIGN || bdq2->op == TCCIR_OP_LOAD))
                {
                  IROperand bs1 = tcc_ir_op_get_src1(ir, bdq1);
                  IROperand bs2 = tcc_ir_op_get_src1(ir, bdq2);
                  int32_t bsvr1 = irop_get_vreg(bs1);
                  int32_t bsvr2 = irop_get_vreg(bs2);
                  if (bsvr1 >= 0 && bsvr1 == bsvr2)
                    is_equal = 1;
                  if (!is_equal && bsvr1 < 0 && bsvr2 < 0)
                    is_equal = ir_opt_nonvreg_expr_equal(ir, bs1, bs2);
                  if (!is_equal && ((bsvr1 >= 0) != (bsvr2 >= 0)))
                  {
                    int vreg_side = (bsvr1 >= 0) ? bsvr1 : bsvr2;
                    IROperand const_side = (bsvr1 >= 0) ? bs2 : bs1;
                    int vreg_def_at = (bsvr1 >= 0) ? bd1 : bd2;
                    int vdef = tcc_ir_find_defining_instruction(ir, vreg_side, vreg_def_at);
                    if (vdef >= 0)
                    {
                      IRQuadCompact *vdq = &ir->compact_instructions[vdef];
                      if (vdq->op == TCCIR_OP_ASSIGN)
                      {
                        IROperand vs = tcc_ir_op_get_src1(ir, vdq);
                        if (irop_get_vreg(vs) < 0)
                          is_equal = ir_opt_nonvreg_expr_equal(ir, vs, const_side);
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    if (!is_equal)
      continue;

    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    int folded = 0;
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      q->op = TCCIR_OP_NOP;
      if (result)
      {
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        next->op = TCCIR_OP_NOP;
      }
      changes++;
      folded = 1;
    }
    else if (next->op == TCCIR_OP_SELECT)
    {
      IROperand select_cond = ir->iroperand_pool[next->operand_base + 3];
      int tok = (int)irop_get_imm64_ex(ir, select_cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
      folded = 1;
    }
    else if (next->op == TCCIR_OP_SETIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand setif_dest = tcc_ir_op_get_dest(ir, next);
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_dest(ir, i + 1, setif_dest);
      tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, irop_get_btype(setif_dest)));
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
      folded = 1;
    }

    if (folded)
      ir_opt_setif_chain_cleanup(ir, def1, def2, vr1, vr2);
  }

  tcc_free(dc);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}
int tcc_ir_opt_cmp_expr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_expr_fold(ctx->ir); }
