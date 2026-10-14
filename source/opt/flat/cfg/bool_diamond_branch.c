/*
 *  TCC IR - Branch straight out of a short-circuit boolean diamond
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

/* A `&&`/`||` written directly in an `if` is compiled as branches, but the
 * same test behind a `static inline` predicate is not: the callee has to
 * MATERIALIZE a 0/1, so inlining leaves a diamond whose merge is immediately
 * re-tested.
 *
 *     0003: JMP to 9  if "!="          ; e != 0x7FF
 *     0005: T3 <-- (cond)              ; true arm computes the boolean
 *     0008: JMP to 10
 *     0009: T3 <-- #0 [ASSIGN]         ; false arm: the constant
 *     0010: TEST_ZERO T3
 *     0011: JMP to 15  if "=="
 *
 * Arriving at 0009 fixes T3, so the test at 0010 is already decided there:
 * the edge into the constant arm can go straight to whichever side of 0011
 * that constant selects (here 15).  The arm is then unreachable and ordinary
 * cleanup deletes it, which is what finally lets setif fusion see
 * `T3 <-- (cond)` feeding the only remaining TEST_ZERO and collapse that too.
 * On `__aeabi_dadd`, whose prologue is four inlined `is_nan_parts` /
 * `is_zero_parts` predicates, each site goes from 16 instructions to 4.
 *
 * The transform only skips the assignment on the redirected edge, so it is
 * legal exactly when nothing else reads the merged value. */
int tcc_ir_opt_bool_diamond_branch(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;
  if (tcc_ir_opt_pass_disabled("ssa:bool_diamond_branch"))
    return 0;

  for (int m = 0; m < n; m++)
  {
    IRQuadCompact *qt = &ir->compact_instructions[m];
    if (qt->op != TCCIR_OP_TEST_ZERO)
      continue;

    IROperand tv = tcc_ir_op_get_src1(ir, qt);
    int32_t tvr = irop_get_vreg(tv);
    /* TEST_ZERO is a 32-bit test; a wider or float operand is a different
     * comparison and its `!= 0` is not this constant's. */
    if (tvr < 0 || tv.is_lval || irop_get_tag(tv) != IROP_TAG_VREG)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(tvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (tv.btype != IROP_BTYPE_INT32 && tv.btype != IROP_BTYPE_INT8 &&
        tv.btype != IROP_BTYPE_INT16)
      continue;

    /* The branch that consumes the flags TEST_ZERO just set. */
    int j = ir_skip_nops_forward(ir, m + 1, n);
    if (j >= n)
      continue;
    IRQuadCompact *qj = &ir->compact_instructions[j];
    if (qj->op != TCCIR_OP_JUMPIF)
      continue;
    int cond = (int)tcc_ir_op_src1_imm(ir, qj);
    if (cond != TOK_EQ && cond != TOK_NE)
      continue;
    int taken_target = (int)tcc_ir_op_dest_imm(ir, qj);
    if (taken_target < 0 || taken_target > n)
      continue;
    int fall_target = ir_skip_nops_forward(ir, j + 1, n);

    /* Mirrored orientation: the constant arm is a JUMP into the test.
     *
     * The Zig C backend writes `c1 || c2` the other way round from the shape
     * below: the SETIF arm falls through into the test while the constant arm
     * jumps in from above —
     *
     *     0005: JMP to 7  if "!="          ; !c1
     *     0006: T3 <-- #1 [ASSIGN]         ; true arm of the first check
     *     (p) JMP to 10                    ; -> the merge test
     *     0007: T3 <-- (cond)              ; false arm: the second check
     *     0010: TEST_ZERO T3               ; <- p lands here
     *     0011: JMP to 15  if "=="
     *
     * Arriving through p fixes T3 to 1, so that edge can go straight to
     * whichever side of 0011 the constant selects.  The SETIF arm then
     * reaches the test on its own — the shape setif fusion collapses — and
     * the arm's ASSIGN, though now unread, still executes for any later
     * reader of T3, so unlike the orientation below no single-reader scan is
     * needed: only the pure TEST/JUMPIF pair is bypassed. */
    int mirrored = 0;
    int arm_idx[8], arm_count = 0;
    for (int p = 0; p < n; p++)
    {
      IRQuadCompact *qp = &ir->compact_instructions[p];
      if (qp->op != TCCIR_OP_JUMP)
        continue;
      if (tcc_ir_op_dest_u_imm32(ir, qp) != m)
        continue;
      int k = p - 1;
      while (k >= 0 && ir->compact_instructions[k].op == TCCIR_OP_NOP)
        k--;
      if (k < 0)
        continue;
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op != TCCIR_OP_ASSIGN)
        continue;
      if (tcc_ir_op_dest_is_lval(ir, qk) || tcc_ir_op_dest_vreg(ir, qk) != tvr)
        continue;
      if (tcc_ir_op_src1_is_lval(ir, qk) || !tcc_ir_op_src1_is_imm(ir, qk))
        continue;
      /* A label between the ASSIGN and its JUMP would enter the arm past the
       * assignment, arriving at the test with some other T3. */
      int past_label = 0;
      for (int t = k + 1; t < p; t++)
        if (ir->compact_instructions[t].is_jump_target)
          past_label = 1;
      if (past_label)
        continue;

      int is_zero = ((int32_t)tcc_ir_op_src1_imm(ir, qk) == 0);
      int taken = (cond == TOK_EQ) ? is_zero : !is_zero;
      int new_target = taken ? taken_target : fall_target;
      if (new_target == m || new_target >= n)
        continue;

      IROperand pd = tcc_ir_op_get_dest(ir, qp);
      pd.u.imm32 = new_target;
      tcc_ir_op_set_dest(ir, qp, pd);
      ir->compact_instructions[new_target].is_jump_target = 1;
      if (arm_count < 8)
        arm_idx[arm_count++] = k;
      mirrored++;
      changes++;
    }
    /* The redirected arms' `Tv <-- #C` no longer reaches the test on any
     * path; with no other reader of Tv they are dead, and leaving them
     * extends Tv's live range across the SETIF arm's whole computation --
     * one callee-saved register more in the prologue (frame_undef
     * two_values).  The arm is entered by the guard's fall-through and the
     * assign is its only instruction besides the JUMP, so the block empties
     * and the branch cleanups fold it away. */
    if (mirrored && arm_count == mirrored)
    {
      int other_read = 0;
      for (int i = 0; i < n && !other_read; i++)
      {
        if (i == m)
          continue;
        IRQuadCompact *qi = &ir->compact_instructions[i];
        if (qi->op == TCCIR_OP_NOP)
          continue;
        for (int slot = 0; slot < 3 && !other_read; slot++)
        {
          IROperand o;
          if (slot == 0)
          {
            if (!irop_config[qi->op].has_src1)
              continue;
            o = tcc_ir_op_get_src1(ir, qi);
          }
          else if (slot == 1)
          {
            if (!irop_config[qi->op].has_src2)
              continue;
            o = tcc_ir_op_get_src2(ir, qi);
          }
          else
          {
            if (!ir_op_has(qi->op, IROP_A_SLOT3))
              continue;
            o = ir->iroperand_pool[qi->operand_base + 3];
          }
          if (irop_get_vreg(o) == tvr)
            other_read = 1;
        }
        if (!other_read && irop_config[qi->op].has_dest)
        {
          if (tcc_ir_op_dest_is_lval(ir, qi) && tcc_ir_op_dest_vreg(ir, qi) == tvr)
            other_read = 1;
        }
      }
      if (!other_read)
        for (int a = 0; a < arm_count; a++)
          ir->compact_instructions[arm_idx[a]].op = TCCIR_OP_NOP;
    }
    if (mirrored)
    {
      /* The redirects may have left the test with no incoming edge; say so,
       * or the fuse passes still see the stale jump-target flag and decline
       * the now-single-def SETIF arm.  An indirect jump or switch table
       * keeps the flag: its targets are not in the scan. */
      int still_targeted = 0;
      for (int p = 0; p < n && !still_targeted; p++)
      {
        IRQuadCompact *qp = &ir->compact_instructions[p];
        if (qp->op == TCCIR_OP_SWITCH_TABLE || qp->op == TCCIR_OP_IJUMP ||
            ir_op_has(qp->op, IROP_A_MAY_BRANCH))
          still_targeted = 1;
        if (qp->op != TCCIR_OP_JUMP && qp->op != TCCIR_OP_JUMPIF)
          continue;
        if (tcc_ir_op_dest_u_imm32(ir, qp) == m)
          still_targeted = 1;
      }
      for (int t = 0; t < ir->num_switch_tables && !still_targeted; t++)
      {
        TCCIRSwitchTable *tbl = &ir->switch_tables[t];
        if (tbl->default_target == m)
          still_targeted = 1;
        for (int e = 0; e < tbl->num_entries && !still_targeted; e++)
          if (tbl->targets[e] == m)
            still_targeted = 1;
      }
      if (!still_targeted)
        qt->is_jump_target = 0;
    }

    /* The constant arm: the block immediately above the test must be nothing
     * but `Tv <-- #C`, entered only by an explicit jump. */
    int k = m - 1;
    while (k >= 0 && ir->compact_instructions[k].op == TCCIR_OP_NOP)
      k--;
    if (k < 0)
      continue;
    IRQuadCompact *qk = &ir->compact_instructions[k];
    if (qk->op != TCCIR_OP_ASSIGN || !qk->is_jump_target)
      continue;
    if (tcc_ir_op_dest_is_lval(ir, qk) || tcc_ir_op_dest_vreg(ir, qk) != tvr)
      continue;
    if (tcc_ir_op_src1_is_lval(ir, qk) || !tcc_ir_op_src1_is_imm(ir, qk))
      continue;

    int is_zero = ((int32_t)tcc_ir_op_src1_imm(ir, qk) == 0);
    int taken = (cond == TOK_EQ) ? is_zero : !is_zero;
    int new_target = taken ? taken_target : fall_target;
    if (new_target == k)
      continue;

    /* Redirecting an edge past the arm skips the assignment on it, so the
     * value may not be read anywhere but the test being folded. */
    int other_read = 0;
    for (int i = 0; i < n && !other_read; i++)
    {
      if (i == m)
        continue;
      IRQuadCompact *qi = &ir->compact_instructions[i];
      if (qi->op == TCCIR_OP_NOP)
        continue;
      for (int slot = 0; slot < 3 && !other_read; slot++)
      {
        IROperand o;
        if (slot == 0)
        {
          if (!irop_config[qi->op].has_src1)
            continue;
          o = tcc_ir_op_get_src1(ir, qi);
        }
        else if (slot == 1)
        {
          if (!irop_config[qi->op].has_src2)
            continue;
          o = tcc_ir_op_get_src2(ir, qi);
        }
        else
        {
          if (!ir_op_has(qi->op, IROP_A_SLOT3))
            continue;
          o = ir->iroperand_pool[qi->operand_base + 3];
        }
        if (irop_get_vreg(o) == tvr)
          other_read = 1;
      }
      /* A write THROUGH the value (`Tv***DEREF*** <-- x`) reads it as well. */
      if (!other_read && irop_config[qi->op].has_dest)
      {
        if (tcc_ir_op_dest_is_lval(ir, qi) && tcc_ir_op_dest_vreg(ir, qi) == tvr)
          other_read = 1;
      }
    }
    if (other_read)
      continue;

    int local = 0;
    for (int p = 0; p < n; p++)
    {
      if (p == j)
        continue;
      IRQuadCompact *qp = &ir->compact_instructions[p];
      if (qp->op != TCCIR_OP_JUMP && qp->op != TCCIR_OP_JUMPIF)
        continue;
      IROperand pd = tcc_ir_op_get_dest(ir, qp);
      if ((int)irop_get_imm64_ex(ir, pd) != k)
        continue;
      pd.u.imm32 = new_target;
      tcc_ir_op_set_dest(ir, qp, pd);
      local++;
    }
    for (int t = 0; t < ir->num_switch_tables; t++)
    {
      TCCIRSwitchTable *tbl = &ir->switch_tables[t];
      if (tbl->default_target == k)
      {
        tbl->default_target = new_target;
        local++;
      }
      for (int e = 0; e < tbl->num_entries; e++)
        if (tbl->targets[e] == k)
        {
          tbl->targets[e] = new_target;
          local++;
        }
    }

    if (local)
    {
      if (new_target < n)
        ir->compact_instructions[new_target].is_jump_target = 1;
      changes += local;
    }
  }

  return changes;
}
