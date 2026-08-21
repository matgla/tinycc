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
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, qj));
    if (cond != TOK_EQ && cond != TOK_NE)
      continue;
    int taken_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qj));
    if (taken_target < 0 || taken_target > n)
      continue;
    int fall_target = ir_skip_nops_forward(ir, j + 1, n);

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
    IROperand kd = tcc_ir_op_get_dest(ir, qk);
    if (kd.is_lval || irop_get_vreg(kd) != tvr)
      continue;
    IROperand ks = tcc_ir_op_get_src1(ir, qk);
    if (ks.is_lval || !irop_is_immediate(ks))
      continue;

    int is_zero = ((int32_t)irop_get_imm64_ex(ir, ks) == 0);
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
          if (qi->op != TCCIR_OP_MLA)
            continue;
          o = tcc_ir_op_get_accum(ir, qi);
        }
        if (irop_get_vreg(o) == tvr)
          other_read = 1;
      }
      /* A write THROUGH the value (`Tv***DEREF*** <-- x`) reads it as well. */
      if (!other_read && irop_config[qi->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, qi);
        if (d.is_lval && irop_get_vreg(d) == tvr)
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
