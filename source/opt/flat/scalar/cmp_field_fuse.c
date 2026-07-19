/*
 *  TCC IR - Aggregate field-compare fusion (flat, pre-SSA)
 *
 *  Collapses `a.f1 != b.f1 || a.f2 != b.f2 || ...` (a run of >=2 bitfield-extract
 *  CMPs all branching "!=" to one label) into `t = A^B; t &= union_mask; CMP t,#0`.
 *  Sound signed and unsigned since != is bit-pattern inequality over disjoint field
 *  masks. Runs before the fusion group folds a side's final shift into the CMP.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"

#ifndef LOG_CMPFUSE
#ifdef TCC_LOG_CMPFUSE
#define LOG_CMPFUSE(...) fprintf(stderr, "[CMPFUSE] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_CMPFUSE(...) ((void)0)
#endif
#endif

/* Two operands denote the same base word: same TEMP/PARAM vreg, or the same
 * memory lvalue (same symbol + addend, same width/deref). */
static int cmpf_same_base(TCCIRState *ir, IROperand a, IROperand b)
{
  if (a.is_lval != b.is_lval)
    return 0;
  if (irop_get_btype(a) != irop_get_btype(b))
    return 0;
  int32_t va = irop_get_vreg(a), vb = irop_get_vreg(b);
  if (va >= 0 || vb >= 0)
    return va >= 0 && va == vb;
  if (a.tag != b.tag)
    return 0;
  if (a.tag == IROP_TAG_SYMREF)
  {
    IRPoolSymref *ra = irop_get_symref_ex(ir, a);
    IRPoolSymref *rb = irop_get_symref_ex(ir, b);
    return ra && rb && ra->sym == rb->sym && ra->addend == rb->addend;
  }
  return a.u.imm32 == b.u.imm32;
}

/* Trace a CMP operand back through one bitfield-extract chain to its base word,
 * accumulating the field mask: AND #m; (x SHL a) SHR b; x SHR s. Unrecognised ->
 * whole-word (base = operand, mask = 0xffffffff). Always returns 1; the caller
 * decides whether the bases line up. */
static int cmpf_trace(TCCIRState *ir, IROperand op, int before_idx, IROperand *base, uint32_t *mask)
{
  *base = op;
  *mask = 0xffffffffu;

  if (op.is_lval)
    return 1; /* memory operand used directly: whole word */
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 1; /* immediate: whole value */
  int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
  if (d < 0)
    return 1;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  if (irop_get_btype(tcc_ir_op_get_dest(ir, dq)) != IROP_BTYPE_INT32)
    return 1;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  IROperand s2 = tcc_ir_op_get_src2(ir, dq);

  if (dq->op == TCCIR_OP_AND && irop_is_immediate(s2) && !s2.is_sym)
  {
    *base = s1;
    *mask = (uint32_t)irop_get_imm64_ex(ir, s2);
    return 1;
  }
  if (dq->op == TCCIR_OP_SHR && irop_is_immediate(s2) && !s2.is_sym)
  {
    int s = (int)irop_get_imm64_ex(ir, s2);
    if (s < 0 || s > 31)
      return 1;
    /* Look for a feeding SHL (mid/high field extract). */
    if (!s1.is_lval)
    {
      int32_t v1 = irop_get_vreg(s1);
      if (v1 >= 0)
      {
        int d2 = tcc_ir_find_defining_instruction(ir, v1, d);
        if (d2 >= 0)
        {
          IRQuadCompact *dq2 = &ir->compact_instructions[d2];
          IROperand s2b = tcc_ir_op_get_src2(ir, dq2);
          if (dq2->op == TCCIR_OP_SHL && irop_is_immediate(s2b) && !s2b.is_sym &&
              irop_get_btype(tcc_ir_op_get_dest(ir, dq2)) == IROP_BTYPE_INT32)
          {
            int a = (int)irop_get_imm64_ex(ir, s2b);
            if (a >= 0 && a <= s)
            {
              int width = 32 - s;
              uint32_t m = (width >= 32) ? 0xffffffffu : ((1u << width) - 1u);
              *base = tcc_ir_op_get_src1(ir, dq2);
              *mask = m << (s - a);
              return 1;
            }
          }
        }
      }
    }
    *base = s1;
    *mask = 0xffffffffu << s;
    return 1;
  }
  return 1; /* unrecognised: whole-word */
}

/* Is instruction k a pure bitfield-extract feeder (safe to sit between fused
 * compare units / safe to NOP)? */
static int cmpf_is_extract_op(TccIrOp op)
{
  return op == TCCIR_OP_AND || op == TCCIR_OP_SHL || op == TCCIR_OP_SHR ||
         op == TCCIR_OP_NOP || op == TCCIR_OP_ASSIGN;
}

int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i + 1 < n; i++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_CMP)
      continue;
    IRQuadCompact *jq = &ir->compact_instructions[i + 1];
    if (jq->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand jcond = tcc_ir_op_get_src1(ir, jq);
    if (!irop_is_immediate(jcond) || (int)irop_get_imm64_ex(ir, jcond) != TOK_NE)
      continue;
    int target = tcc_ir_op_get_dest(ir, jq).u.imm32;

    /* Trace the first unit's two sides to their base words + field masks. */
    IROperand baseA, baseB;
    uint32_t mA, mB;
    cmpf_trace(ir, tcc_ir_op_get_src1(ir, cq), i, &baseA, &mA);
    cmpf_trace(ir, tcc_ir_op_get_src2(ir, cq), i, &baseB, &mB);
    if (mA != mB)
      continue; /* asymmetric extract — not a clean field compare */
    if (mA == 0)
      continue;

    /* Walk forward collecting further units with the same bases + target. */
    uint32_t union_mask = mA;
    int last_cmp = i, last_jmp = i + 1;
    int units = 1;
    int scan = i + 2;
    while (scan + 1 < n)
    {
      /* Skip extract feeders between units; bail on anything else / labels. */
      int k = scan;
      while (k < n && ir->compact_instructions[k].op != TCCIR_OP_CMP)
      {
        if (ir->compact_instructions[k].is_jump_target)
          break;
        if (!cmpf_is_extract_op(ir->compact_instructions[k].op))
          break;
        k++;
      }
      if (k + 1 >= n || ir->compact_instructions[k].op != TCCIR_OP_CMP)
        break;
      if (ir->compact_instructions[k].is_jump_target)
        break;
      IRQuadCompact *ck = &ir->compact_instructions[k];
      IRQuadCompact *jk = &ir->compact_instructions[k + 1];
      if (jk->op != TCCIR_OP_JUMPIF || ck->is_jump_target || jk->is_jump_target)
        break;
      IROperand jc = tcc_ir_op_get_src1(ir, jk);
      if (!irop_is_immediate(jc) || (int)irop_get_imm64_ex(ir, jc) != TOK_NE)
        break;
      if (tcc_ir_op_get_dest(ir, jk).u.imm32 != target)
        break;
      IROperand bA, bB;
      uint32_t kmA, kmB;
      cmpf_trace(ir, tcc_ir_op_get_src1(ir, ck), k, &bA, &kmA);
      cmpf_trace(ir, tcc_ir_op_get_src2(ir, ck), k, &bB, &kmB);
      if (kmA != kmB || kmA == 0)
        break;
      if (!cmpf_same_base(ir, bA, baseA) || !cmpf_same_base(ir, bB, baseB))
        break;
      union_mask |= kmA;
      last_cmp = k;
      last_jmp = k + 1;
      units++;
      scan = k + 2;
    }

    if (units < 2)
      continue;

    /* Need two free slots immediately before last_cmp for XOR (+ AND). */
    int need_and = (union_mask != 0xffffffffu);
    int xor_slot = last_cmp - (need_and ? 2 : 1);
    if (xor_slot <= i) /* must stay within the fused span */
      continue;

    int32_t tx = tcc_ir_get_vreg_temp(ir);
    IROperand txv = irop_make_vreg(tx, IROP_BTYPE_INT32);

    /* NOP the whole span [i .. last_cmp-1]; we rebuild into the tail slots. */
    for (int z = i; z < last_cmp; z++)
      ir->compact_instructions[z].op = TCCIR_OP_NOP;

    /* xor = baseA ^ baseB */
    ir->compact_instructions[xor_slot].op = TCCIR_OP_XOR;
    tcc_ir_op_set_dest(ir, &ir->compact_instructions[xor_slot], txv);
    tcc_ir_set_src1(ir, xor_slot, baseA);
    tcc_ir_set_src2(ir, xor_slot, baseB);

    IROperand cmp_lhs = txv;
    if (need_and)
    {
      int32_t tm = tcc_ir_get_vreg_temp(ir);
      IROperand tmv = irop_make_vreg(tm, IROP_BTYPE_INT32);
      ir->compact_instructions[xor_slot + 1].op = TCCIR_OP_AND;
      tcc_ir_op_set_dest(ir, &ir->compact_instructions[xor_slot + 1], tmv);
      tcc_ir_set_src1(ir, xor_slot + 1, txv);
      tcc_ir_set_src2(ir, xor_slot + 1, irop_make_imm32(-1, (int32_t)union_mask, IROP_BTYPE_INT32));
      cmp_lhs = tmv;
    }

    /* last CMP -> CMP cmp_lhs, #0 ; keep the last JUMPIF (!= -> target). */
    ir->compact_instructions[last_cmp].op = TCCIR_OP_CMP;
    tcc_ir_set_src1(ir, last_cmp, cmp_lhs);
    tcc_ir_set_src2(ir, last_cmp, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));

    LOG_CMPFUSE("fused %d field compares @%d..%d -> XOR&%#x at @%d (target %d)",
                units, i, last_jmp, union_mask, xor_slot, target);
    changes++;
    i = last_jmp; /* continue after the fused run */
  }

  return changes;
}

int tcc_ir_opt_cmp_field_fuse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_cmp_field_fuse(ctx->ir);
}
