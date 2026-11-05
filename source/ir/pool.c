/*
 *  TCC IR - Operand Pool Management Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "op_props.h"

/* ============================================================================
 * IROperand Pool Operations
 * ============================================================================ */

/* Add IROperand to pool, return index */
/* A quarter more, not double: after tcc_ir_settle a pool that doubles no longer
 * fits in fast memory, right when register allocation needs it most. */
int tcc_ir_pool_capacity_for(int cap, int needed)
{
  if (needed > TCC_IR_POOL_MAX_SLOTS)
    tcc_error("function too large: more than %d IR operands", TCC_IR_POOL_MAX_SLOTS);
  while (cap < needed)
    cap += cap / 4 + 256;
  return cap < TCC_IR_POOL_MAX_SLOTS ? cap : TCC_IR_POOL_MAX_SLOTS;
}

int tcc_ir_pool_add(TCCIRState *ir, IROperand irop)
{
  if (ir->iroperand_pool_count >= ir->iroperand_pool_capacity)
  {
    ir->iroperand_pool_capacity = tcc_ir_pool_capacity_for(ir->iroperand_pool_capacity, ir->iroperand_pool_count + 1);
    ir->iroperand_pool = (IROperand *)tcc_hot_realloc(ir->iroperand_pool,
                                                    sizeof(IROperand) * ir->iroperand_pool_capacity);
    if (!ir->iroperand_pool)
    {
      fprintf(stderr, "tcc_ir_pool_add: out of memory\n");
      exit(1);
    }
  }
  ir->iroperand_pool[ir->iroperand_pool_count] = irop;
  return ir->iroperand_pool_count++;
}

/* Before a pipeline: drop operands no instruction refers to, lay the rest out in
 * instruction order, and reseat both arrays at their real size.  44% of the pool
 * is dead by the time registers are allocated, and these two arrays are most of a
 * compile's data misses on the board.  Doubling slack must not decide what fits in
 * fast memory, and a pass's scan over the instructions now walks the pool in order. */
void tcc_ir_settle(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int slots = 0;
  for (int i = 0; i < n; i++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    int k = ir_op_slot_count(q->op);
    if (ir_op_has(q->op, IROP_A_SLOT3))
      k = 4;
    slots += k;
  }
  int cap = tcc_ir_pool_capacity_for(slots + slots / 8 + 16, slots);
  IROperand *np = tcc_malloc(sizeof(IROperand) * cap);
  int wp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int k = ir_op_slot_count(q->op);
    if (ir_op_has(q->op, IROP_A_SLOT3))
      k = 4;
    if (k && (int)q->operand_base + k <= ir->iroperand_pool_count)
      memcpy(&np[wp], &ir->iroperand_pool[q->operand_base], sizeof(IROperand) * k);
    else if (k)
      for (int j = 0; j < k; j++)
        np[wp + j] = (int)q->operand_base + j < ir->iroperand_pool_count ? ir->iroperand_pool[q->operand_base + j] : IROP_NONE;
    q->operand_base = wp;
    wp += k;
  }
  tcc_hot_free(ir->iroperand_pool);
  ir->iroperand_pool = NULL;
  int qcap = n + n / 8 + 16;
  ir->compact_instructions = tcc_hot_settle(ir->compact_instructions, sizeof(IRQuadCompact) * (unsigned long)n, sizeof(IRQuadCompact) * (unsigned long)qcap);
  ir->compact_instructions_size = qcap;
  ir->iroperand_pool = tcc_hot_settle(np, sizeof(IROperand) * (unsigned long)wp, sizeof(IROperand) * (unsigned long)cap);
  ir->iroperand_pool_count = wp;
  ir->iroperand_pool_capacity = cap;
}

/* Get IROperand from pool by index */
IROperand tcc_ir_pool_get(TCCIRState *ir, int index)
{
  if (index < 0 || index >= ir->iroperand_pool_count)
  {
    IROperand empty = {0};
    return empty;
  }
  return ir->iroperand_pool[index];
}

/* Set IROperand in pool by index */
void tcc_ir_pool_set(TCCIRState *ir, int index, IROperand irop)
{
  if (index < 0 || index >= ir->iroperand_pool_count)
    return;
  ir->iroperand_pool[index] = irop;
}

/* Ensure pool has capacity for n more elements */
void tcc_ir_pool_ensure(TCCIRState *ir, int n)
{
  int needed = ir->iroperand_pool_count + n;
  if (needed > ir->iroperand_pool_capacity)
  {
    ir->iroperand_pool_capacity = tcc_ir_pool_capacity_for(ir->iroperand_pool_capacity, needed);
    ir->iroperand_pool = (IROperand *)tcc_hot_realloc(ir->iroperand_pool,
                                                    sizeof(IROperand) * ir->iroperand_pool_capacity);
    if (!ir->iroperand_pool)
    {
      fprintf(stderr, "tcc_ir_pool_ensure: out of memory\n");
      exit(1);
    }
  }
}

/* ============================================================================
 * Jump Target Management
 * ============================================================================ */

/* Set jump target address in dest operand */
void tcc_ir_pool_jump_target_set(TCCIRState *ir, int instr_idx, int target_address)
{
  IRQuadCompact *cq = &ir->compact_instructions[instr_idx];
  int pool_off = cq->operand_base;
  
  /* Update iroperand_pool */
  ir->iroperand_pool[pool_off].u.imm32 = target_address;
}

/* Get jump target address from dest operand */
int tcc_ir_pool_jump_target_get(TCCIRState *ir, int instr_idx)
{
  IRQuadCompact *cq = &ir->compact_instructions[instr_idx];
  int pool_off = cq->operand_base;
  return ir->iroperand_pool[pool_off].u.imm32;
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Add IROperand to pool - legacy name */
int tcc_ir_iroperand_pool_add(TCCIRState *ir, IROperand irop)
{
  return tcc_ir_pool_add(ir, irop);
}

/* Set jump target address - legacy name */
void tcc_ir_set_dest_jump_target(TCCIRState *ir, int instr_idx, int target_address)
{
  tcc_ir_pool_jump_target_set(ir, instr_idx, target_address);
}
