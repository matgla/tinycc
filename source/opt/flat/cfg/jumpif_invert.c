/*
 *  TCC IR - JUMPIF inversion over an immediately-following unconditional jump
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
#include "opt_engine.h"
#include "opt_utils.h"

/* allow_backward=0: inverting over a backward latch JUMP destroys the body->latch JUMP loop rotation matches on (pr112581-1). */
int tcc_ir_opt_jumpif_invert(TCCIRState *ir, int allow_backward)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int a = (int)irop_get_imm64_ex(ir, dest);
    if (a < 0 || a > n)
      continue;

    int jmp_idx = i + 1;
    while (jmp_idx < n && ir->compact_instructions[jmp_idx].op == TCCIR_OP_NOP)
      jmp_idx++;
    if (jmp_idx >= n)
      continue;
    IRQuadCompact *jq = &ir->compact_instructions[jmp_idx];
    if (jq->op != TCCIR_OP_JUMP)
      continue;

    int after = jmp_idx + 1;
    while (after < n && ir->compact_instructions[after].op == TCCIR_OP_NOP)
      after++;
    if (after != a)
      continue;

    if (!allow_backward &&
        (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq)) < i)
      continue;

    int inv = invert_condition((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q)));
    if (inv < 0)
      continue;

    /* An edge landing on the JUMP or a NOP before it would be silently rerouted onto A by the NOPing. */
    int targeted = 0;
    for (int j = 0; j < n && !targeted; j++)
    {
      IRQuadCompact *tq = &ir->compact_instructions[j];
      if (j != i && (tq->op == TCCIR_OP_JUMP || tq->op == TCCIR_OP_JUMPIF))
      {
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, tq));
        if (t > i && t <= jmp_idx)
          targeted = 1;
      }
    }
    for (int t = 0; t < ir->num_switch_tables && !targeted; t++)
    {
      TCCIRSwitchTable *tbl = &ir->switch_tables[t];
      if (tbl->default_target > i && tbl->default_target <= jmp_idx)
        targeted = 1;
      for (int j = 0; j < tbl->num_entries && !targeted; j++)
        if (tbl->targets[j] > i && tbl->targets[j] <= jmp_idx)
          targeted = 1;
    }
    if (targeted)
      continue;

    tcc_ir_op_set_dest(ir, q, tcc_ir_op_get_dest(ir, jq));
    IROperand new_cond = irop_make_imm32(-1, inv, VT_INT);
    tcc_ir_op_set_src1(ir, q, new_cond);
    jq->op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}
