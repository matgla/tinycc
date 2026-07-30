/*
 *  TCC SSA opt - strlen constant fold handler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "str_handlers.h"
#include <string.h>

static int strlen_eval(const StrFoldCtx *c, int *out_len)
{
  IROperand arg0;
  const char *s;
  int stack_len;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0))
    return 0;
  if (ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s, 0))
  {
    *out_len = (int)strlen(s);
    return 1;
  }
  if (ir_opt_eval_stack_strlen(c->ir, arg0, c->call_idx, &stack_len))
  {
    *out_len = stack_len;
    return 1;
  }
  return 0;
}

static int strlen_can_fold(const StrFoldCtx *c)
{
  int len;
  return strlen_eval(c, &len);
}

static int strlen_fold(StrFoldCtx *c)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  int len;

  if (!strlen_eval(c, &len))
    return 0;

  ir_opt_nop_call_params(c->ir, c->call_idx);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(c->ir, c->call_idx, irop_make_imm32(-1, len, VT_INT));
  tcc_ir_set_src2(c->ir, c->call_idx, IROP_NONE);
  return 1;
}

const StrFoldHandler tcc_strfold_strlen = {
    STRBI_STRLEN,
    "strlen",
    strlen_can_fold,
    strlen_fold,
};
