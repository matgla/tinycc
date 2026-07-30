/*
 *  TCC SSA opt - strchr/strrchr/index/rindex constant fold handlers
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* strchr(s,c)/index(s,c) -> &s+off (first match), strrchr(s,c)/rindex(s,c) ->
 * &s+off (last match), NULL when not found.  c is truncated to unsigned char;
 * the terminating NUL is a valid target (c=='\0' returns a pointer to it, never
 * NULL).  See docs/plan_ssa_const_string_fold.md. */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "str_handlers.h"
#include <string.h>

/* out_off = byte index of the match within the resolved const string (already
 * relative to sr->addend), or -1 = not found -> NULL.  out_sym = the string
 * literal symref operand (base for &sym+K).  last selects first/last match. */
static int search_eval(const StrFoldCtx *c, int last, IROperand *out_sym, int *out_off)
{
  IROperand arg0, arg1, sym;
  const char *s;
  uint64_t chv;
  unsigned char target;
  size_t len, i;
  int found;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &arg1))
    return 0;
  if (!ir_opt_eval_const_u64(c->ir, arg1, c->call_idx, &chv, 0))
    return 0;
  if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s, 0) ||
      !ir_opt_eval_const_string_operand(c->ir, arg0, c->call_idx, &sym, 0))
    return 0;
  if (irop_get_tag(sym) != IROP_TAG_SYMREF)
    return 0;

  target = (unsigned char)chv;
  len = strlen(s);
  found = -1;
  for (i = 0; i <= len; i++)
  {
    if ((unsigned char)s[i] == target)
    {
      found = (int)i;
      if (!last)
        break;
    }
  }

  *out_sym = sym;
  *out_off = found;
  return 1;
}

static int search_fold(StrFoldCtx *c, int last)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  IROperand sym, res;
  int off;

  if (!search_eval(c, last, &sym, &off))
    return 0;

  if (off < 0)
  {
    res = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  }
  else
  {
    IRPoolSymref *sr = irop_get_symref_ex(c->ir, sym);
    uint32_t pidx;
    if (!sr)
      return 0;
    pidx = tcc_ir_pool_add_symref(c->ir, sr->sym, sr->addend + off, sr->flags);
    res = irop_make_symref(irop_get_vreg(sym), pidx, sym.is_lval, sym.is_local, sym.is_const, irop_get_btype(sym));
  }

  ir_opt_nop_call_params(c->ir, c->call_idx);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(c->ir, c->call_idx, res);
  tcc_ir_set_src2(c->ir, c->call_idx, IROP_NONE);
  return 1;
}

static int strchr_can_fold(const StrFoldCtx *c)
{
  IROperand sym;
  int off;
  return search_eval(c, 0, &sym, &off);
}

static int strchr_fold(StrFoldCtx *c)
{
  return search_fold(c, 0);
}

static int strrchr_can_fold(const StrFoldCtx *c)
{
  IROperand sym;
  int off;
  return search_eval(c, 1, &sym, &off);
}

static int strrchr_fold(StrFoldCtx *c)
{
  return search_fold(c, 1);
}

const StrFoldHandler tcc_strfold_strchr = {
    STRBI_STRCHR,
    "strchr",
    strchr_can_fold,
    strchr_fold,
};

const StrFoldHandler tcc_strfold_index = {
    STRBI_INDEX,
    "index",
    strchr_can_fold,
    strchr_fold,
};

const StrFoldHandler tcc_strfold_strrchr = {
    STRBI_STRRCHR,
    "strrchr",
    strrchr_can_fold,
    strrchr_fold,
};

const StrFoldHandler tcc_strfold_rindex = {
    STRBI_RINDEX,
    "rindex",
    strrchr_can_fold,
    strrchr_fold,
};
