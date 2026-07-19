/*
 *  TCC IR - Transform legality range checks
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx)
{
  for (int j = from_idx + 1; j < to_idx; j++)
  {
    TccIrOp bop = ir->compact_instructions[j].op;
    if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
      return 0;
  }
  return 1;
}

int ir_xform_range_preserves_memory(TCCIRState *ir, int lo, int hi)
{
  if (hi < lo)
    return 0;
  for (int k = lo + 1; k < hi; k++) {
    const IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* A jump target lets another path's stores execute between the operand's old and new read points. */
    if (q->is_jump_target)
      return 0;
    switch (q->op) {
    /* control flow — the range is not straight-line */
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    /* memory writers / barriers */
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
      return 0;
    default:
      break;
    }
  }
  return 1;
}