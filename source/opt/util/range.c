/*
 *  TCC IR - Hazard queries over instructions and ranges, block cursors
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_range.h"
#include "opt_utils.h"

static int ir_q_reads_through(IROperand op)
{
  return op.is_lval || op.is_llocal;
}

static uint32_t ir_q_instr_hazards(TCCIRState *ir, const IRQuadCompact *q, uint32_t mask)
{
  uint32_t hz = 0;
  int op = q->op;

  if ((mask & IR_HZ_JOIN) && q->is_jump_target)
    hz |= IR_HZ_JOIN;
  if ((mask & (IR_HZ_DEST_LVAL | IR_HZ_DEST_STACKOFF)) && irop_config[op].has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval)
      hz |= IR_HZ_DEST_LVAL;
    if (irop_get_tag(d) == IROP_TAG_STACKOFF)
      hz |= IR_HZ_DEST_STACKOFF;
  }
  if (mask & IR_HZ_SRC_LVAL)
  {
    if ((irop_config[op].has_src1 && ir_q_reads_through(tcc_ir_op_get_src1(ir, q))) ||
        (irop_config[op].has_src2 && ir_q_reads_through(tcc_ir_op_get_src2(ir, q))) ||
        (ir_op_has(op, IROP_A_SLOT3) && ir_q_reads_through(ir->iroperand_pool[q->operand_base + 3])))
      hz |= IR_HZ_SRC_LVAL;
  }
  if ((mask & IR_HZ_VOLATILE) && tcc_ir_instr_access_is_volatile(ir, q))
    hz |= IR_HZ_VOLATILE;
  return hz & mask;
}

uint32_t ir_q_hazards(TCCIRState *ir, const IRQuadCompact *q, uint32_t mask)
{
  uint32_t hz = ir_op_props[q->op] & IR_HZ_FROM_OP & mask;
  if (mask & IR_HZ_FROM_INSTR)
    hz |= ir_q_instr_hazards(ir, q, mask);
  return hz;
}

uint32_t ir_q_hazards_except(TCCIRState *ir, const IRQuadCompact *q, uint32_t mask, IROpSet except)
{
  uint32_t hz = ir_opset_has(except, q->op) ? 0 : ir_op_props[q->op] & IR_HZ_FROM_OP & mask;
  if (mask & IR_HZ_FROM_INSTR)
    hz |= ir_q_instr_hazards(ir, q, mask);
  return hz;
}

int ir_range_first_hazard_except(TCCIRState *ir, int lo, int hi, uint32_t mask, IROpSet except)
{
  if (hi < lo)
    return lo;
  for (int k = lo + 1; k < hi; k++)
  {
    if (ir_q_hazards_except(ir, &ir->compact_instructions[k], mask & ~IR_HZ_JOIN_END, except))
      return k;
  }
  if ((mask & IR_HZ_JOIN_END) && hi > lo && ir->compact_instructions[hi].is_jump_target)
    return hi;
  return -1;
}

int ir_range_first_hazard(TCCIRState *ir, int lo, int hi, uint32_t mask)
{
  return ir_range_first_hazard_except(ir, lo, hi, mask, IROPSET_NONE);
}

int ir_bb_next(TCCIRState *ir, int i, int limit)
{
  if (ir_op_has(ir->compact_instructions[i].op, IROP_ENDS_BLOCK))
    return -1;
  for (int j = i + 1; j < limit; j++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->is_jump_target) /* before the NOP skip: a NOP can be the join */
      return -1;
    if (q->op != TCCIR_OP_NOP)
      return j;
  }
  return -1;
}

int ir_bb_prev(TCCIRState *ir, int i, int limit)
{
  for (int j = i - 1; j >= limit; j--)
  {
    const IRQuadCompact *q = &ir->compact_instructions[j];
    if (ir->compact_instructions[j + 1].is_jump_target)
      return -1;
    if (q->op == TCCIR_OP_NOP)
      continue;
    return ir_op_has(q->op, IROP_ENDS_BLOCK) ? -1 : j;
  }
  return -1;
}
