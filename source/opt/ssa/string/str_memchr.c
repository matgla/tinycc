/*
 *  TCC SSA opt - memchr constant fold handler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* memchr(s,c,n) -> &s+off (first match within n bytes) or NULL.  Only the bytes
 * up to and including the NUL are known, so n > strlen(s)+1 does not fold.
 * See docs/plan_ssa_const_string_fold.md. */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "str_handlers.h"
#include <string.h>

/* out_off = byte index of the match relative to sym->addend, or -1 = NULL. */
static int memchr_eval(const StrFoldCtx *c, IROperand *out_sym, int *out_off)
{
  IROperand arg0, arg1, arg2, sym;
  const char *s;
  uint64_t needle, n;

  if (!c->is_valued)
    return 0;
  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &arg0) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &arg1) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 2, &arg2))
    return 0;
  if (!ir_opt_eval_const_u64(c->ir, arg2, c->call_idx, &n, 0))
    return 0;
  if (!ir_opt_eval_const_string(c->ir, arg0, c->call_idx, &s, 0) ||
      !ir_opt_eval_const_string_operand(c->ir, arg0, c->call_idx, &sym, 0))
    return 0;
  if (irop_get_tag(sym) != IROP_TAG_SYMREF)
    return 0;
  if (!ir_opt_eval_const_u64(c->ir, arg1, c->call_idx, &needle, 0))
    return 0;
  if (n > (uint64_t)strlen(s) + 1)
    return 0;
  if (!ir_opt_fold_memchr_offset(s, (unsigned char)needle, n, out_off))
    return 0;

  *out_sym = sym;
  return 1;
}

static int memchr_can_fold(const StrFoldCtx *c)
{
  IROperand sym;
  int off;
  return memchr_eval(c, &sym, &off);
}

static int memchr_fold(StrFoldCtx *c)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  IROperand sym, res;
  int off;

  if (!memchr_eval(c, &sym, &off))
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

const StrFoldHandler tcc_strfold_memchr = {
    STRBI_MEMCHR,
    "memchr",
    memchr_can_fold,
    memchr_fold,
};
