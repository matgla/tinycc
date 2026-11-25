/*
 *  TCC SSA opt - -ffortify string checks a known string does not need
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* __tcc_fortify_str(p, n) (builtin/fortify.c, before a call whose argument
 * is a null_terminated_string_arg) ends the program when p's NUL is not in
 * its object's n bytes.  A string whose length is known here -- a literal,
 * or a frame buffer whose bytes this function wrote (`char buf[] =
 * "/usr/lib"`) -- with its NUL inside the n bytes needs no check: the call
 * goes.  The frontend folds what sizes alone prove; only this needs the
 * bytes, which the frontend does not follow. */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "str_handlers.h"
#include <string.h>

static int fortify_str_fits(const StrFoldCtx *c)
{
  IROperand a0, a1;
  const char *s;
  int len;
  if (c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &a1) || !irop_is_immediate(a1) ||
      !irop_is_plain_imm(a1) || a1.btype == IROP_BTYPE_INT64)
    return 0;
  const uint32_t n = (uint32_t)irop_get_imm64_ex(c->ir, a1);
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &a0))
    return 0;
  if (ir_opt_eval_const_string(c->ir, a0, c->call_idx, &s, 0))
    len = (int)strlen(s);
  else if (!ir_opt_eval_stack_strlen(c->ir, a0, c->call_idx, &len))
    return 0;
  return len >= 0 && (uint32_t)len < n;
}

static int fortify_str_fold(StrFoldCtx *c)
{
  if (!fortify_str_fits(c))
    return 0;
  ir_opt_nop_call_params(c->ir, c->call_idx);
  c->ir->compact_instructions[c->call_idx].op = TCCIR_OP_NOP;
  return 1;
}

const StrFoldHandler tcc_strfold_fortify_str = {
    STRBI_FORTIFY_STR,
    "__tcc_fortify_str",
    fortify_str_fits,
    fortify_str_fold,
};
