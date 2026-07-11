/*
 *  TCC SSA opt - strcpy constant-source inline fold handler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* strcpy(stack_dst, "literal") -> BLOCK_COPY(dst, literal, L+1) when L+1 is a
 * whole number of words.  BLOCK_COPY copies word-at-a-time; a non-multiple of 4
 * would clobber dst past the terminating NUL, so those are left as the runtime
 * __tcc_strcpy call.  The backend lowers a small BLOCK_COPY to inline stores and
 * a large one to memcpy -- both strictly better than the strcpy call.  Operand
 * convention mirrors the memset->BLOCK_COPY rewrite in ir/opt.c. */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "str_handlers.h"
#include <string.h>

static int strcpy_prepare(const StrFoldCtx *c, IROperand *out_dst, IROperand *out_src, int *out_size)
{
  IROperand dst;
  IROperand src;
  IROperand src_sym;
  const char *s;
  int len;

  if (!ir_opt_get_call_param_operand(c->ir, c->call_idx, 0, &dst) ||
      !ir_opt_get_call_param_operand(c->ir, c->call_idx, 1, &src))
    return 0;

  /* dst must be a bare stack address -- BLOCK_COPY's destination is a stack
   * frame offset (codegen reads it directly, no pointer register). */
  if (irop_get_tag(dst) != IROP_TAG_STACKOFF || dst.is_lval)
    return 0;

  if (!ir_opt_eval_const_string(c->ir, src, c->call_idx, &s, 0) ||
      !ir_opt_eval_const_string_operand(c->ir, src, c->call_idx, &src_sym, 0))
    return 0;
  len = (int)strlen(s);
  if (len < 0 || ((len + 1) & 3) != 0)
    return 0;

  /* BLOCK_COPY reads the source with LDM, which faults on an unaligned base.
   * String literals are word-aligned (tccgen), but a non-word addend (a
   * substring, e.g. "abc"+1) would still land unaligned -- decline those. */
  {
    IRPoolSymref *sr = irop_get_symref_ex(c->ir, src_sym);
    if (!sr || (sr->addend & 3) != 0)
      return 0;
  }

  /* strcpy returns dst; if the result is consumed the copy alone is not enough.
   * A -1 result vreg means the call produces no value (discarded) -> safe. */
  if (c->is_valued)
  {
    IROperand res = tcc_ir_op_get_dest(c->ir, &c->ir->compact_instructions[c->call_idx]);
    int32_t rv = irop_get_vreg(res);
    if (rv != -1)
    {
      IRSSAVregInfo *vi;
      if (!c->ssa)
        return 0;
      vi = ssa_opt_vinfo(c->ssa, rv);
      if (!vi || vi->use_count != 0)
        return 0;
    }
  }

  *out_dst = irop_make_stackoff(-1, irop_get_stack_offset(dst), 1, 0, 0, IROP_BTYPE_INT32);
  *out_src = src_sym;
  *out_size = len + 1;
  return 1;
}

static int strcpy_can_fold(const StrFoldCtx *c)
{
  IROperand dst;
  IROperand src;
  int size;
  return strcpy_prepare(c, &dst, &src, &size);
}

static int strcpy_fold(StrFoldCtx *c)
{
  IRQuadCompact *q = &c->ir->compact_instructions[c->call_idx];
  IROperand dst;
  IROperand src;
  int size;
  int pool_base;

  if (!strcpy_prepare(c, &dst, &src, &size))
    return 0;

  ir_opt_nop_call_params(c->ir, c->call_idx);

  pool_base = tcc_ir_iroperand_pool_add(c->ir, dst);
  tcc_ir_iroperand_pool_add(c->ir, src);
  tcc_ir_iroperand_pool_add(c->ir, irop_make_imm32(-1, size, VT_INT));

  q->op = TCCIR_OP_BLOCK_COPY;
  q->operand_base = pool_base;
  return 1;
}

const StrFoldHandler tcc_strfold_strcpy = {
    STRBI_STRCPY,
    "strcpy",
    strcpy_can_fold,
    strcpy_fold,
};
