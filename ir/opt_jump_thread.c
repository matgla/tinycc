/*
 *  TCC IR - Jump Threading Optimization
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

/* ============================================================================
 * Jump Threading Optimization (Phase 2c)
 * ============================================================================
 *
 * This pass optimizes control flow by:
 * 1. Forwarding jump targets through NOPs to the next real instruction
 * 2. Following chains of unconditional jumps
 * 3. Eliminating fall-through jumps (jumps to the next instruction)
 *
 * Example before:
 *   JMP to 5       ; jump to a NOP
 *   ...
 *   5: NOP
 *   6: ADD ...
 *
 * After:
 *   JMP to 6       ; jump directly to the real instruction
 *   ...
 *   5: NOP
 *   6: ADD ...
 */

/* Find the first non-NOP instruction at or after the given index.
 * Returns the index of the first real instruction, or the original index
 * if all remaining instructions are NOP.
 */
static int find_first_non_nop(TCCIRState *ir, int start_idx)
{
  int n = ir->next_instruction_index;
  int idx = start_idx;

  while (idx < n && ir->compact_instructions[idx].op == TCCIR_OP_NOP)
    idx++;

  return (idx < n) ? idx : start_idx;
}

/* Follow a chain of unconditional jumps to find the ultimate target.
 * Returns the final target index, or the original target if a cycle is detected
 * or the target has multiple predecessors.
 */
static int follow_jump_chain(TCCIRState *ir, int target_idx, uint8_t *visited)
{
  int n = ir->next_instruction_index;
  int current = target_idx;
  int iterations = 0;
  const int MAX_ITERATIONS = 100; /* Prevent infinite loops */

  while (current < n && iterations < MAX_ITERATIONS)
  {
    /* Mark current as visited to detect cycles */
    if (visited[current])
      break;
    visited[current] = 1;

    IRQuadCompact *q = &ir->compact_instructions[current];

    /* If it's a NOP, skip to next */
    if (q->op == TCCIR_OP_NOP)
    {
      current = find_first_non_nop(ir, current);
      continue;
    }

    /* If it's an unconditional jump, follow it */
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int next_target = (int)irop_get_imm64_ex(ir, dest);

      /* Validate target */
      if (next_target < 0 || next_target >= n)
        break;

      current = next_target;
      iterations++;
      continue;
    }

    /* Found a real instruction that's not a jump - this is our target */
    break;
  }

  return current;
}

/* ============================================================================
 * Jump Threading - Forward jump targets through NOPs and jump chains
 * ============================================================================ */
int tcc_ir_opt_jump_threading(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== JUMP THREADING START ===");

  /* Allocate visited array for cycle detection */
  uint8_t *visited = tcc_mallocz(n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);

    /* Validate target */
    if (target < 0 || target >= n)
      continue;

    /* Clear visited array for this chain following */
    memset(visited, 0, n);

    /* Find the ultimate target by following NOPs and jump chains */
    int new_target = follow_jump_chain(ir, target, visited);

    /* Also skip NOPs at the new target itself */
    new_target = find_first_non_nop(ir, new_target);

    if (new_target != target)
    {
      IROperand new_dest = dest;
      new_dest.u.imm32 = new_target;
      tcc_ir_op_set_dest(ir, q, new_dest);

      LOG_IR_GEN("JUMP_THREAD: %d -> %d (was %d)", i, new_target, target);
      changes++;
    }
  }

  tcc_free(visited);

  LOG_IR_GEN("=== JUMP THREADING END: %d jumps threaded ===", changes);

  return changes;
}

/* ============================================================================
 * Eliminate Fall-Through Jumps
 * ============================================================================
 *
 * Remove unconditional jumps that target the next instruction.
 * These jumps are redundant since execution would fall through anyway.
 */
int tcc_ir_opt_eliminate_fallthrough(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== ELIMINATE FALL-THROUGH START ===");

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);

    /* Find the next non-NOP instruction after this one */
    int next_real = find_first_non_nop(ir, i + 1);

    /* If jump target equals the next real instruction, eliminate it */
    if (target == next_real)
    {
      q->op = TCCIR_OP_NOP;

      LOG_IR_GEN("FALLTHROUGH: Eliminated JUMP at %d (target %d)", i, target);
      changes++;
    }
  }

  LOG_IR_GEN("=== ELIMINATE FALL-THROUGH END: %d jumps eliminated ===", changes);

  return changes;
}

int tcc_ir_opt_jump_threading_ex(IROptCtx *ctx) { return tcc_ir_opt_jump_threading(ctx->ir); }
int tcc_ir_opt_eliminate_fallthrough_ex(IROptCtx *ctx) { return tcc_ir_opt_eliminate_fallthrough(ctx->ir); }
