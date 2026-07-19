/*
 *  TCC IR - SSA CMP constant-offset fold (ssa:cmp_offset_fold)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Fold CMP+JUMPIF/SELECT when one compared value is the other plus a known
 * integer constant (A = B + K, or a shared base A = X+K1 / B = X+K2), reducing
 * the compare to `delta cond 0`.  Signed / EQ / NE only. */ 
int ssa_opt_cmp_offset_fold(struct IRSSAOptCtx *ctx);
