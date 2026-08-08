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

/* ============================================================================
 * IROperand Pool Operations
 * ============================================================================ */

/* Add IROperand to pool, return index */
int tcc_ir_pool_add(TCCIRState *ir, IROperand irop)
{
  if (ir->iroperand_pool_count >= ir->iroperand_pool_capacity)
  {
    /* Guard against a zero (or negative) capacity: `0 * 2 == 0` would never
       grow the pool, and the subsequent write would overflow a zero-size
       buffer. Seed to 1 so the doubling below makes progress. */
    if (ir->iroperand_pool_capacity <= 0)
      ir->iroperand_pool_capacity = 1;
    else
      ir->iroperand_pool_capacity *= 2;
    ir->iroperand_pool = (IROperand *)tcc_realloc(ir->iroperand_pool,
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
    /* Guard against a zero (or negative) capacity: `0 * 2 == 0` forever, so
       the doubling loop below would never terminate. Seed to 1 first. */
    if (ir->iroperand_pool_capacity <= 0)
      ir->iroperand_pool_capacity = 1;
    while (ir->iroperand_pool_capacity < needed)
      ir->iroperand_pool_capacity *= 2;
    ir->iroperand_pool = (IROperand *)tcc_realloc(ir->iroperand_pool,
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
