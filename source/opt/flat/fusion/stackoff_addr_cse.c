/*
 *  TCC IR - Fusion & Addressing Mode Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "opt_alias.h"
#include "opt_utils.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);



/* Stack-address ADD-operand CSE.
 *
 * When one StackLoc[X] address appears as an inline literal source in two or
 * more ADDs (each with a vreg other operand), codegen re-materializes
 * `add rX, sp, #off` per use, and the downstream SHL+ADD fusion bails on the
 * is_local base.  Fix: hoist one ASSIGN of that StackLoc to a fresh TEMP at
 * function entry and replace each literal use with the TEMP, giving the ADDs
 * a register base so the indexed-memory fusion can fire.
 *
 * Safety: the ASSIGN sits at function entry (before anything can move the
 * frame pointer), so the address is constant for the whole function; the
 * TEMP holds the same FP-relative pointer as the literal.
 */
int tcc_ir_opt_stackoff_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  /* Pass 1: count uses per unique StackLoc offset. */
#define SAC_MAX_OFFSETS 32
  struct {
    int32_t offset;
    int count;
    int32_t hoisted_vreg;
    IROperand sample; /* operand we cloned (for btype) */
  } slots[SAC_MAX_OFFSETS];
  int nslots = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    for (int sl = 0; sl < 2; sl++)
    {
      IROperand op = (sl == 0) ? src1 : src2;
      IROperand other = (sl == 0) ? src2 : src1;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF)
        continue;
      if (op.is_lval)
        continue;
      /* Require a vreg other operand (the SHL+ADD pattern); constant-other
       * cases are already handled by stack_addr_cse. */
      if (!irop_has_vreg(other))
        continue;
      int32_t off = op.u.imm32;
      int slot = -1;
      for (int s = 0; s < nslots; s++)
        if (slots[s].offset == off) { slot = s; break; }
      if (slot < 0)
      {
        if (nslots >= SAC_MAX_OFFSETS)
          continue;
        slot = nslots++;
        slots[slot].offset = off;
        slots[slot].count = 0;
        slots[slot].hoisted_vreg = -1;
        slots[slot].sample = op;
      }
      slots[slot].count++;
    }
  }

  /* Pass 2: for each offset with >= 2 uses, hoist an ASSIGN at function entry. */
  int changes = 0;
  for (int s = 0; s < nslots; s++)
  {
    if (slots[s].count < 2)
      continue;

    int32_t t_anon = tcc_ir_vreg_alloc_temp(ir);
    if (t_anon < 0)
      continue;

    /* Mirror the sample operand's btype/sign to keep the IR consistent. */
    IROperand new_dest = irop_make_vreg(t_anon, slots[s].sample.btype);
    new_dest.is_unsigned = slots[s].sample.is_unsigned;
    IROperand new_src = slots[s].sample;
    IRQuadCompact assign_q = {0};
    assign_q.op = TCCIR_OP_ASSIGN;
    assign_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, new_src);

    if (gsym_cse_insert_before(ir, 0, &assign_q) < 0)
      continue;
    n++;
    slots[s].hoisted_vreg = t_anon;
  }

  if (changes >= 0)
  {
    /* Pass 3: rewrite uses (inserts shifted every index, so iterate fresh). */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ADD)
        continue;
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      for (int sl = 0; sl < 2; sl++)
      {
        IROperand op = (sl == 0) ? src1 : src2;
        IROperand other = (sl == 0) ? src2 : src1;
        if (irop_get_tag(op) != IROP_TAG_STACKOFF || op.is_lval)
          continue;
        if (!irop_has_vreg(other))
          continue;
        int32_t off = op.u.imm32;
        int slot = -1;
        for (int s = 0; s < nslots; s++)
          if (slots[s].offset == off) { slot = s; break; }
        if (slot < 0 || slots[slot].hoisted_vreg < 0)
          continue;
        IROperand replacement = irop_make_vreg(slots[slot].hoisted_vreg, op.btype);
        replacement.is_unsigned = op.is_unsigned;
        if (sl == 0)
          tcc_ir_set_src1(ir, i, replacement);
        else
          tcc_ir_set_src2(ir, i, replacement);
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== STACKOFF ADDR CSE: %d uses rewritten ===", changes);
  return changes;
#undef SAC_MAX_OFFSETS
}
