/*
 *  TCC SSA opt - constant folding (ALU ops with constant operands)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_FOLD_H
#define TCC_OPT_SSA_FOLD_H

struct IRSSAOptCtx;

/* Constant folding: evaluate ALU ops with two immediate operands at compile
 * time, replacing the instruction with ASSIGN dest = #result.  Also handles
 * algebraic identities (x+0, x*1, x|x, x^x→0, etc.), bit-complement collapse,
 * double-negation, and barrel-shift-annotated folds.  Returns the number of
 * instructions rewritten. */
int ssa_opt_fold(struct IRSSAOptCtx *ctx);

#endif /* TCC_OPT_SSA_FOLD_H */
