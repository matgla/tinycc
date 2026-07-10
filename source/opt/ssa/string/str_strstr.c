/*
 *  TCC SSA opt - strstr / strpbrk constant substring/char-set fold handlers
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

/* Byte index of the first substring match, empty needle -> 0 (C returns hay). */
static int strstr_off(const char *s, const char *t)
{
  const char *hit;
  if (t[0] == '\0')
    return 0;
  hit = strstr(s, t);
  return hit ? (int)(hit - s) : -1;
}

/* Byte index of the first char of s that occurs in accept, empty accept -> -1. */
static int strpbrk_off(const char *s, const char *t)
{
  const char *hit = strpbrk(s, t);
  return hit ? (int)(hit - s) : -1;
}

static int search2_eval(const StrFoldCtx *c, int (*off_fn)(const char *, const char *), IROperand *out_sym,
                        int *out_off)
{
  IROperand arg0, arg1, sym;
  const char *s, *t;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &arg1))
    return 0;
  if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s, 0) ||
      !ir_opt_eval_const_string(c->ir, arg1, c->call_idx, &t, 0))
    return 0;
  if (!ir_opt_eval_const_string_operand(c->ir, arg0, c->call_idx, &sym, 0))
    return 0;
  if (irop_get_tag(sym) != IROP_TAG_SYMREF)
    return 0;

  *out_sym = sym;
  *out_off = off_fn(s, t);
  return 1;
}

static int search2_fold(StrFoldCtx *c, int (*off_fn)(const char *, const char *))
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  IROperand sym, res;
  int off;

  if (!search2_eval(c, off_fn, &sym, &off))
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

static int strstr_can_fold(const StrFoldCtx *c)
{
  IROperand sym;
  int off;
  return search2_eval(c, strstr_off, &sym, &off);
}

static int strstr_fold(StrFoldCtx *c)
{
  return search2_fold(c, strstr_off);
}

static int strpbrk_can_fold(const StrFoldCtx *c)
{
  IROperand sym;
  int off;
  return search2_eval(c, strpbrk_off, &sym, &off);
}

static int strpbrk_fold(StrFoldCtx *c)
{
  return search2_fold(c, strpbrk_off);
}

const StrFoldHandler tcc_strfold_strstr = {
    STRBI_STRSTR,
    "strstr",
    strstr_can_fold,
    strstr_fold,
};

const StrFoldHandler tcc_strfold_strpbrk = {
    STRBI_STRPBRK,
    "strpbrk",
    strpbrk_can_fold,
    strpbrk_fold,
};
