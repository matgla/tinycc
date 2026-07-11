/*
 *  TCC SSA opt - constant-fold __builtin bit-op helper calls
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_BITOP_CONST_FOLD_H
#define TCC_OPT_SSA_BITOP_CONST_FOLD_H

#include <stdint.h>

struct IRSSAOptCtx;

/* Fold a call to a bit-op runtime helper (__popcount/__clz/__ctz/__parity
 * si2·di2, ffs/ffsl/ffsll) whose argument is a compile-time constant into that
 * constant.  tcc lowers __builtin_popcount(const) etc. to a helper call; GCC
 * evaluates it at compile time (gcc-execute/builtin-bitops-1). */
int tcc_ir_ssa_opt_bitop_const_fold(struct IRSSAOptCtx *ctx);

/* Introspection hook for unit tests: classify a bit-op helper `name` and
 * evaluate it on `val`.  Returns 1 (writes *out) when foldable, else 0. */
int tcc_ir_bitop_const_eval(const char *name, int64_t val, int *out);

#endif /* TCC_OPT_SSA_BITOP_CONST_FOLD_H */
