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

/* Upper bound on the entries one follow_jump_chain() marks: every JUMP step
 * counts against MAX_ITERATIONS and at most one NOP skip follows each step
 * (find_first_non_nop lands on a non-NOP, or on an already-visited index). */
#define JT_MAX_ITERATIONS 100
#define JT_MAX_MARKS (2 * (JT_MAX_ITERATIONS + 1) + 2)

/* Marks the indices it visits in `visited` and records them in `marks` so the
 * caller can clear exactly those afterwards instead of the whole array. */
static int follow_jump_chain(TCCIRState *ir, int target_idx, uint8_t *visited, int *marks, int *nmarks)
{
  int n = ir->next_instruction_index;
  int current = target_idx;
  int iterations = 0;
  const int MAX_ITERATIONS = JT_MAX_ITERATIONS;

  while (current < n && iterations < MAX_ITERATIONS && *nmarks < JT_MAX_MARKS)
  {
    if (visited[current])
      break;
    visited[current] = 1;
    marks[(*nmarks)++] = current;

    IRQuadCompact *q = &ir->compact_instructions[current];

    if (q->op == TCCIR_OP_NOP)
    {
      current = find_first_non_nop(ir, current);
      continue;
    }

    if (q->op == TCCIR_OP_JUMP)
    {
      int next_target = (int)tcc_ir_op_dest_imm(ir, q);
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

/* Forward every JUMP/JUMPIF across a chain of unconditional JUMPs.
 *
 * allow_backward_jumpif=0 keeps a conditional branch from being retargeted
 * BACKWARD (the guard dates from the legacy pipeline; its recorded reason is
 * that the JUMPIF then lands inside an enclosing loop body and a downstream
 * cleanup collapsed a live loop-exit test).  The pre-regalloc cfg_cleanup run
 * keeps it: the flat loop passes after it match loops on their trampoline
 * layout.
 *
 * allow_backward_jumpif=1 is for the post-regalloc run only.  There no pass
 * reads loop shape any more, register assignment is final, and a trampoline
 * `L: JUMP H` emits nothing but the branch, so the register and flag state on
 * arrival at H is the state the JUMPIF had, whichever way it gets there.  The
 * scratch-register bookkeeping still holds: a value live into H is live at L
 * (L reaches H) and live at H, and its interval -- contiguous in instruction
 * order, extended over the back-edge at L -- spans H..L, which contains the
 * JUMPIF.  What this buys is the hot `continue`/state-machine
 * shape `bcc L ... L: b H` -- one taken `b` per iteration on each threaded
 * path (Zig CBE's Tokenizer.next, getKeyword) -- and, when nothing else
 * reaches L, jumpif_invert(allow_backward=1) then turns the fall-through
 * `bcc exit; b H` pair into one `b!cc H`.  TCC_DISABLE_PASS=ra:jt_backward
 * turns it off. */
static int jump_threading_impl(TCCIRState *ir, int allow_backward_jumpif)
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

    int marks[JT_MAX_MARKS];
    int nmarks = 0;
    int new_target = follow_jump_chain(ir, target, visited, marks, &nmarks);
    for (int k = 0; k < nmarks; k++)
      visited[marks[k]] = 0;
    new_target = find_first_non_nop(ir, new_target);

    /* Retargeting a JUMPIF backward would land it inside an enclosing loop body and let downstream cleanup collapse a live loop-exit test
     * (pre-regalloc only, see above). */
    if (q->op == TCCIR_OP_JUMPIF && new_target < target && !allow_backward_jumpif)
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

int tcc_ir_opt_jump_threading(TCCIRState *ir)
{
  return jump_threading_impl(ir, 0);
}

int tcc_ir_opt_jump_threading_post_ra(TCCIRState *ir)
{
  return jump_threading_impl(ir, !tcc_ir_opt_pass_disabled("ra:jt_backward"));
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

    int target = (int)tcc_ir_op_dest_imm(ir, q);

    int next_real = find_first_non_nop(ir, i + 1);

    /* Also fires when a JUMPIF and the unconditional JUMP behind it converge on one target. */
    if (target != next_real)
    {
      if (q->op != TCCIR_OP_JUMPIF || next_real >= n)
        continue;
      IRQuadCompact *nq = &ir->compact_instructions[next_real];
      if (nq->op != TCCIR_OP_JUMP)
        continue;
      int next_target = (int)tcc_ir_op_dest_imm(ir, nq);
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
          /* A jump_target heads the basic block; don't reason across it --
           * a NOP one included (tested before the NOP skip). */
          if (pq->is_jump_target)
            break;
          if (pq->op == TCCIR_OP_NOP)
            continue;
          if (pq->op == TCCIR_OP_FUNCCALLVAL || pq->op == TCCIR_OP_FUNCCALLVOID)
          {
            Sym *callee = tcc_ir_op_src1_sym(ir, pq);
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

/* ---- Branches into __builtin_unreachable ----
 *
 * A TRAP marked `unreachable` (the frontend's __builtin_unreachable) is never
 * reached, and neither is any instruction from which every path runs into one
 * without doing anything observable on the way: those are DOOMED.  A
 * conditional branch with one doomed side therefore always goes the other
 * way.  It is folded -- dropped when its target is doomed, made unconditional
 * when its fall-through is -- and the compare, the loop counter or the switch
 * range check that fed it is left to DCE.  The Zig C backend ends every
 * exhaustive switch with `default: zig_unreachable();` (3,400 in zig.c) and
 * bounds loops whose exit is unreachable.
 *
 * "Observable" is read conservatively: only register-to-register value
 * instructions and non-volatile loads are passed over; a store, a call, an
 * atomic or anything else keeps its path. */
static int unreach_passes_over(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_CMP:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_CLZ:
  case TCCIR_OP_RBIT:
  case TCCIR_OP_REV:
  case TCCIR_OP_REV16:
    break;
  case TCCIR_OP_LOAD:
    if (tcc_ir_access_is_volatile(ir, tcc_ir_op_get_src1(ir, q)))
      return 0;
    break;
  default:
    return 0;
  }
  /* The value lands in a register or a VAR: a store through a pointer is no
   * value instruction. */
  if (irop_config[q->op].has_dest)
  {
    if (tcc_ir_op_dest_is_lval(ir, q) && !tcc_ir_op_dest_is_local(ir, q))
      return 0;
  }
  return 1;
}

TCC_DBG_ENV_FLAG(unreach_dbg, "TCC_UNREACH_DBG")

int tcc_ir_opt_unreachable_fold(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int any = 0;
  for (int i = 0; i < n && !any; i++)
    any = ir->compact_instructions[i].op == TCCIR_OP_TRAP && ir->compact_instructions[i].unreachable;
  if (!any)
    return 0;

  uint8_t *doomed = tcc_mallocz(n + 1); /* doomed[n] = 0: the epilogue */
  for (int round = 0, changed = 1; changed && round < 64; round++)
  {
    changed = 0;
    for (int p = n - 1; p >= 0; p--)
    {
      if (doomed[p])
        continue;
      IRQuadCompact *q = &ir->compact_instructions[p];
      int d = 0;
      if (q->op == TCCIR_OP_TRAP)
        d = q->unreachable;
      else if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        int t = (int)tcc_ir_op_dest_imm(ir, q);
        d = t >= 0 && t < n && doomed[t] && (q->op == TCCIR_OP_JUMP || doomed[p + 1]);
      }
      else if (unreach_passes_over(ir, q))
        d = doomed[p + 1];
      if (d)
        doomed[p] = 1, changed = 1;
    }
  }

  int changes = 0;
  for (int p = 0; p < n; p++)
  {
    IRQuadCompact *q = &ir->compact_instructions[p];
    if (q->op != TCCIR_OP_JUMPIF || doomed[p])
      continue;
    int t = (int)tcc_ir_op_dest_imm(ir, q);
    if (t < 0 || t > n)
      continue;
    if (doomed[t])
      q->op = TCCIR_OP_NOP; /* never taken */
    else if (doomed[p + 1])
      q->op = TCCIR_OP_JUMP; /* always taken */
    else
      continue;
    changes++;
    TCC_DBG_BLOCK(unreach_dbg)
    {
      extern const char *funcname;
      fprintf(stderr, "[UNREACH] %s %d %s\n", funcname, p, q->op == TCCIR_OP_NOP ? "never-taken" : "always-taken");
    }
  }
  tcc_free(doomed);
  return changes;
}
