/*
 *  TCC SSA opt - branch folding (CMP/TEST_ZERO constant-fold + phi edge prune)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Constant-fold CMP/TEST_ZERO branches and prune phi operands from
 * transitively-unreachable blocks.  Returns the number of instructions
 * rewritten. */
int ssa_opt_branch(struct IRSSAOptCtx *ctx);

