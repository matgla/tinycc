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

/* tcc inlines small static helpers at every call; out of line they are smaller on the device. */
#ifdef __TINYC__
#define OPT_DSL_OUTLINE __attribute__((noinline))
#else
#define OPT_DSL_OUTLINE
#endif

/* Read accessors — decode a bound operand slot in a GUARD expression. */
#define vreg(x)         irop_get_vreg(x)
#define imm(x)          irop_get_imm64_ex(ir, x)
#define stackoff(x)     irop_get_stack_offset(x)
#define lval(x)         ((x).is_lval)

/* A fresh immediate operand, e.g. REWRITE(set_src2(mk_imm(v))). */
#define mk_imm(v)       irop_make_imm32(0, (int32_t)(v), IROP_BTYPE_INT32)

/* mk_imm with an explicit btype (e.g. the folded dest's width). */
#define mk_imm_bt(v, bt) irop_make_imm32(0, (int32_t)(v), (bt))

/* Guard clauses — statements over _opt_dsl_guard_ok (declared by MATCH). */
#define when(expr)    _opt_dsl_guard_ok = !!(expr)
#define and(expr)     _opt_dsl_guard_ok = _opt_dsl_guard_ok && (expr)
#define and_not(expr) _opt_dsl_guard_ok = _opt_dsl_guard_ok && !(expr)

/* Trace hook — no-op when TCC_TRACE_OPT is not defined. */
#ifndef TCC_TRACE_OPT
#define TCC_TRACE_OPT(...) do {} while (0)
#endif

