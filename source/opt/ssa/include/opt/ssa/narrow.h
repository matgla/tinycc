/*
 *  TCC SSA opt - narrowing / extension folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Shift-pair folding, redundant AND elimination, and SHR→UBFX fusion.
 * Returns the number of instructions rewritten. */
int ssa_opt_narrow(struct IRSSAOptCtx *ctx);

