/*
 *  TCC IR - Boolean simplification generator table (pre-SSA engine)
 *
 *  Generators for idempotent boolean simplifications:
 *    a && a → a,  a || a → a
 *    a && 1 → a,  a || 0 → a
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
#include "opt_dsl.h"
#include "opt/flat/bool.h"

/* BOOL_AND/OR operands are boolean (0/1), so with a constant operand:
 *   neutral element     (AND:1, OR:0): `a OP n → a`     → ASSIGN src1
 *   annihilator element (AND:0, OR:1): `a OP z → z`     → ASSIGN #z
 * Idempotent `a OP a → a` also collapses to ASSIGN src1.
 * REWRITE cannot clear src2 to NONE, so clear it manually. */
static int bool_idempotent(IROptCtx *ctx, int i, int neutral)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  if (irop_is_immediate(src2) && imm(src2) == !neutral) {
    tcc_ir_set_src2(ir, i, IROP_NONE);
    REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = mk_imm(!neutral));
  }
  GUARD(
    when((vreg(src1) >= 0 && vreg(src1) == vreg(src2)) ||
         (irop_is_immediate(src2) && imm(src2) == neutral)));
  tcc_ir_set_src2(ir, i, IROP_NONE);
  REWRITE(.new_op = TCCIR_OP_ASSIGN);
}

OPT_GEN_FLAT(bool_idempotent_and, TCCIR_OP_BOOL_AND) {
  return bool_idempotent(ctx, i, 1);
}

OPT_GEN_FLAT(bool_idempotent_or, TCCIR_OP_BOOL_OR) {
  return bool_idempotent(ctx, i, 0);
}

const IROptGen bool_gens[] = {
    OPT_GEN_ENTRY_FLAT(bool_idempotent_and, TCCIR_OP_BOOL_AND),
    OPT_GEN_ENTRY_FLAT(bool_idempotent_or, TCCIR_OP_BOOL_OR),
};

const int bool_gens_count = sizeof(bool_gens) / sizeof(bool_gens[0]);
