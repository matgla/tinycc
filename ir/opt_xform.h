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

/* In-place arithmetic peephole: fold `T = V OP src; V = T [STORE]` into
 * `V = V OP src; NOP`, saving one mov at codegen.  Returns number of folds. */
int tcc_ir_opt_store_inplace_arith(TCCIRState *ir);
struct IROptCtx;
int tcc_ir_opt_store_inplace_arith_ex(struct IROptCtx *ctx);

/* An operand with is_lval (or is_llocal) is a fused memory read — a stack
 * slot, a deref through a pointer, or a global — evaluated when the
 * instruction executes, not when the operand's vreg was defined. */
static inline int ir_xform_operand_reads_memory(IROperand op)
{
  return op.is_lval || op.is_llocal;
}

/* Moving an instruction's memory-read operand to a different program point
 * changes which value the load observes if any store to that location can
 * execute in between.  Return 1 when every instruction strictly between lo
 * and hi is straight-line (no control flow in or out, no jump targets) and
 * cannot write memory, so a memory read may be moved between lo and hi
 * safely. */
int ir_xform_range_preserves_memory(TCCIRState *ir, int lo, int hi);

#endif /* TCC_IR_OPT_XFORM_H */