/*
 *  TCC SSA opt - reassociation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Reassociate associative/commutative ops so constant operands bubble together
 * for the fold pass.  Returns the number of instructions rewritten. */
int ssa_opt_reassoc(struct IRSSAOptCtx *ctx);

