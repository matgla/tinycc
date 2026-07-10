/*
 *  TCC SSA opt - strength reduction (MUL/UDIV/UMOD by powers of two)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_STRENGTH_H
#define TCC_OPT_SSA_STRENGTH_H

struct IRSSAOptCtx;

/* Single-instruction strength reductions on power-of-two operands:
 *   MUL  x, 2^n -> SHL x, n   (immediate in either source; MUL is commutative)
 *   UDIV x, 2^n -> SHR x, n
 *   UMOD x, 2^n -> AND x, 2^n - 1
 * Returns the number of instructions rewritten. */
int ssa_opt_strength(struct IRSSAOptCtx *ctx);

#endif /* TCC_OPT_SSA_STRENGTH_H */
