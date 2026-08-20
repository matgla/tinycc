/*
 *  TCC IR - "does this instruction touch volatile memory" for the optimizer
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

/* Volatility of a whole instruction, for passes that delete or move one rather
 * than rewrite a single operand.  Memory ops carry the mark in two places: on
 * the deref (lvalue) operand of a LOAD/STORE, and on the BASE operand of a
 * LOAD/STORE_INDEXED, where the fusion passes put it (irop_carry_access_marks).
 * An ordinary ALU op can also embed a deref operand, so every source is
 * checked — but only lvalue ones, so a function that touches volatile memory
 * once does not turn its whole body unremovable. */
int tcc_ir_instr_access_is_volatile(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!ir->func_has_volatile_access)
    return 0;

  switch (q->op)
  {
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
    /* base operand: not an lvalue, carries the transferred mark */
    return irop_access_is_volatile(tcc_ir_op_get_src1(ir, q));
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return irop_access_is_volatile(tcc_ir_op_get_dest(ir, q));
  default:
    break;
  }

  if (irop_config[q->op].has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval && irop_access_is_volatile(d))
      return 1;
  }
  if (irop_config[q->op].has_src1)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval && irop_access_is_volatile(s))
      return 1;
  }
  if (irop_config[q->op].has_src2)
  {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (s.is_lval && irop_access_is_volatile(s))
      return 1;
  }
  return 0;
}
