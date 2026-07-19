/*
 *  TCC IR - SSA SETIF OR-chain tautology fold (ssa:setif_or_taut)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Fold OR-chains of CMP+SETIF booleans whose {LT,EQ,GT} masks cover all states
 * to constant 1. */
int ssa_opt_setif_or_taut(struct IRSSAOptCtx *ctx);
