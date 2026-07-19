/*
 *  TCC IR - SSA SETIF state-mask algebra (ssa:setif_or_taut, ssa:setif_mask_fold)
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

/* Collapse `CMP <bool-chain>, <bool-chain>` (both built from compares of the
 * same operand pair) plus its flag consumer into a single compare of that pair:
 * `(a<b) ^ (a>b)`  ==>  `a != b`  (gcc PR107881). */
int ssa_opt_setif_mask_fold(struct IRSSAOptCtx *ctx);
