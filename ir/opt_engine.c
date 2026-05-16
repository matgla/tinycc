/*
 *  TCC IR - Pre-SSA optimization engine
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
#include "opt_utils.h"

void tcc_ir_opt_ctx_init(IROptCtx *ctx, TCCIRState *ir)
{
  ctx->ir = ir;
  ctx->n = ir->next_instruction_index;
  ctx->generation = 1;
  ctx->du.def = NULL;
  ctx->du_gen = 0;
  ctx->merge_bitmap = NULL;
  ctx->merge_gen = 0;
  ctx->changes = 0;
}

void tcc_ir_opt_ctx_free(IROptCtx *ctx)
{
  if (ctx->du.def) {
    tcc_free(ctx->du.def);
    ctx->du.def = NULL;
  }
  if (ctx->merge_bitmap) {
    tcc_free(ctx->merge_bitmap);
    ctx->merge_bitmap = NULL;
  }
}

void tcc_ir_opt_ctx_invalidate(IROptCtx *ctx)
{
  ctx->generation++;
  ctx->n = ctx->ir->next_instruction_index;
}

const IROptDU *tcc_ir_opt_ctx_require_du(IROptCtx *ctx)
{
  if (ctx->du_gen != ctx->generation) {
    if (ctx->du.def)
      tcc_free(ctx->du.def);
    ir_opt_du_build(ctx->ir, &ctx->du);
    ctx->du_gen = ctx->generation;
  }
  return &ctx->du;
}

const uint8_t *tcc_ir_opt_ctx_require_merge(IROptCtx *ctx)
{
  if (ctx->merge_gen != ctx->generation) {
    if (ctx->merge_bitmap)
      tcc_free(ctx->merge_bitmap);
    ctx->merge_bitmap = ir_opt_build_merge_bitmap(ctx->ir, ctx->n);
    ctx->merge_gen = ctx->generation;
  }
  return ctx->merge_bitmap;
}

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
