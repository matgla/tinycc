/*
 *  TCC SSA opt - strength reduction (MUL/UDIV/UMOD by powers of two)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_dsl.h"
#include "opt/ssa/strength.h"
#include "opt/ssa/ssa_opt_helpers.h"

/* Single-instruction strength reductions, expressed via source/opt/framework:
 *   MUL  x, 2^n  -> SHL x, n
 *   UDIV x, 2^n  -> SHR x, n
 *   UMOD x, 2^n  -> AND x, 2^n - 1
 * Multi-instruction patterns (2^n+-1) stay in the pre-SSA pass.
 * is_imm32 / is_power_of_2 come from ssa_opt_helpers.h. */

static int mul_pick_imm(IROperand src1, IROperand src2,
                        IROperand *val, IROperand *imm)
{
  if (is_imm32(src2)) { *imm = src2; *val = src1; return 1; }
  if (is_imm32(src1)) { *imm = src1; *val = src2; return 1; }
  return 0;
}

OPT_GEN_SSA(sr_mul, TCCIR_OP_MUL) {
  int shift = 0;
  IROperand val_op = IROP_NONE, imm_op = IROP_NONE;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(mul_pick_imm(src1, src2, &val_op, &imm_op));
    and(is_power_of_2((uint32_t)imm_op.u.imm32, &shift)));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src1   = val_op,
    .src2   = mk_imm(shift));
}

OPT_GEN_SSA(sr_udiv, TCCIR_OP_UDIV) {
  int shift = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(is_imm32(src2));
    and(is_power_of_2((uint32_t)src2.u.imm32, &shift)));
  REWRITE(
    .new_op = TCCIR_OP_SHR,
    .src2   = mk_imm(shift));
}

OPT_GEN_SSA(sr_umod, TCCIR_OP_UMOD) {
  int shift = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(is_imm32(src2));
    and(is_power_of_2((uint32_t)src2.u.imm32, &shift)));
  REWRITE(
    .new_op = TCCIR_OP_AND,
    .src2   = mk_imm((uint32_t)src2.u.imm32 - 1u));
}

static const IRSSAOptGen strength_gens[] = {
  OPT_GEN_ENTRY(sr_mul,  TCCIR_OP_MUL),
  OPT_GEN_ENTRY(sr_udiv, TCCIR_OP_UDIV),
  OPT_GEN_ENTRY(sr_umod, TCCIR_OP_UMOD),
};

int ssa_opt_strength(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, strength_gens,
                          OPT_DSL_TABLE_COUNT(strength_gens));
}
