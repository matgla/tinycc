/*
 *  TCC SSA opt - narrowing / extension folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Shift-pair folding, redundant AND elimination, SHR→UBFX fusion, and the
 * soft-FP double→float demotion fold (f2d→mathfn→[d2f] → mathfn-f).
 * Returns the number of instructions rewritten. */
int ssa_opt_narrow(struct IRSSAOptCtx *ctx);

/* Double→float immediate narrowing used by the demotion fold: builds the F32
 * immediate when `op` is a double immediate whose value is exactly a float
 * (IMM32+FLOAT64 integer shorthand always qualifies; F64/I64 pool constants
 * must round-trip exactly).  Exposed for unit tests. */
struct TCCIRState;
int tcc_ir_ssa_narrow_f64_imm_exact_f32(struct TCCIRState *ir, IROperand op, IROperand *out);

