/*
 *  TCC IR - SSA Value Range Propagation (ssa:vrp)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Dominator-scoped per-SSA-value [min,max] range propagation; folds CMP+JUMPIF
 * and CMP+SETIF over constant compares */
int ssa_opt_vrp(struct IRSSAOptCtx *ctx);
