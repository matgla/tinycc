/*
 *  TCC IR — Optimization DSL: Main Header
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#pragma once

/* Optimization DSL umbrella header — include ir.h first. See docs/optimizations/opt_dsl_framework.md. */

#include "opt_dsl_types.h"
#include "opt_dsl_helpers.h"
#include "opt_dsl_entry.h"

/* OPT_GEN_SSA(name, OP) — opens dispatch fn opt_dsl_dispatch_##name(ctx, i). */
#define OPT_GEN_SSA(name, op) \
  static int opt_dsl_dispatch_##name(IRSSAOptCtx *ctx, int i); \
  static int opt_dsl_dispatch_##name(IRSSAOptCtx *ctx, int i)

/* OPT_GEN_FLAT(name, OP) — OPT_GEN_SSA for the flat (pre-SSA) engine (IROptCtx). */
#define OPT_GEN_FLAT(name, op) \
  static int opt_dsl_dispatch_##name##_flat(IROptCtx *ctx, int i); \
  static int opt_dsl_dispatch_##name##_flat(IROptCtx *ctx, int i)

/* PATTERN(...) — must be first in the body: binds ir/q/dest/src1/src2 + guard flag, then checks constraints. */
#define PATTERN(...) \
  TCCIRState *ir = ctx->ir; \
  IRQuadCompact *q = &ir->compact_instructions[i]; \
  IROperand dest = tcc_ir_op_get_dest(ir, q); \
  IROperand src1 = tcc_ir_op_get_src1(ir, q); \
  IROperand src2 = tcc_ir_op_get_src2(ir, q); \
  int _opt_dsl_guard_ok = 1; \
  (void)dest; (void)src1; (void)src2; (void)_opt_dsl_guard_ok; \
  do { \
    const IROptPatternSpec _opt_dsl_pat = { __VA_ARGS__ }; \
    OPT_DSL_CHECK_CONSTRAINT(src1, _opt_dsl_pat.constraints.src1); \
    OPT_DSL_CHECK_CONSTRAINT(src2, _opt_dsl_pat.constraints.src2); \
    OPT_DSL_CHECK_CONSTRAINT(dest, _opt_dsl_pat.constraints.dest); \
    (void)_opt_dsl_pat; \
  } while (0)

/* GUARD(...) — Guard expression over when/and/and_not; returns 0 if it fails. */
#define GUARD(...) \
  do { __VA_ARGS__; } while (0); \
  if (!_opt_dsl_guard_ok) return 0

/* REWRITE(...) — mutates instruction i; operands built with mk_imm(...); .new_op defaults to keep. */
#define REWRITE(...) \
  do { \
    const IROptRewriteSpec _opt_dsl_rw = { .new_op = OPT_DSL_KEEP_OP, __VA_ARGS__ }; \
    OPT_DSL_APPLY_REWRITE(q, _opt_dsl_rw); \
    return 1; \
  } while (0)

