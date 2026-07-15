/*
 *  TCC SSA opt - VAR immediate propagation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Propagate immediate values from single-def VARs into their uses.
 * Returns the number of instructions rewritten. */
int ssa_opt_var_imm_prop(struct IRSSAOptCtx *ctx);

