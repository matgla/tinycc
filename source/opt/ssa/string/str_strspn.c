/*
 *  TCC SSA opt - strspn / strcspn constant fold handlers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

static int span_read_args(const StrFoldCtx *c, const char **s, const char **set)
{
  IROperand arg0, arg1;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &arg1))
    return 0;
  if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, s, 0) ||
      !ir_opt_eval_const_string(c->ir, arg1, c->call_idx, set, 0))
    return 0;
  return 1;
}

static int span_fold_to_int(StrFoldCtx *c, int v)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];

  ir_opt_nop_call_params(c->ir, c->call_idx);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(c->ir, c->call_idx, irop_make_imm32(-1, v, VT_INT));
  tcc_ir_set_src2(c->ir, c->call_idx, IROP_NONE);
  return 1;
}

static int strspn_eval(const StrFoldCtx *c, int *out_val)
{
  const char *s, *accept;

  if (!span_read_args(c, &s, &accept))
    return 0;
  *out_val = (int)strspn(s, accept);
  return 1;
}

static int strspn_can_fold(const StrFoldCtx *c)
{
  int v;
  return strspn_eval(c, &v);
}

static int strspn_fold(StrFoldCtx *c)
{
  int v;

  if (!strspn_eval(c, &v))
    return 0;
  return span_fold_to_int(c, v);
}

static int strcspn_eval(const StrFoldCtx *c, int *out_val)
{
  const char *s, *reject;

  if (!span_read_args(c, &s, &reject))
    return 0;
  *out_val = (int)strcspn(s, reject);
  return 1;
}

static int strcspn_can_fold(const StrFoldCtx *c)
{
  int v;
  return strcspn_eval(c, &v);
}

static int strcspn_fold(StrFoldCtx *c)
{
  int v;

  if (!strcspn_eval(c, &v))
    return 0;
  return span_fold_to_int(c, v);
}

const StrFoldHandler tcc_strfold_strspn = {
    STRBI_STRSPN,
    "strspn",
    strspn_can_fold,
    strspn_fold,
};

const StrFoldHandler tcc_strfold_strcspn = {
    STRBI_STRCSPN,
    "strcspn",
    strcspn_can_fold,
    strcspn_fold,
};
