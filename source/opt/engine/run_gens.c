/*
 *  TCC IR - Pre-SSA generator drivers
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

int tcc_ir_opt_run_gens(IROptCtx *ctx, const IROptGen *gens, int count)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  int any_du = 0;
  for (int g = 0; g < count; g++) {
    if (gens[g].needs_du) {
      any_du = 1;
      break;
    }
  }
  if (any_du)
    tcc_ir_opt_ctx_require_du(ctx);

  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    for (int g = 0; g < count; g++) {
      if (gens[g].op >= 0 && gens[g].op != op)
        continue;
      int d = gens[g].fn(ctx, i);
      if (d > 0) {
        changes += d;
        break;
      }
    }
  }

  return changes;
}

int tcc_ir_opt_run_stateful_gens(IROptCtx *ctx, const IROptGen *gens, int count,
                                 const IROptStatefulOps *ops)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  int any_du = 0;
  for (int g = 0; g < count; g++) {
    if (gens[g].needs_du) {
      any_du = 1;
      break;
    }
  }
  if (any_du)
    tcc_ir_opt_ctx_require_du(ctx);

  ctx->pass_state = ops->begin ? ops->begin(ctx) : NULL;

  /* Each instruction goes to exactly one handler: the first gen whose op matches, so a wildcard op < 0 must be registered last. */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    /* each_pre runs before the NOP-skip so a pass can invalidate facts at block boundaries landing on a NOP. */
    if (ops->each_pre)
      ops->each_pre(ctx, i);
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    for (int g = 0; g < count; g++) {
      if (gens[g].op >= 0 && gens[g].op != op)
        continue;
      int d = gens[g].fn(ctx, i);
      if (d > 0)
        changes += d;
      break;
    }
  }

  if (ops->end)
    ops->end(ctx);
  ctx->pass_state = NULL;

  return changes;
}
