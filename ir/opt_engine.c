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
#include "licm.h"

void tcc_ir_opt_ctx_init(IROptCtx *ctx, TCCIRState *ir)
{
  ctx->ir = ir;
  ctx->n = ir->next_instruction_index;
  ctx->generation = 1;
  ctx->du.def = NULL;
  ctx->du_gen = 0;
  ctx->du_mode = IR_DU_MODE_FULL;
  ctx->merge_bitmap = NULL;
  ctx->merge_gen = 0;
  ctx->block_starts = NULL;
  ctx->block_starts_gen = 0;
  ctx->loops = NULL;
  ctx->loops_gen = 0;
  ctx->pass_state = NULL;
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
  if (ctx->block_starts) {
    tcc_free(ctx->block_starts);
    ctx->block_starts = NULL;
  }
  if (ctx->loops) {
    tcc_ir_free_loops(ctx->loops);
    ctx->loops = NULL;
  }
}

void tcc_ir_opt_ctx_invalidate(IROptCtx *ctx)
{
  ctx->generation++;
  ctx->n = ctx->ir->next_instruction_index;
}

const IROptDU *tcc_ir_opt_ctx_require_du(IROptCtx *ctx)
{
  return tcc_ir_opt_ctx_require_du_mode(ctx, ctx->du_mode);
}

const IROptDU *tcc_ir_opt_ctx_require_du_mode(IROptCtx *ctx, uint8_t mode)
{
  if (ctx->du_gen != ctx->generation || ctx->du.mode != mode) {
    if (ctx->du.def)
      tcc_free(ctx->du.def);
    ir_opt_du_build_mode(ctx->ir, &ctx->du, mode);
    ctx->du_gen = ctx->generation;
    ctx->du_mode = mode;
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

const uint8_t *tcc_ir_opt_ctx_require_block_starts(IROptCtx *ctx)
{
  if (ctx->block_starts_gen != ctx->generation) {
    if (ctx->block_starts)
      tcc_free(ctx->block_starts);
    ctx->block_starts = ir_opt_build_block_starts_bitmap(ctx->ir, ctx->n);
    ctx->block_starts_gen = ctx->generation;
  }
  return ctx->block_starts;
}

IRLoops *tcc_ir_opt_ctx_require_loops(IROptCtx *ctx)
{
  if (ctx->loops_gen != ctx->generation) {
    if (ctx->loops)
      tcc_ir_free_loops(ctx->loops);
    ctx->loops = tcc_ir_detect_loops(ctx->ir);
    ctx->loops_gen = ctx->generation;
  }
  return ctx->loops;
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

  /* First-matching-gen-owns-the-instruction: unlike run_gens (which tries the
   * next gen when one returns 0), a stateful pass dispatches each instruction to
   * exactly one handler — the first gen whose op matches (a wildcard op < 0
   * matches anything and must be registered last).  This mirrors the legacy
   * priority chain (`if (op==STORE) …; else …; goto post_op`) where a handler
   * updates shared lattice state whether or not it rewrites anything. */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    /* each_pre runs before the NOP-skip so a pass can invalidate facts at
     * basic-block boundaries even when the boundary lands on a NOP. */
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
