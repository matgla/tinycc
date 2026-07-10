/*
 *  TCC IR - Jump Chain Management
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address)
{
  IROperand cur;
  int next;
  if (t < 0)
    return; /* -1 means no chain */

  while (t >= 0 && t < ir->next_instruction_index)
  {
    TccIrOp op = ir->compact_instructions[t].op;

    /* Check if this instruction is actually a jump */
    if (op != TCCIR_OP_JUMP && op != TCCIR_OP_JUMPIF)
    {
      break; /* Don't corrupt non-jump instructions */
    }

    cur = tcc_ir_op_get_dest(ir, &ir->compact_instructions[t]);
    next = cur.u.imm32;
    cur.u.imm32 = target_address;

    /* Sync to iroperand_pool as well to keep both pools in sync */
    const int pool_off = ir->compact_instructions[t].operand_base;
    ir->iroperand_pool[pool_off] = cur;

    /* Mark the target instruction as a jump target.
     * If it already exists, set the flag directly.
     * If it is the next-to-be-created slot (tcc_ir_backpatch_to_here pattern),
     * set a pending flag that tcc_ir_put picks up on creation. */
    if (target_address >= 0 && target_address < ir->next_instruction_index)
      ir->compact_instructions[target_address].is_jump_target = 1;
    else if (target_address == ir->next_instruction_index)
      ir->next_insn_is_jump_target = 1;

    /* Chain ends when next is -1 (sentinel), out of range, or already patched */
    if (next < 0 || next >= ir->next_instruction_index || next == target_address)
      break;
    t = next;
  }
}

void tcc_ir_backpatch_to_here(TCCIRState *ir, int t)
{
  if (!ir)
    return;
  tcc_ir_backpatch(ir, t, ir->next_instruction_index);
  /* A backpatch target is a new basic block boundary — multiple control flow
   * paths converge here. Mark it so that tcc_ir_put() does NOT coalesce the
   * next ASSIGN with the previous instruction, which may belong to a
   * different branch. Without this, ternary operators like
   *   var = cond ? true_expr : false_expr
   * can have their merge-point ASSIGN coalesced into the true-path LOAD,
   * leaving the false-path result disconnected from the variable. */
  if (t >= 0)
  {
    ir->basic_block_start = 1;
    /* Must match CODE_ON() in tccgen.c: clear the CODE_OFF_BIT so that
     * subsequent code is not treated as unreachable.  Without this, a
     * ternary inside a while loop (which uses gjmp → CODE_OFF) leaves the
     * bit set and causes a following switch statement to skip its entire
     * dispatch because sw->nocode_wanted is captured as non-zero.  This
     * mirrors the gsym() function which calls CODE_ON() after patching. */
    nocode_wanted &= ~0x20000000; /* CODE_OFF_BIT */
  }
}

void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address)
{
  int lp, next;
  if (t < 0)
    return; /* -1 means no chain */
  do
  {
    lp = t;
    next = tcc_ir_op_get_dest(ir, &ir->compact_instructions[t]).u.imm32;
    /* Stop if we hit end of chain or go out of bounds */
    if (next < 0 || next >= ir->next_instruction_index)
      break;
    t = next;
  } while (1);
  tcc_ir_pool_jump_target_set(ir, lp, target_address);
}

int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t)
{
  if (n >= 0 && n < ir->next_instruction_index)
  {
    tcc_ir_backpatch_first(ir, n, t);
    return n;
  }
  return t;
}
