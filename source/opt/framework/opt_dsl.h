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

/* MATCH() opens a rule body: binds ir, q and the guard flag; operands are bound on demand by BIND. */
#define MATCH() \
  TCCIRState *ir = ctx->ir; \
  IRQuadCompact *q = &ir->compact_instructions[i]; \
  int _opt_dsl_guard_ok = 1; \
  (void)q; (void)_opt_dsl_guard_ok

/* BIND*(slot) binds dest/src1/src2 by name; the suffixed forms bail unless the operand is IMM/VREG/STACKOFF. */
#define BIND(name)          IROperand name = tcc_ir_op_get_##name(ir, q)
#define BIND_IMM(name)      BIND(name); if (!irop_is_immediate(name)) return 0
#define BIND_VREG(name)     BIND(name); if (irop_get_vreg(name) < 0) return 0
#define BIND_STACKOFF(name) BIND(name); if (irop_get_tag(name) != IROP_TAG_STACKOFF) return 0

/* GUARD(...) — Guard expression over when/and/and_not; returns 0 if it fails. */
#define GUARD(...) \
  do { __VA_ARGS__; } while (0); \
  if (!_opt_dsl_guard_ok) return 0

/* REWRITE(set_op(..), set_dest(..), set_src1(..), set_src2(..)) — applied in argument order, then returns 1. */
#define REWRITE(...) do { __VA_ARGS__; return 1; } while (0)

#define set_op(x)   ((void)(q->op = (x)))
#define set_dest(v) tcc_ir_set_dest(ir, i, (v))
#define set_src1(v) tcc_ir_set_src1(ir, i, (v))
#define set_src2(v) tcc_ir_set_src2(ir, i, (v))

/* set_srcN_imm(v, bt) == set_srcN(mk_imm_bt(v, bt)), as one small call per site. */
OPT_DSL_OUTLINE static inline void opt_dsl_set_src1_imm(TCCIRState *ir, int i, int32_t v, int bt)
{
  tcc_ir_set_src1(ir, i, irop_make_imm32(0, v, bt));
}

OPT_DSL_OUTLINE static inline void opt_dsl_set_src2_imm(TCCIRState *ir, int i, int32_t v, int bt)
{
  tcc_ir_set_src2(ir, i, irop_make_imm32(0, v, bt));
}

/* set_srcN_ref(x) == set_srcN(x) for an lvalue x, passed by address instead of as a 9-byte copy. */
OPT_DSL_OUTLINE static inline void opt_dsl_set_src1_ref(TCCIRState *ir, int i, const IROperand *v)
{
  tcc_ir_set_src1(ir, i, *v);
}

OPT_DSL_OUTLINE static inline void opt_dsl_set_src2_ref(TCCIRState *ir, int i, const IROperand *v)
{
  tcc_ir_set_src2(ir, i, *v);
}

#define set_src1_imm(v, bt) opt_dsl_set_src1_imm(ir, i, (v), (bt))
#define set_src2_imm(v, bt) opt_dsl_set_src2_imm(ir, i, (v), (bt))
#define set_src1_ref(x)     opt_dsl_set_src1_ref(ir, i, &(x))
#define set_src2_ref(x)     opt_dsl_set_src2_ref(ir, i, &(x))
