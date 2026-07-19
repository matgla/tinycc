/*
 *  TCC IR - stack-address sequence CSE (flat, pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"



#define STACK_CSE_MAX_ENTRIES 32

typedef struct StackAddrSeq
{
  int32_t stack_offset;
  int64_t add_constant;
  int32_t result_vreg;
  int assign_idx;
  int add_idx; /* -1 when there is no ADD */
  int eliminated;
} StackAddrSeq;

int tcc_ir_opt_stack_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  StackAddrSeq seqs[STACK_CSE_MAX_ENTRIES];
  int seq_count = 0;
  int i, j, k;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== STACK ADDRESS CSE START (n=%d) ===", n);

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF || src1.is_lval)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vreg = irop_get_vreg(dest);
    int32_t stack_off = src1.u.imm32;
    int64_t add_const = 0;
    int add_idx = -1;
    int32_t final_vreg = vreg;

    if (i + 1 < n)
    {
      IRQuadCompact *qnext = &ir->compact_instructions[i + 1];
      if (qnext->op == TCCIR_OP_ADD)
      {
        IROperand nd = tcc_ir_op_get_dest(ir, qnext);
        IROperand ns1 = tcc_ir_op_get_src1(ir, qnext);
        IROperand ns2 = tcc_ir_op_get_src2(ir, qnext);
        int32_t nd_vr = irop_get_vreg(nd);
        int32_t ns1_vr = irop_get_vreg(ns1);

        if (nd_vr == vreg && ns1_vr == vreg && irop_is_immediate(ns2))
        {
          add_const = irop_get_imm64_ex(ir, ns2);
          add_idx = i + 1;
          final_vreg = nd_vr;
        }
      }
    }

    /* Bare stack-address ASSIGNs feed too many unrelated uses to merge safely. */
    if (add_idx < 0)
      continue;

    if (seq_count < STACK_CSE_MAX_ENTRIES)
    {
      seqs[seq_count].stack_offset = stack_off;
      seqs[seq_count].add_constant = add_const;
      seqs[seq_count].result_vreg = final_vreg;
      seqs[seq_count].assign_idx = i;
      seqs[seq_count].add_idx = add_idx;
      seqs[seq_count].eliminated = 0;
      seq_count++;
    }
  }

  /* Folding the pair lets the backend emit one ADD Rd, SP, #combined. */
  for (i = 0; i < seq_count; i++)
  {
    IRQuadCompact *q_assign = &ir->compact_instructions[seqs[i].assign_idx];
    IRQuadCompact *q_add = &ir->compact_instructions[seqs[i].add_idx];
    IROperand src1 = tcc_ir_op_get_src1(ir, q_assign);
    int32_t combined = seqs[i].stack_offset + (int32_t)seqs[i].add_constant;

    IROperand new_src = src1;
    new_src.u.imm32 = combined;
    tcc_ir_op_set_src1(ir, q_assign, new_src);

    q_add->op = TCCIR_OP_NOP;
    seqs[i].stack_offset = combined;
    seqs[i].add_constant = 0;
    seqs[i].add_idx = -1;
    changes++;

    LOG_IR_GEN("  FOLD: ASSIGN StackLoc[%d] + ADD #%lld → ASSIGN StackLoc[%d] at idx %d", (int)src1.u.imm32,
               (long long)seqs[i].add_constant, combined, seqs[i].assign_idx);
  }

  if (seq_count < 2)
  {
    LOG_IR_GEN("=== STACK ADDRESS CSE END: %d folds, fewer than 2 sequences ===", changes);
    return changes;
  }

  for (i = 0; i < seq_count; i++)
  {
    if (seqs[i].eliminated)
      continue;

    for (j = i + 1; j < seq_count; j++)
    {
      if (seqs[j].eliminated)
        continue;
      if (seqs[i].stack_offset != seqs[j].stack_offset)
        continue;
      if (seqs[i].add_constant != seqs[j].add_constant)
        continue;

      /* The earlier result vreg must survive unredefined up to the duplicate. */
      int first_last_def = (seqs[i].add_idx >= 0) ? seqs[i].add_idx : seqs[i].assign_idx;
      int second_start = seqs[j].assign_idx;
      int redefined = 0;

      for (k = first_last_def + 1; k < second_start; k++)
      {
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[qk->op].has_dest)
          continue;
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        if (irop_get_vreg(dk) == seqs[i].result_vreg)
        {
          redefined = 1;
          break;
        }
      }

      if (redefined)
        continue;

      LOG_IR_GEN("  CSE: seq[%d] (off=%d +%lld vreg=%d idx=%d/%d) duplicates seq[%d]", j, seqs[j].stack_offset,
                 (long long)seqs[j].add_constant, seqs[j].result_vreg, seqs[j].assign_idx, seqs[j].add_idx, i);

      ir->compact_instructions[seqs[j].assign_idx].op = TCCIR_OP_NOP;
      if (seqs[j].add_idx >= 0)
        ir->compact_instructions[seqs[j].add_idx].op = TCCIR_OP_NOP;

      /* Rewrite src1/src2 only — never dest — to avoid redirecting writes. */
      int32_t old_vr = seqs[j].result_vreg;
      int32_t new_vr = seqs[i].result_vreg;

      for (k = 0; k < n; k++)
      {
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[qk->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, qk);
          if (irop_get_vreg(s1) == old_vr)
          {
            IROperand rep = s1;
            irop_set_vreg(&rep, new_vr);
            tcc_ir_op_set_src1(ir, qk, rep);
          }
        }
        if (irop_config[qk->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, qk);
          if (irop_get_vreg(s2) == old_vr)
          {
            IROperand rep = s2;
            irop_set_vreg(&rep, new_vr);
            tcc_ir_op_set_src2(ir, qk, rep);
          }
        }
      }

      seqs[j].eliminated = 1;
      changes++;
    }
  }

  LOG_IR_GEN("=== STACK ADDRESS CSE END: %d replacements ===", changes);

  return changes;
}
