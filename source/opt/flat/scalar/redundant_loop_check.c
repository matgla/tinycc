/*
 *  TCC IR - Variable-to-Temp Promotion & Forwarding
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
#include "opt_du.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"


int tcc_ir_opt_redundant_loop_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    int32_t guard_vreg = -1;
    int64_t guard_const = 0;
    int guard_body_fact = -1;
    int guard_cmp_idx = -1;

    for (int i = loop->header_idx; i <= loop->header_idx + 4 && i <= loop->end_idx && i < n - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op == TCCIR_OP_NOP)
        continue;
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      int32_t vr = irop_get_vreg(s1);
      if (vr < 0)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);
      int target = (int)jdest.u.imm32;

      if (target > loop->end_idx || target < loop->start_idx)
      {
        int neg = vrp_negate_cmp_tok(cond);
        if (neg >= 0)
        {
          guard_cmp_idx = i;
          guard_vreg = vr;
          guard_const = irop_get_imm64_ex(ir, s2);
          guard_body_fact = neg;
          break;
        }
      }
    }

    if (guard_body_fact < 0)
      continue;

    /* The guard fact holds up to the exit target, which may lie past the
     * back-edge (body blocks reached via a forward JMP), so bound the scan
     * by the exit target rather than end_idx. */
    int exit_target = -1;
    {
      int gi = guard_cmp_idx + 1;
      while (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_NOP)
        gi++;
      if (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_JUMPIF)
      {
        IROperand gd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[gi]);
        exit_target = (int)gd.u.imm32;
      }
    }
    int scan_end = (exit_target > 0) ? exit_target - 1 : loop->end_idx;

    for (int i = loop->start_idx; i <= scan_end && i < n - 1; i++)
    {
      if (i == guard_cmp_idx)
        continue;

      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      if (irop_get_imm64_ex(ir, s2) != guard_const)
        continue;

      int32_t inner_vr = irop_get_vreg(s1);
      if (inner_vr < 0)
        continue;

      int vreg_match = (inner_vr == guard_vreg);
      if (!vreg_match)
      {
        int def_idx = tcc_ir_find_defining_instruction(ir, inner_vr, i);
        if (def_idx >= 0)
        {
          IRQuadCompact *dq = &ir->compact_instructions[def_idx];
          if (dq->op == TCCIR_OP_STORE || dq->op == TCCIR_OP_ASSIGN)
          {
            IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
            if (irop_get_vreg(dsrc) == guard_vreg)
              vreg_match = 1;
          }
        }
      }
      if (!vreg_match)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int inner_cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);

      if (vrp_cmp_implies(guard_body_fact, inner_cond))
      {
        cq->op = TCCIR_OP_NOP;
        jq->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, j, jdest);
        changes++;
      }
      else
      {
        int neg_inner = vrp_negate_cmp_tok(inner_cond);
        if (neg_inner >= 0 && vrp_cmp_implies(guard_body_fact, neg_inner))
        {
          cq->op = TCCIR_OP_NOP;
          jq->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }
  }

  tcc_ir_free_loops(loops);
  return changes;
}
int tcc_ir_opt_redundant_loop_check_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_loop_check(ctx->ir); }
