/*
 *  TCC IR - Vreg def-count and address-taken queries
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

int ir_opt_vreg_address_taken_between(TCCIRState *ir, int32_t vreg, int start_idx, int end_idx)
{
  if (!ir)
    return 0;

  for (int i = start_idx + 1; i < end_idx; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_LEA && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
  }

  return 0;
}

int tcc_ir_vreg_has_single_def(TCCIRState *ir, int32_t vreg)
{
  int def_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      def_count++;
      if (def_count > 1)
        return 0;
    }
  }
  return def_count == 1;
}

/* A ZERO-def vreg (e.g. an untouched incoming parameter) is as safe as a single-def one: no def means no back-edge can change it. */
int tcc_ir_vreg_has_multi_def(TCCIRState *ir, int32_t vreg)
{
  int def_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      def_count++;
      if (def_count > 1)
        return 1;
    }
  }
  return 0;
}

/* MLA's 4th (accumulator) pool operand is a real vreg USE invisible to has_src1/2; use-scans must count it or the def is wrongly killed. Returns -1 if none. */
int32_t ir_opt_mla_accum_vreg(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_MLA)
    return -1;
  return irop_get_vreg(tcc_ir_op_get_accum(ir, q));
}
