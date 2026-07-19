/*
 *  TCC IR - Loop IR mutation primitives
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


/* Insert an instr at pos, shifting later instrs and fixing jump targets; returns pos. */
int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  int n = ir->next_instruction_index;

  if (n + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, new_size * sizeof(IRQuadCompact));
    ir->compact_instructions_size = new_size;
  }

  if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
  {
    tcc_ir_pool_ensure(ir, 3);
    if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
    {
      if (TCC_LOG_IV_SR)
        fprintf(stderr, "[IV_SR] ERROR: iroperand_pool_capacity limit reached\n");
      return -1;
    }
  }

  for (int i = n; i > pos; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }
  ir->next_instruction_index++;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (i == pos)
      continue; /* skip the newly inserted slot */
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jdest);
      if (target >= pos)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[ti];
    if (table->default_target >= pos)
      table->default_target++;
    for (int tj = 0; tj < table->num_entries; tj++)
    {
      if (table->targets[tj] >= pos)
        table->targets[tj]++;
    }
  }

  IRQuadCompact *new_q = &ir->compact_instructions[pos];
  new_q->op = op;
  /* Fresh orig_index (bump max) so side tables sized max_orig_index+1 stay covered. */
  new_q->orig_index = ++ir->max_orig_index;
  new_q->is_jump_target = 0;
  new_q->no_unroll = 0;
  new_q->line_num = 0;
  new_q->operand_base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, src1);
  tcc_ir_pool_add(ir, src2);

  return pos;
}

/* Write an instr into a NOP slot at pos; slot MUST already be NOP. */
void write_instr_at_nop(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = op;
  q->is_jump_target = 0;
  /* Only write operand slots the instr uses, so pool layout matches codegen. */
  int base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
  q->operand_base = base;
}

/* Write a SELECT into a NOP slot; 4 pool entries: dest, then, else, cond at base+3. */
void write_select_at_nop(TCCIRState *ir, int pos, IROperand dest, IROperand then_val,
                                IROperand else_val, int cond_tok)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = TCCIR_OP_SELECT;
  q->is_jump_target = 0;
  IROperand cond_op = irop_make_imm32(-1, cond_tok, IROP_BTYPE_INT32);
  int base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, then_val);
  tcc_ir_pool_add(ir, else_val);
  tcc_ir_pool_add(ir, cond_op);
  q->operand_base = base;
}
