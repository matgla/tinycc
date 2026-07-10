/*
 *  TCC SSA opt - constant string/memory builtin folding pass
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_CONST_STRING_FOLD_H
#define TCC_OPT_SSA_CONST_STRING_FOLD_H

struct IRSSAOptCtx;

/* SSA-phase folder for strlen/strcmp/... of constant strings. Runs after the
 * address-folding SSA passes so index-computed string addresses that only
 * become constant in SSA (docs/plan_ssa_const_string_fold.md) still fold. */
int tcc_ir_ssa_opt_const_string_fold(struct IRSSAOptCtx *ctx);

#endif /* TCC_OPT_SSA_CONST_STRING_FOLD_H */
