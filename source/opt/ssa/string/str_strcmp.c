/*
 *  TCC SSA opt - strcmp/strncmp/memcmp constant fold handlers
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

static int strcmp_family_eval(const StrFoldCtx *c, int *out_result)
{
  IROperand arg0;
  IROperand arg1;
  IROperand arg2;
  const char *s1;
  const char *s2;
  uint64_t n;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &arg1))
    return 0;

  if (c->builtin_id == STRBI_STRCMP)
  {
    if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s1, 0) ||
        !ir_opt_eval_const_string(c->ir, arg1, c->call_idx, &s2, 0))
      return 0;
    *out_result = ir_opt_fold_strcmp_result(s1, s2);
    return 1;
  }

  /* strncmp/memcmp: n==0 is 0 regardless of the (possibly non-constant) args,
   * so resolve the length before touching the string operands. */
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 2, &arg2) ||
      !ir_opt_eval_const_u64(c->ir, arg2, c->call_idx, &n, 0))
    return 0;
  if (n == 0)
  {
    *out_result = 0;
    return 1;
  }
  if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s1, 0) ||
      !ir_opt_eval_const_string(c->ir, arg1, c->call_idx, &s2, 0))
    return 0;
  /* eval_const_string reads up to the first NUL; refuse if either operand's
   * compared window would run past that known length. */
  if (n > (uint64_t)strlen(s1) + 1 || n > (uint64_t)strlen(s2) + 1)
    return 0;

  if (c->builtin_id == STRBI_STRNCMP)
    *out_result = ir_opt_fold_strncmp_result(s1, s2, n);
  else
    *out_result = ir_opt_fold_memcmp_result(s1, s2, n);
  return 1;
}

static int strcmp_can_fold(const StrFoldCtx *c)
{
  int result;
  return strcmp_family_eval(c, &result);
}

static int strcmp_fold(StrFoldCtx *c)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  int result;

  if (!strcmp_family_eval(c, &result))
    return 0;

  ir_opt_nop_call_params(c->ir, c->call_idx);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(c->ir, c->call_idx, irop_make_imm32(-1, result, VT_INT));
  tcc_ir_set_src2(c->ir, c->call_idx, IROP_NONE);
  return 1;
}

const StrFoldHandler tcc_strfold_strcmp = {
    STRBI_STRCMP, "strcmp", strcmp_can_fold, strcmp_fold,
};
const StrFoldHandler tcc_strfold_strncmp = {
    STRBI_STRNCMP, "strncmp", strcmp_can_fold, strcmp_fold,
};
const StrFoldHandler tcc_strfold_memcmp = {
    STRBI_MEMCMP, "memcmp", strcmp_can_fold, strcmp_fold,
};
