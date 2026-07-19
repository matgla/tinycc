/*
 *  TCC SSA opt - constant string/memory builtin folding pass (driver + registry)
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
#include "const_string_fold.h"

/* Registry: append a handler TU's object here — dispatch is by builtin_id. */
static const StrFoldHandler *const g_handlers[] = {
    &tcc_strfold_strlen,
    &tcc_strfold_strcmp,
    &tcc_strfold_strncmp,
    &tcc_strfold_memcmp,
    &tcc_strfold_memchr,
    &tcc_strfold_strcpy,
    &tcc_strfold_strspn,
    &tcc_strfold_strcspn,
    &tcc_strfold_strchr,
    &tcc_strfold_index,
    &tcc_strfold_strrchr,
    &tcc_strfold_rindex,
    &tcc_strfold_strstr,
    &tcc_strfold_strpbrk,
};

static const StrFoldHandler *g_by_id[STRBI__COUNT];
static int g_index_built;

static void ensure_index_built(void)
{
  if (g_index_built)
    return;
  for (size_t k = 0; k < sizeof(g_handlers) / sizeof(g_handlers[0]); k++)
  {
    const StrFoldHandler *h = g_handlers[k];
    if (h->builtin_id > STRBI_UNKNOWN && h->builtin_id < STRBI__COUNT)
      g_by_id[h->builtin_id] = h;
  }
  g_index_built = 1;
}

/* ctx may be NULL: the early flat-region run has no SSA state yet, and every
 * handler either ignores it or checks it (str_strcpy). */
static int const_string_fold_core(TCCIRState *ir, IRSSAOptCtx *ctx)
{
  int changes = 0;

  ensure_index_built();

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;
    int id;
    const StrFoldHandler *h;
    StrFoldCtx c;

    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;

    id = resolve_str_builtin_id(callee->v, get_tok_str(callee->v, NULL));
    if (id <= STRBI_UNKNOWN || id >= STRBI__COUNT)
      continue;

    h = g_by_id[id];
    if (!h)
      continue;

    c.ssa = ctx;
    c.ir = ir;
    c.call_idx = i;
    c.builtin_id = id;
    c.is_valued = (q->op == TCCIR_OP_FUNCCALLVAL);

    if (h->can_fold(&c))
      changes += h->fold(&c);
  }

  return changes;
}

int tcc_ir_ssa_opt_const_string_fold(IRSSAOptCtx *ctx)
{
  int changes = const_string_fold_core(ctx->ir, ctx);

  if (changes)
    tcc_ir_ssa_opt_rebuild(ctx);

  return changes;
}

int tcc_ir_ssa_opt_const_string_fold_flat(TCCIRState *ir)
{
  return const_string_fold_core(ir, NULL);
}
