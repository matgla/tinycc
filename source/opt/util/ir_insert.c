/*
 *  TCC IR - Instruction insertion with jump/switch-target fixup
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

/* Insert instruction before before_idx, shift array, patch jumps; returns index. */
int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q)
{
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    ir->compact_instructions_size = new_size;
  }
  for (int i = ir->next_instruction_index; i > before_idx; i--)
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;
  /* Patch jump targets */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)tcc_ir_op_dest_imm(ir, q);
      if (target >= before_idx)
        tcc_ir_op_set_dest_imm32(ir, q, target + 1, IROP_BTYPE_INT32);
    }
  }
  /* Patch switch-table targets (side table independent of the IR array). */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= before_idx)
      table->default_target += 1;
    if (table->targets)
    {
      for (int j = 0; j < table->num_entries; j++)
      {
        if (table->targets[j] >= before_idx)
          table->targets[j] += 1;
      }
    }
  }
  tcc_ir_frame_scope_insert(ir, before_idx);
  return before_idx;
}
