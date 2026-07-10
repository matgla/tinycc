/*
 *  TCC SSA opt - phi simplification (CFG structural pass)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_PHI_H
#define TCC_OPT_SSA_PHI_H

struct IRSSAOptCtx;

/* Eliminate trivial phi nodes:
 *   - all-operand-same-vreg phis -> replace dest with that vreg
 *   - all-self-operand phis -> kept (no incoming value)
 *   - mixed-operand phis -> kept (non-trivial)
 *   - single-operand (degenerate) phis -> replace with operand
 *
 * The replacement bails when a use must keep dest's exact vreg identity
 * (e.g. ARM barrel-shift src2); the phi is kept so phi resolution still
 * materializes it.
 *
 * Returns the number of phis eliminated. */
int ssa_opt_phi_simplify(struct IRSSAOptCtx *ctx);

#endif /* TCC_OPT_SSA_PHI_H */
