/*
 *  TCC IR - Fall-Through Jump Elimination
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
 * or the target has multiple predecessors. */
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
      /* target == n is the epilogue (one past the last instruction): a valid
       * terminal, so follow the chain into it. */
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

/* ============================================================================
 * Jump Threading — post-RA control-flow cleanup (out of scope for the flat->SSA
 * retirement; runs after regalloc/out-of-SSA where there are no phi nodes).
 * Forwards each JUMP/JUMPIF target through NOP runs and unconditional-JUMP
 * chains to the ultimate destination.  See docs/plan_legacy_flat_ir_ssa_retire.md.
 * ============================================================================ */
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

    /* A CONDITIONAL branch (JUMPIF) must not have its taken edge retargeted
     * BACKWARD by chain-following — that would land it inside an enclosing loop
     * body and let downstream cleanup collapse a live loop-exit test.  Forward
     * conditional threading and all unconditional-JUMP threading stay enabled.
     * (A "single-hop, fall-through-unreachable" relaxation was tried and broke
     * test 295 / cmp_offset_fold back-edge shapes — do not re-relax without a
     * fuzz sweep.) */
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

int tcc_ir_opt_eliminate_fallthrough_ex(IROptCtx *ctx) { return tcc_ir_opt_eliminate_fallthrough(ctx->ir); }

/* JUMPIF inversion — `JUMPIF C -> A; JUMP -> B` where A is the first real
 * instruction after the JUMP becomes `JUMPIF !C -> B` (JUMP NOPed), saving a
 * branch.  opt_select normalizes this shape pre-RA, but the cfg_cleanup
 * cascade re-creates it after opt_select already ran (a collapsed diamond
 * leaves a conditional skip over a backward latch JUMP), so the cascade needs
 * a standalone copy.  allow_backward=0 (pre-RA) skips backward B: inverting a
 * body's `JUMPIF skip; JMP latch` destroys the unconditional body->latch JUMP
 * try_rotate_loop pattern-matches, blocking rotation (pr112581-1). */
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

    /* No external edge may land on the JUMP or a NOP before it — NOPing the
     * JUMP would silently reroute that edge onto A. */
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
