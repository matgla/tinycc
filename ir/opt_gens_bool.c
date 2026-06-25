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
#include "opt_gens_bool.h"

static int ir_gen_bool_idempotent(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int is_and = (q->op == TCCIR_OP_BOOL_AND);

  if (src1.vr >= 0 && src1.vr == src2.vr) {
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src2(ir, i, IROP_NONE);
    LOG_IR_GEN("BOOL IDEMPOTENT: %s vr%d with itself at i=%d -> ASSIGN",
               is_and ? "&&" : "||", src1.vr, i);
    return 1;
  }

  if (src2.vr < 0 && irop_is_immediate(src2)) {
    int64_t val = irop_get_imm64_ex(ir, src2);
    if ((is_and && val == 1) || (!is_and && val == 0)) {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src2(ir, i, IROP_NONE);
      LOG_IR_GEN("BOOL IDEMPOTENT: %s with neutral element at i=%d -> ASSIGN",
                 is_and ? "&&" : "||", i);
      return 1;
    }
  }

  return 0;
}

const IROptGen bool_gens[] = {
    {TCCIR_OP_BOOL_AND, ir_gen_bool_idempotent, "bool_idempotent_and", 0},
    {TCCIR_OP_BOOL_OR, ir_gen_bool_idempotent, "bool_idempotent_or", 0},
};

const int bool_gens_count = sizeof(bool_gens) / sizeof(bool_gens[0]);
