/*
 *  TCC SSA opt - constant folding (ALU ops with constant operands)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;
struct TCCIRState;

/* Constant folding: evaluate ALU ops with two immediate operands at compile
 * time, replacing the instruction with ASSIGN dest = #result.  Also handles
 * algebraic identities (x+0, x*1, x|x, x^x→0, etc.), bit-complement collapse,
 * double-negation, and barrel-shift-annotated folds.  Returns the number of
 * instructions rewritten. */
int ssa_opt_fold(struct IRSSAOptCtx *ctx);


/* Every reader of the flags the compare at `c` sets only asks whether the
 * compared value is zero: EQ/NE, and with unsigned_ok also the unsigned `> 0`
 * and `<= 0` of a `CMP x,#0`.  Readers are followed to the next flag write, a
 * return or a jump (whose target must not read them either). */
int ssa_opt_flags_test_zero_only(struct TCCIRState *ir, int c, int unsigned_ok);

/* Swap EQ and NE in every reader of the flags the compare at `c` sets (the
 * readers ssa_opt_flags_test_zero_only accepted). */
void ssa_opt_flags_invert_zero_test(struct TCCIRState *ir, int c);
