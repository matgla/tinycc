/*
 *  TCC SSA opt - CMP equality-fact propagation (CFG/dominator pass)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Walk the dominator tree pushing equality/inequality facts derived from
 * CMP+JEQ / CMP+JNE terminators.  When a dominated block repeats a CMP on the
 * same operand pair, the active fact folds the following JUMPIF to an
 * unconditional JUMP (always taken) or NOP (never taken).  Returns the number
 * of instructions rewritten. */
int ssa_opt_cmp_eq_prop(struct IRSSAOptCtx *ctx);

