/*
 *  TCC IR - Transform Primitives (shared pre-SSA optimization helpers)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir.h"

static inline void ir_xform_nop(TCCIRState *ir, int idx)
{
  ir->compact_instructions[idx].op = TCCIR_OP_NOP;
}

/* 1 if no JUMP/JUMPIF strictly between from_idx and to_idx; NOP is not a boundary. */
int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx);

/* Folds `T = V OP src; V = T` into `V = V OP src; NOP`; returns number of folds. */
int tcc_ir_opt_store_inplace_arith(TCCIRState *ir);
struct IROptCtx;
int tcc_ir_opt_store_inplace_arith_ex(struct IROptCtx *ctx);

/* is_lval/is_llocal operands are fused memory reads, evaluated when the instruction runs. */
static inline int ir_xform_operand_reads_memory(IROperand op)
{
  return op.is_lval || op.is_llocal;
}

/* 1 if (lo,hi) is straight-line and writes no memory, so a memory read may move across it. */
int ir_xform_range_preserves_memory(TCCIRState *ir, int lo, int hi);
