/*
 *  TCC SSA opt - constant string/memory builtin folding pass
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* SSA-phase folder for strlen/strcmp/... of constant strings. Runs after the
 * address-folding SSA passes so index-computed string addresses that only
 * become constant in SSA (docs/plan_ssa_const_string_fold.md) still fold. */
int tcc_ir_ssa_opt_const_string_fold(struct IRSSAOptCtx *ctx);

