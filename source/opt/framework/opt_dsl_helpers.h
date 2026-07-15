/*
 *  TCC IR — Optimization DSL: Helper Macros
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_dsl_types.h"

/* DSL helper macros — requires ir.h first. See docs/optimizations/opt_dsl_framework.md. */

/* REWRITE(.new_op) "keep" sentinel: opcode 0 is TCCIR_OP_ADD, so 0 cannot mean unset. */
#define OPT_DSL_KEEP_OP (-1)

/* Read accessors — decode a bound operand slot in a GUARD expression. */
#define vreg(x)         irop_get_vreg(x)
#define imm(x)          irop_get_imm64_ex(ir, x)
#define stackoff(x)     irop_get_stack_offset(x)
#define lval(x)         ((x).is_lval)

/* Construct a fresh immediate operand for a REWRITE(.srcN = mk_imm(v)). */
#define mk_imm(v)       irop_make_imm32(0, (int32_t)(v), IROP_BTYPE_INT32)

/* mk_imm with an explicit btype (e.g. the folded dest's width). */
#define mk_imm_bt(v, bt) irop_make_imm32(0, (int32_t)(v), (bt))

/* Constraint check — folded into PATTERN; bails out of the dispatch on mismatch. */
#define OPT_DSL_CHECK_CONSTRAINT(elt, constraint)        \
  do {                                                   \
    switch (constraint) {                                \
    case IR_CONSTRAINT_IMM:                              \
      if (!irop_is_immediate(elt))                       \
        return 0;                                        \
      break;                                             \
    case IR_CONSTRAINT_VREG:                             \
      if (irop_get_vreg(elt) < 0)                        \
        return 0;                                        \
      break;                                             \
    case IR_CONSTRAINT_STACKOFF:                         \
      if (irop_get_tag(elt) != IROP_TAG_STACKOFF)        \
        return 0;                                        \
      break;                                             \
    case IR_CONSTRAINT_ANY:                              \
    default:                                             \
      break;                                             \
    }                                                    \
  } while (0)

/* Rewrite expansion — an unspecified operand field is a zero IROperand (tag NONE), so skipped. */
#define OPT_DSL_APPLY_REWRITE(q, spec)                          \
  do {                                                          \
    if ((spec).new_op != OPT_DSL_KEEP_OP)                       \
      (q)->op = (spec).new_op;                                  \
    if (irop_get_tag((spec).dest) != IROP_TAG_NONE)             \
      tcc_ir_set_dest(ir, i, (spec).dest);                      \
    if (irop_get_tag((spec).src1) != IROP_TAG_NONE)             \
      tcc_ir_set_src1(ir, i, (spec).src1);                      \
    if (irop_get_tag((spec).src2) != IROP_TAG_NONE)             \
      tcc_ir_set_src2(ir, i, (spec).src2);                      \
  } while (0)

/* Guard clauses — statements over _opt_dsl_guard_ok (declared by PATTERN). */
#define when(expr)    _opt_dsl_guard_ok = !!(expr)
#define and(expr)     _opt_dsl_guard_ok = _opt_dsl_guard_ok && (expr)
#define and_not(expr) _opt_dsl_guard_ok = _opt_dsl_guard_ok && !(expr)

/* Trace hook — no-op when TCC_TRACE_OPT is not defined. */
#ifndef TCC_TRACE_OPT
#define TCC_TRACE_OPT(...) do {} while (0)
#endif

