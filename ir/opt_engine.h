/*
 *  TCC IR - Pre-SSA optimization engine
 *
 *  Mirrors the SSA engine (IRSSAOptGen / ssa_opt_run_gens) for the
 *  post-destruction IR layer.  A single forward pass dispatches to
 *  opcode-triggered generator functions, sharing a lazy analysis cache.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_ENGINE_H
#define TCC_IR_OPT_ENGINE_H

#include <stdint.h>
#include "opt_du.h"

struct TCCIRState;

typedef struct IROptCtx
{
  struct TCCIRState *ir;
  int n;
  uint32_t generation;

  IROptDU du;
  uint32_t du_gen;

  uint8_t *merge_bitmap;
  uint32_t merge_gen;

  int changes;
} IROptCtx;

typedef int (*ir_opt_gen_fn)(IROptCtx *ctx, int instr_idx);

typedef struct IROptGen
{
  int op;
  ir_opt_gen_fn fn;
  const char *name;
  uint8_t needs_du;
} IROptGen;

void tcc_ir_opt_ctx_init(IROptCtx *ctx, struct TCCIRState *ir);
void tcc_ir_opt_ctx_free(IROptCtx *ctx);
void tcc_ir_opt_ctx_invalidate(IROptCtx *ctx);

const IROptDU *tcc_ir_opt_ctx_require_du(IROptCtx *ctx);
const uint8_t *tcc_ir_opt_ctx_require_merge(IROptCtx *ctx);

int tcc_ir_opt_run_gens(IROptCtx *ctx, const IROptGen *gens, int count);

#endif /* TCC_IR_OPT_ENGINE_H */
