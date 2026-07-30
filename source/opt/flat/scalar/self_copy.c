/*
 *  TCC IR - Eliminate memcpy/memmove calls whose dst and src compute the same
 *  value — the copy is a no-op regardless of length or overlap (pre-SSA engine)
 *
 *  Triggered notably by `*p = *p` aggregate self-assignments, where struct /
 *  vector lowering already emits a memmove(p, p, sizeof).
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/self_copy.h"

OPT_GEN_FLAT(self_copy_elim, TCCIR_OP_FUNCCALLVAL)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY });

  Sym *callee = irop_get_sym_ex(ir, src1);
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  /* memcpy/memmove and AAPCS variants, plus the __tcc_memmove alias. */
  if (!name ||
      !(ir_opt_is_memcpy_or_memmove_name(name) || strcmp(name, "__tcc_memmove") == 0))
    return 0;

  IROperand p0, p1;
  if (!ir_opt_get_call_param_operand(ir, i, 0, &p0) ||
      !ir_opt_get_call_param_operand(ir, i, 1, &p1))
    return 0;

  /* Resolve each param at its own marshalling site, not the call index: a
   * source redefined between param0 and param1 would otherwise collapse to the
   * same (last) reaching def and fold a non-self copy. */
  int p0_idx = ir_opt_get_call_param_index(ir, i, 0);
  int p1_idx = ir_opt_get_call_param_index(ir, i, 1);
  if (p0_idx < 0 || p1_idx < 0)
    return 0;

  if (!ir_opt_pure_expr_equal(ir, p0, p0_idx, p1, p1_idx, 0))
    return 0;

  ir_opt_nop_call_params(ir, i);
  if (q->op == TCCIR_OP_FUNCCALLVAL)
  {
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, p0);
    tcc_ir_set_src2(ir, i, IROP_NONE);
  }
  else
  {
    q->op = TCCIR_OP_NOP;
  }
  return 1;
}

const IROptGen self_copy_gens[] = {
    OPT_GEN_ENTRY_FLAT(self_copy_elim, TCCIR_OP_FUNCCALLVAL),
    OPT_GEN_ENTRY_FLAT(self_copy_elim, TCCIR_OP_FUNCCALLVOID),
};

const int self_copy_gens_count = sizeof(self_copy_gens) / sizeof(self_copy_gens[0]);

int tcc_ir_opt_self_copy_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, self_copy_gens, self_copy_gens_count);
}

int tcc_ir_opt_self_copy_elim(TCCIRState *ir)
{
  if (!ir)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, self_copy_gens, self_copy_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}
