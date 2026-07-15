/*
 *  TCC IR - Self-expression arithmetic identity fold (pre-SSA engine)
 *
 *  Folds x/x -> 1, x%x -> 0 for matching register reads and non-volatile
 *  global symbolrefs. Safe because x/x is UB when x==0.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_dsl.h"
#include "opt/flat/self_arith.h"

/* Both operands must be same is_sym+is_lval global read (same sym+addend),
 * non-volatile and non-FP. fold_val is the identity result (div->1, mod->0).
 * REWRITE cannot clear src2 to NONE, so clear it manually. */
static int self_arith(IROptCtx *ctx, int i, int fold_val)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });

  /* Same register on both sides: x/x==1, x%x==0 for x!=0 (div-by-zero UB).
   * Both read at this quad, so equal vregs = identical value. Registers only
   * — exclude memory derefs (is_lval, may be volatile), symrefs, and immediates. */
  if (vreg(src1) >= 0 && vreg(src1) == vreg(src2) &&
      !src1.is_lval && !src2.is_lval && !src1.is_sym && !src2.is_sym &&
      !irop_is_immediate(src1) && !irop_is_immediate(src2)) {
    tcc_ir_set_src2(ir, i, IROP_NONE);
    REWRITE(.new_op = TCCIR_OP_ASSIGN,
            .src1 = irop_make_imm32(-1, fold_val, irop_get_btype(dest)));
  }

  GUARD(when(src1.is_sym && src1.is_lval && src2.is_sym && src2.is_lval));

  IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
  IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
  if (!a_ref || !b_ref || a_ref->sym != b_ref->sym || a_ref->addend != b_ref->addend)
    return 0;

  int ttype = a_ref->sym->type.t;
  int btype = ttype & VT_BTYPE;
  if ((ttype & VT_VOLATILE) ||
      btype == VT_FLOAT || btype == VT_DOUBLE || btype == VT_LDOUBLE)
    return 0;

  tcc_ir_set_src2(ir, i, IROP_NONE);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = irop_make_imm32(-1, fold_val, irop_get_btype(dest)));
}

OPT_GEN_FLAT(self_arith_div, TCCIR_OP_DIV) {
  return self_arith(ctx, i, 1);
}

OPT_GEN_FLAT(self_arith_udiv, TCCIR_OP_UDIV) {
  return self_arith(ctx, i, 1);
}

OPT_GEN_FLAT(self_arith_imod, TCCIR_OP_IMOD) {
  return self_arith(ctx, i, 0);
}

OPT_GEN_FLAT(self_arith_umod, TCCIR_OP_UMOD) {
  return self_arith(ctx, i, 0);
}

const IROptGen self_arith_gens[] = {
    OPT_GEN_ENTRY_FLAT(self_arith_div, TCCIR_OP_DIV),
    OPT_GEN_ENTRY_FLAT(self_arith_udiv, TCCIR_OP_UDIV),
    OPT_GEN_ENTRY_FLAT(self_arith_imod, TCCIR_OP_IMOD),
    OPT_GEN_ENTRY_FLAT(self_arith_umod, TCCIR_OP_UMOD),
};

const int self_arith_gens_count = sizeof(self_arith_gens) / sizeof(self_arith_gens[0]);

int tcc_ir_opt_self_arith_fold_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, self_arith_gens, self_arith_gens_count);
}

int tcc_ir_opt_self_arith_fold(TCCIRState *ir)
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, self_arith_gens, self_arith_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}
