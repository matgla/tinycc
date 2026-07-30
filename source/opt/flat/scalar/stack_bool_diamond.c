/*
 *  TCC IR - Stack-bool diamond folding (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt/flat/branch.h"

/* Stack-bool-diamond peephole: fold a constant-store diamond into direct jumps. */
int tcc_ir_opt_stack_bool_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 6)
    return 0;

  LOG_IR_GEN("=== STACK BOOL DIAMOND START ===");

  /* Iterate on merge (the TEST_ZERO); match the store/jump/reload diamond shape. */
  for (int merge = 2; merge + 1 < n; merge++)
  {
    IRQuadCompact *q3 = &ir->compact_instructions[merge];
    IRQuadCompact *q4 = &ir->compact_instructions[merge + 1];

    if (q3->op != TCCIR_OP_TEST_ZERO)
      continue;
    if (q4->op != TCCIR_OP_JUMPIF)
      continue;
    if (q4->is_jump_target)
      continue;

    /* Locate q_c (fall-into-merge STORE): post-rotation at merge-1, pre-rotation at merge-2. */
    int q_c_idx = -1;
    int extra_jmp = -1; /* idx of the redundant `JUMP merge` (pre-rotation) */
    if (ir->compact_instructions[merge - 1].op == TCCIR_OP_STORE)
    {
      q_c_idx = merge - 1;
    }
    else if (merge >= 2 && ir->compact_instructions[merge - 1].op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[merge - 1]);
      if ((int)jd.u.imm32 == merge && ir->compact_instructions[merge - 2].op == TCCIR_OP_STORE)
      {
        q_c_idx = merge - 2;
        extra_jmp = merge - 1;
      }
    }
    if (q_c_idx < 0)
      continue;

    IRQuadCompact *q2 = &ir->compact_instructions[q_c_idx];

    /* q_c's dest slot must match q_d's source slot. */
    IROperand d2 = tcc_ir_op_get_dest(ir, q2);
    IROperand r3 = tcc_ir_op_get_src1(ir, q3);
    if (!stackoff_same_slot(d2, r3))
      continue;

    /* q_c's stored value must be an immediate. */
    IROperand sb = tcc_ir_op_get_src1(ir, q2);
    if (!irop_is_immediate(sb))
      continue;
    int64_t val_b = irop_get_imm64_ex(ir, sb);

    /* JUMPIF condition must be EQ or NE. */
    IROperand q4_cond = tcc_ir_op_get_src1(ir, q4);
    int cond_tok = (int)irop_get_imm64_ex(ir, q4_cond);
    if (cond_tok != 0x94 && cond_tok != 0x95)
      continue;

    /* Locate q_b: the unique unconditional JUMP targeting merge (excluding extra_jmp). */
    int q_b_idx = -1;
    int multi = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt != merge)
        continue;
      if (j == extra_jmp)
        continue;
      if (qj->op != TCCIR_OP_JUMP)
      {
        multi = 1;
        break;
      }
      if (q_b_idx >= 0)
      {
        multi = 1;
        break;
      }
      q_b_idx = j;
    }
    if (multi || q_b_idx <= 0)
      continue;

    IRQuadCompact *q1 = &ir->compact_instructions[q_b_idx];
    IRQuadCompact *q0 = &ir->compact_instructions[q_b_idx - 1];

    /* q_b must not itself be a jump target. */
    if (q1->is_jump_target)
      continue;
    if (q0->op != TCCIR_OP_STORE)
      continue;

    /* q_a must store the same slot with an immediate value. */
    IROperand d0 = tcc_ir_op_get_dest(ir, q0);
    if (!stackoff_same_slot(d0, d2))
      continue;
    IROperand sa = tcc_ir_op_get_src1(ir, q0);
    if (!irop_is_immediate(sa))
      continue;
    int64_t val_a = irop_get_imm64_ex(ir, sa);

    /* Full scan: slot referenced only by q_a/q_c/q_d, and no stray jumps into the diamond. */
    int bail = 0;
    for (int j = 0; j < n && !bail; j++)
    {
      if (j == q_b_idx - 1 || j == q_c_idx || j == merge)
        continue;
      if (extra_jmp >= 0 && j == extra_jmp)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      if (irop_config[qj->op].has_dest)
      {
        IROperand op = tcc_ir_op_get_dest(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src1)
      {
        IROperand op = tcc_ir_op_get_src1(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src2)
      {
        IROperand op = tcc_ir_op_get_src2(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }

      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF)
      {
        int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
        if (j != q_b_idx && j != extra_jmp && tgt == merge)
        {
          bail = 1;
          break;
        }
        if (tgt == q_b_idx || tgt == merge + 1)
        {
          bail = 1;
          break;
        }
        if (extra_jmp >= 0 && tgt == extra_jmp)
        {
          bail = 1;
          break;
        }
      }
    }
    if (bail)
      continue;

    /* Evaluate: JUMPIF EQ jumps when reload == 0; JUMPIF NE jumps when != 0. */
    IROperand q4_dest = tcc_ir_op_get_dest(ir, q4);
    int target_T = (int)q4_dest.u.imm32;
    int target_next = merge + 2;

    int a_jumps = (cond_tok == 0x94) ? (val_a == 0) : (val_a != 0);
    int b_jumps = (cond_tok == 0x94) ? (val_b == 0) : (val_b != 0);

    int a_target = a_jumps ? target_T : target_next;
    int b_target = b_jumps ? target_T : target_next;

    /* Rewrite q_b to JUMP directly to a_target. */
    IROperand q1_dest = tcc_ir_op_get_dest(ir, q1);
    q1_dest.u.imm32 = a_target;
    tcc_ir_set_dest(ir, q_b_idx, q1_dest);

    if (extra_jmp >= 0)
    {
      /* Pre-rotation: rewrite the redundant JUMP to b_target and NOP q_e. */
      IRQuadCompact *jq = &ir->compact_instructions[extra_jmp];
      IROperand jd = tcc_ir_op_get_dest(ir, jq);
      jd.u.imm32 = b_target;
      tcc_ir_set_dest(ir, extra_jmp, jd);
      q4->op = TCCIR_OP_NOP;
    }
    else
    {
      /* Post-rotation: rewrite q_e from JUMPIF to unconditional JUMP b_target. */
      q4_dest.u.imm32 = b_target;
      q4->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, merge + 1, q4_dest);
    }

    /* NOP the scaffolding. */
    q0->op = TCCIR_OP_NOP;
    q2->op = TCCIR_OP_NOP;
    q3->op = TCCIR_OP_NOP;

    LOG_IR_GEN(
        "STACK BOOL DIAMOND: slot=%d A=%lld B=%lld cond=0x%x T=%d next=%d a_tgt=%d b_tgt=%d a_idx=%d merge=%d %s",
        (int)d0.u.imm32, (long long)val_a, (long long)val_b, cond_tok, target_T, target_next, a_target, b_target,
        q_b_idx - 1, merge, extra_jmp >= 0 ? "(pre-rot)" : "(post-rot)");
    changes++;
  }

  LOG_IR_GEN("=== STACK BOOL DIAMOND END: %d fused ===", changes);
  return changes;
}

int tcc_ir_opt_stack_bool_diamond_ex(IROptCtx *ctx) { return tcc_ir_opt_stack_bool_diamond(ctx->ir); }
