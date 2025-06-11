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
#include "opt_utils.h"

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

      /* Validate target.  target == n is the epilogue (one past the last
       * instruction): a valid terminal, so follow the chain into it — this
       * lets a conditional branch whose arms both reach the epilogue (e.g.
       * `cond ? f() : 0;` with the result discarded) be threaded to a single
       * common target and then collapsed. */
      if (next_target < 0 || next_target > n)
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

    /* A CONDITIONAL branch (JUMPIF) must not have its taken edge retargeted
     * BACKWARD by chain-following.  Although chasing an unconditional-JUMP
     * chain is locally value-preserving, retargeting a conditional edge onto
     * an EARLIER instruction lands it inside an enclosing loop body, where the
     * not-taken (fall-through) edge also reaches it via the loop back-edge; the
     * downstream branch-cleanup cascade then sees both arms "converge" and
     * collapses what is actually a live loop-exit test.  That dropped the
     * `i < cfg->num_blocks` guard of tcc_ir_opt_licm_ex's fixed-point loop,
     * letting the index walk cfg->blocks[] out of bounds (the 02..08 self-host
     * HardFault).  Forward conditional threading (real if/else diamonds) and
     * all unconditional-JUMP threading stay enabled. */
    if (q->op == TCCIR_OP_JUMPIF && new_target < target)
      new_target = target;

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
 * Remove jumps that target the next instruction.  Covers both:
 *   - Unconditional JUMP whose target equals the fallthrough — pure no-op.
 *   - Conditional JUMPIF whose target equals the fallthrough — both branches
 *     go to the same place, so the test (and the flag-setter that feeds it)
 *     is dead.  The flag-setter is cleaned up by orphan_cmp_elim in the next
 *     cascade iteration.
 */
int tcc_ir_opt_eliminate_fallthrough(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== ELIMINATE FALL-THROUGH START ===");

  /* Iterate over every instruction including the last (i < n, not n - 1): a
   * trailing JUMP/JUMPIF to the epilogue (target == n) at the final slot is a
   * fall-through no-op too, and find_first_non_nop(n) returns n so it matches. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);

    /* Find the next non-NOP instruction after this one (n == epilogue) */
    int next_real = find_first_non_nop(ir, i + 1);

    /* If jump target equals the next real instruction, eliminate it.
     * Also eliminate a JUMPIF whose target matches the target of the
     * immediately following unconditional JMP (both arms converge). */
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

    /* For JUMPIF (conditional), avoid the case that exposes TCC's
     * non-aliasing-aware constant prop: removing a JUMPIF whose flag-setter
     * chain involves a user CALL would orphan the CMP, demote the user
     * FUNCCALLVAL → FUNCCALLVOID, and let constant prop incorrectly fold a
     * subsequent memory-CMP whose value depends on the call's pointer-arg
     * side effects.  Safe to eliminate when EITHER:
     *   (a) Fallthrough/target is itself an unconditional control transfer
     *       (JUMP/RETURN/TRAP) — no following CMP-on-memory to misfold.
     *   (b) Every CALL in the JUMPIF's basic block (scanning back from the
     *       JUMPIF to the nearest jump_target / function start) is a known-
     *       pure helper (aeabi soft-float helpers, isnan, etc.) — pure
     *       means no memory side effects to mis-track. */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      int safe = 0;
      if (next_real >= n)
      {
        /* Both arms reach the epilogue (target == fall-through == past-end).
         * There is no following instruction whose constant prop could be
         * misled by an orphaned CMP, so this is always safe regardless of any
         * impure call in the block. */
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
          /* Stop at the basic-block boundary: a jump_target is the head of
           * the BB, and we shouldn't reason across it. */
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

int tcc_ir_opt_jump_threading_ex(IROptCtx *ctx) { return tcc_ir_opt_jump_threading(ctx->ir); }
int tcc_ir_opt_eliminate_fallthrough_ex(IROptCtx *ctx) { return tcc_ir_opt_eliminate_fallthrough(ctx->ir); }
