/*
 *  TCC IR - SSA boolean re-normalization elimination (ssa:bool_norm)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Rewrite `CMP x,y` + `SETIF EQ/NE` into one bitwise op when x and y provably
 * hold 0 or 1: `x != y` becomes `x ^ y`, `x != 0` becomes x itself. */
int ssa_opt_bool_norm(struct IRSSAOptCtx *ctx);
