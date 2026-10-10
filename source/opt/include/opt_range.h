/*
 *  TCC IR - Hazard queries over instructions and ranges, block cursors
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir.h"

/* Deny by default.  A pass asks whether a range is free of hazards with
 * IR_HZ_ALL minus the classes it has proven harmless, never with a list of the
 * ones it thought of:
 *
 *   if (!ir_range_safe(ir, def, use, IR_HZ_ALL & ~(IR_HZ_MEM_READ | IR_HZ_SRC_LVAL)))
 *     return 0;  // a store, call, asm, join, volatile access, ... in between
 *
 * The hazard bits are in op_props.h.  ir_range_safe_except exists to wrap a
 * legacy helper bit-exactly: its op list names the ops the helper never
 * checked, so every gap is written down where it can be found and closed. */

/* The hazards in `mask` that instruction `q` has: its opcode's
 * (ir_op_props[]) and the instruction-level ones.  Operand checks run only for
 * the bits asked for. */
uint32_t ir_q_hazards(TCCIRState *ir, const IRQuadCompact *q, uint32_t mask);

/* As ir_q_hazards, but the opcode hazards of the ops in `except` are ignored.
 * Instruction-level hazards (join, volatile, lvalue/STACKOFF operands) still
 * apply to them. */
uint32_t ir_q_hazards_except(TCCIRState *ir, const IRQuadCompact *q, uint32_t mask, IROpSet except);

/* The first instruction strictly between lo and hi with a hazard in `mask`, or
 * -1 when there is none.  IR_HZ_JOIN looks at the jump targets in (lo, hi),
 * NOPs included; IR_HZ_JOIN_END at hi itself (another path reaches hi without
 * passing lo), and is reported as hi.  hi < lo is a hazard (returns lo);
 * hi == lo is an empty range. */
int ir_range_first_hazard(TCCIRState *ir, int lo, int hi, uint32_t mask);
int ir_range_first_hazard_except(TCCIRState *ir, int lo, int hi, uint32_t mask, IROpSet except);

static inline int ir_range_safe(TCCIRState *ir, int lo, int hi, uint32_t mask)
{
  return ir_range_first_hazard(ir, lo, hi, mask) < 0;
}

static inline int ir_range_safe_except(TCCIRState *ir, int lo, int hi, uint32_t mask, IROpSet except)
{
  return ir_range_first_hazard_except(ir, lo, hi, mask, except) < 0;
}

/* Block cursors: the next/previous non-NOP instruction in the same basic
 * block, or -1.  A block ends at a jump target -- a NOP one too, which is
 * what a scan that skips NOPs first gets wrong -- and after an instruction
 * that may branch, does not fall through, or returns twice (setjmp).
 *
 *   for (int k = ir_bb_next(ir, i, n); k >= 0; k = ir_bb_next(ir, k, n))
 *
 * `limit` bounds the walk: next stops before it, prev stops below it. */
int ir_bb_next(TCCIRState *ir, int i, int limit);
int ir_bb_prev(TCCIRState *ir, int i, int limit);
