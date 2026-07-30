/*
 *  TCC IR - Decrement-to-zero loop rewrite
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"


/* Count-up pure-counter loop -> count-down-to-zero so latch SUB+CMP#0 fuses to SUBS; returns 1 if rewritten. */
int dtz_try_region(TCCIRState *ir, int start, int end, int header_idx,
                   int preheader_idx)
{
  int n = ir->next_instruction_index;

  /* Find a simple count-up IV: V = V + 1 in the latch */
  int iv_def_idx = -1;
  int32_t iv_vr = -1;

  for (int i = end; i >= start; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMPIF)
      continue;
    if (q->op != TCCIR_OP_ADD)
      break;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int d_vr = irop_get_vreg(dest);
    int s1_vr = irop_get_vreg(src1);
    if (d_vr >= 0 && d_vr == s1_vr && irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 1 &&
        TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
    {
      iv_def_idx = i;
      iv_vr = d_vr;
      break;
    }
    break;
  }

  if (iv_def_idx < 0)
    return 0;

  /* Find the back-edge CMP: CMP V, #limit; JUMPIF <S body */
  int be_cmp_idx = -1;
  int be_jmpif_idx = -1;
  int limit_val = 0;

  for (int i = end; i >= end - 5 && i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s1) != iv_vr || !irop_is_immediate(s2))
      continue;

    int jq_idx = i + 1;
    while (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP)
      jq_idx++;
    if (jq_idx >= n || ir->compact_instructions[jq_idx].op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond = tcc_ir_op_get_src1(ir, &ir->compact_instructions[jq_idx]);
    int cond_tok = (int)irop_get_imm64_ex(ir, cond);
    if (cond_tok != 0x9c) /* TOK_LT (<S) */
      continue;

    limit_val = (int)irop_get_imm64_ex(ir, s2);
    if (limit_val <= 0)
      continue;

    be_cmp_idx = i;
    be_jmpif_idx = jq_idx;
    break;
  }

  if (be_cmp_idx < 0)
    return 0;

  /* Find the IV init: V = #0 in the preheader */
  int init_idx = -1;
  for (int i = preheader_idx; i >= 0 && i >= preheader_idx - 5; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(dest) == iv_vr && irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
    {
      init_idx = i;
      break;
    }
  }

  if (init_idx < 0)
    return 0;

  /* Pre-test guard near header; skip index coinciding with back-edge CMP/JUMPIF else deletes only back-edge test (docs/bugs.md #12) */
  int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
  {
    int scan_start = preheader_idx;
    if (scan_start < 0)
      scan_start = 0;
    for (int i = scan_start; i <= header_idx + 2 && i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_CMP)
        continue;
      if (i == be_cmp_idx)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s1) != iv_vr)
        continue;

      int jq_idx = i + 1;
      while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                            ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
        jq_idx++;
      if (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_JUMPIF)
      {
        if (jq_idx == be_jmpif_idx)
          continue;
        hdr_cmp_idx = i;
        hdr_jmpif_idx = jq_idx;
        break;
      }
    }
  }

  /* IV (and optional copy-through temp) must have no other uses across its live range */
  {
    int other_uses = 0;
    int copy_through_vr = -1;

    for (int k = iv_def_idx - 1; k >= iv_def_idx - 3 && k >= 0; k--)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == iv_vr)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          copy_through_vr = irop_get_vreg(d);
        }
      }
      break;
    }

    if (copy_through_vr >= 0)
    {
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || i == iv_def_idx)
          continue;
        if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == copy_through_vr)
        {
          other_uses++;
          break;
        }
        if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == copy_through_vr)
        {
          other_uses++;
          break;
        }
      }
    }

    /* Live range ends at next post-loop redef of iv_vr (exclusive) */
    int live_end = n;
    for (int i = end + 1; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == iv_vr && !irop_op_is_lval(d))
        {
          live_end = i;
          break;
        }
      }
    }

    for (int i = 0; i < live_end && other_uses == 0; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (i == init_idx || i == iv_def_idx || i == be_cmp_idx || i == hdr_cmp_idx)
        continue;

      if (copy_through_vr >= 0 && q->op == TCCIR_OP_ASSIGN && i >= iv_def_idx - 3 && i < iv_def_idx)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == iv_vr)
          continue;
      }

      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == iv_vr)
        other_uses++;
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == iv_vr)
        other_uses++;
    }

    if (other_uses > 0)
      return 0;
  }

  /* Need a separate pre-test guard, else init=#limit would skip the loop */
  if (hdr_cmp_idx < 0)
    return 0;

  /* Apply transformation in place */

  /* 1. Init: V = #0  ->  V = #limit */
  {
    IRQuadCompact *q = &ir->compact_instructions[init_idx];
    IROperand new_init = irop_make_imm32(-1, limit_val, IROP_BTYPE_INT32);
    tcc_ir_op_set_src1(ir, q, new_init);
  }

  /* 2. Increment: V = V + 1  ->  V = V - 1 */
  {
    IRQuadCompact *q = &ir->compact_instructions[iv_def_idx];
    q->op = TCCIR_OP_SUB;
    IROperand new_step = irop_make_imm32(-1, 1, IROP_BTYPE_INT32);
    tcc_ir_op_set_src2(ir, q, new_step);
  }

  /* 3. Back-edge: CMP V, #limit  ->  CMP V, #0 */
  {
    IRQuadCompact *q = &ir->compact_instructions[be_cmp_idx];
    IROperand zero = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    tcc_ir_op_set_src2(ir, q, zero);
  }

  /* 4. Back-edge condition: <S  ->  != (SUB+CMP#0 peephole is Z-flag EQ/NE). */
  {
    IRQuadCompact *q = &ir->compact_instructions[be_jmpif_idx];
    IROperand new_cond = irop_make_imm32(-1, 0x95, IROP_BTYPE_INT32); /* TOK_NE (!=) */
    tcc_ir_op_set_src1(ir, q, new_cond);
  }

  /* 5. NOP the pre-test guard (always passes since limit > 0) */
  ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
  ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;

  return 1;
}
