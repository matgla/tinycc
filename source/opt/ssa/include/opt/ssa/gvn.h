/*
 *  TCC SSA opt - global value numbering (dominator-tree GVN)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Dominator-tree global value numbering: hash pure instructions by
 * (opcode, operands) and rewrite a redundant instruction dominated by a
 * congruent one into ASSIGN dest = earlier-result.  Returns the number of
 * instructions rewritten. */
int ssa_opt_gvn(struct IRSSAOptCtx *ctx);

