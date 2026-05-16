/*
 *  TCC IR - Transform Primitives (shared pre-SSA optimization helpers)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_XFORM_H
#define TCC_IR_OPT_XFORM_H

#include "ir.h"

/* NOP out an instruction in place. */
static inline void ir_xform_nop(TCCIRState *ir, int idx)
{
  ir->compact_instructions[idx].op = TCCIR_OP_NOP;
}

/* Return 1 if no JUMP/JUMPIF appears strictly between from_idx and to_idx
 * (i.e. in the open interval (from_idx, to_idx)). NOP is not a boundary --
 * NOPs are nominally absent (compact_nops removes them) and never end a
 * basic block.  Callers that need defensive abort-on-NOP semantics should
 * keep their own loop. */
int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx);

#endif /* TCC_IR_OPT_XFORM_H */