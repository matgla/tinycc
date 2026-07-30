/*
 *  TCC IR - Noreturn Call Epilogue Suppression
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
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

static int call_is_noreturn(TCCIRState *ir, IRQuadCompact *q)
{
  return tcc_ir_callee_is_noreturn(irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q)));
}

int tcc_ir_opt_noreturn_call_epilogue_suppress(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->noreturn)
    return 0; /* already set by a stronger pass */

  /* No RETURN may survive and at least one noreturn FUNCCALL must exist. */
  int has_return = 0;
  int has_noreturn_call = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      has_return = 1;
      break;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      if (call_is_noreturn(ir, q))
        has_noreturn_call = 1;
    }
  }
  if (has_return || !has_noreturn_call)
    return 0;

  /* Last live op must be a noreturn FUNCCALL (trailing CALLSEQ_END ignored). */
  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CALLSEQ_END)
      continue; /* trailing frame cleanup */
    last_idx = i;
    break;
  }
  if (last_idx < 0)
    return 0;

  int last_op = ir->compact_instructions[last_idx].op;
  if (last_op != TCCIR_OP_FUNCCALLVAL && last_op != TCCIR_OP_FUNCCALLVOID)
    return 0;

  /* Bail if any live jump can land after the final noreturn call. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int target = -1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      target = (int)irop_get_imm64_ex(ir, dest);
    }
    else
      continue;

    while (target >= 0 && target < n && ir->compact_instructions[target].op == TCCIR_OP_NOP)
      target++;
    if (target < 0 || target >= n || target > last_idx)
      return 0;
  }

  if (!call_is_noreturn(ir, &ir->compact_instructions[last_idx]))
    return 0;

  LOG_IR_GEN("NORETURN-CALL-EPILOGUE-SUPPRESS: function ends at noreturn call "
             "(i=%d) — setting ir->noreturn to skip dead epilogue", last_idx);
  ir->noreturn = 1;
  return 1;
}
