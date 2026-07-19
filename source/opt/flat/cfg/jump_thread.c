/*
 *  TCC IR - Jump threading + fall-through jump elimination
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

/* Falls back to start_idx when everything from there on is NOP. */
static int find_first_non_nop(TCCIRState *ir, int start_idx)
{
  int n = ir->next_instruction_index;
  int idx = start_idx;

  while (idx < n && ir->compact_instructions[idx].op == TCCIR_OP_NOP)
    idx++;

  return (idx < n) ? idx : start_idx;
}

static int follow_jump_chain(TCCIRState *ir, int target_idx, uint8_t *visited)
{
  int n = ir->next_instruction_index;
  int current = target_idx;
  int iterations = 0;
  const int MAX_ITERATIONS = 100;

  while (current < n && iterations < MAX_ITERATIONS)
  {
    if (visited[current])
      break;
    visited[current] = 1;

    IRQuadCompact *q = &ir->compact_instructions[current];

    if (q->op == TCCIR_OP_NOP)
    {
      current = find_first_non_nop(ir, current);
      continue;
    }

    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int next_target = (int)irop_get_imm64_ex(ir, dest);
      /* target == n is the epilogue, a valid terminal: hence > n, not >= n. */
      if (next_target < 0 || next_target > n)
        break;
      current = next_target;
      iterations++;
      continue;
    }
    break;
  }
  return current;
}

/* Post-RA pass: no phi nodes exist, so jump targets can be forwarded freely. */
int tcc_ir_opt_jump_threading(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  uint8_t *visited = tcc_mallocz(n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);
    if (target < 0 || target >= n)
      continue;

    memset(visited, 0, n);
    int new_target = follow_jump_chain(ir, target, visited);
    new_target = find_first_non_nop(ir, new_target);

    /* Retargeting a JUMPIF backward would land it inside an enclosing loop body and let downstream cleanup collapse a live loop-exit test. */
    if (q->op == TCCIR_OP_JUMPIF && new_target < target)
      new_target = target;

    if (new_target != target)
    {
      IROperand new_dest = dest;
      new_dest.u.imm32 = new_target;
      tcc_ir_op_set_dest(ir, q, new_dest);
      changes++;
    }
  }

  tcc_free(visited);
  return changes;
}

/* Removing a JUMPIF leaves its flag-setter orphaned; orphan_cmp_elim clears it in the next cascade iteration. */
int tcc_ir_opt_eliminate_fallthrough(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== ELIMINATE FALL-THROUGH START ===");

  /* i < n, not n - 1: a trailing jump to the epilogue (target == n) is a fall-through no-op too. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);

    int next_real = find_first_non_nop(ir, i + 1);

    /* Also fires when a JUMPIF and the unconditional JUMP behind it converge on one target. */
    if (target != next_real)
    {
      if (q->op != TCCIR_OP_JUMPIF || next_real >= n)
        continue;
      IRQuadCompact *nq = &ir->compact_instructions[next_real];
      if (nq->op != TCCIR_OP_JUMP)
        continue;
      IROperand nd = tcc_ir_op_get_dest(ir, nq);
      int next_target = (int)irop_get_imm64_ex(ir, nd);
      if (next_target != target)
        continue;
    }

    /* Dropping a JUMPIF orphans its CMP; past an impure CALL that lets TCC's non-aliasing-aware const prop misfold a later memory CMP. */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      int safe = 0;
      if (next_real >= n)
      {
        /* Both arms reach the epilogue: nothing follows that could be misfolded. */
        safe = 1;
      }
      else if (next_real >= 0 && next_real < n)
      {
        int nop = ir->compact_instructions[next_real].op;
        if (nop == TCCIR_OP_JUMP || nop == TCCIR_OP_RETURNVALUE ||
            nop == TCCIR_OP_RETURNVOID || nop == TCCIR_OP_TRAP)
          safe = 1;
      }
      if (!safe)
      {
        safe = 1;
        for (int j = i - 1; j >= 0; j--)
        {
          IRQuadCompact *pq = &ir->compact_instructions[j];
          if (pq->op == TCCIR_OP_NOP)
            continue;
          /* A jump_target heads the basic block; don't reason across it. */
          if (pq->is_jump_target)
            break;
          if (pq->op == TCCIR_OP_FUNCCALLVAL || pq->op == TCCIR_OP_FUNCCALLVOID)
          {
            Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, pq));
            const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
            if (!name || (!tcc_ir_is_pure_aeabi(name) &&
                          !ir_opt_is_pure_helper_name(name) &&
                          !ir_opt_is_flag_cmp_helper_name(name)))
            {
              safe = 0;
              break;
            }
          }
        }
      }
      if (!safe)
        continue;
    }

    LOG_IR_GEN("FALLTHROUGH: Eliminated %s at %d (target %d)",
               q->op == TCCIR_OP_JUMP ? "JUMP" : "JUMPIF", i, target);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  LOG_IR_GEN("=== ELIMINATE FALL-THROUGH END: %d jumps eliminated ===", changes);

  return changes;
}

int tcc_ir_opt_eliminate_fallthrough_ex(IROptCtx *ctx) { return tcc_ir_opt_eliminate_fallthrough(ctx->ir); }
