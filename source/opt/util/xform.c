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
#include "opt_range.h"
#include "opt_utils.h"

int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx)
{
  /* Control flow only: what the instructions do is the caller's business. */
  const uint32_t not_control = IR_HZ_CALL | IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_MEM_READ | IR_HZ_MEM_WRITE |
                               IR_HZ_UPDATES_SRC | IR_HZ_VLA | IR_HZ_CHAIN | IR_HZ_HINT | IR_HZ_FLAGS_SET |
                               IR_HZ_FLAGS_READ | IR_HZ_VOLATILE | IR_HZ_DEST_LVAL | IR_HZ_DEST_STACKOFF |
                               IR_HZ_SRC_LVAL;
  if (to_idx < from_idx)
    return 1; /* IR_LEGACY_GAP: an inverted range counts as one block */
  return ir_range_safe_except(ir, from_idx, to_idx,
                              IR_HZ_ALL & ~(not_control | IR_LEGACY_GAP_HZ(IR_HZ_JOIN | IR_HZ_JOIN_END | IR_HZ_RETURN |
                                                                           IR_HZ_TRAP | IR_HZ_NONLOCAL | IR_HZ_ASM)),
                              IR_LEGACY_GAP_OPS(TCCIR_OP_IJUMP, TCCIR_OP_SWITCH_TABLE));
}

int ir_xform_range_preserves_memory(TCCIRState *ir, int lo, int hi)
{
  /* Reads, the flags and register-only call setup leave memory alone.  A jump
   * target lets another path's stores execute between the operand's old and
   * new read points (IR_HZ_JOIN, NOP ones included) -- `hi` itself too
   * (IR_HZ_JOIN_END: a back edge entering there).  A store through a
   * destination operand is a write, a volatile access in between would be
   * reordered with the moved read, and a TRAP ends the path. */
  return ir_range_safe(ir, lo, hi,
                       IR_HZ_ALL & ~(IR_HZ_MEM_READ | IR_HZ_SRC_LVAL | IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ |
                                     IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_UPDATES_SRC | IR_HZ_HINT));
}