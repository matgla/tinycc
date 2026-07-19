/*
 *  TCC IR - Stack-slot aliasing helpers (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_alias.h"

int ir_opt_store_btype_size_bytes(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

int ir_opt_stack_slot_range_for_offset(const TCCIRState *ir, int64_t frame_offset,
                                       int64_t *base_out, int64_t *end_out)
{
  const TCCStackSlot *slot;

  if (!ir)
    return 0;

  slot = tcc_ir_stack_slot_by_offset(ir, (int)frame_offset);
  if (!slot)
  {
    for (int si = 0; si < ir->stack_layout.slot_count; ++si)
    {
      const TCCStackSlot *candidate = &ir->stack_layout.slots[si];
      int64_t candidate_base = candidate->offset;
      int64_t candidate_end = candidate_base + candidate->size;

      if (candidate->size <= 0)
        continue;
      if (frame_offset >= candidate_base && frame_offset < candidate_end)
      {
        slot = candidate;
        break;
      }
    }
  }

  if (!slot || slot->size <= 0)
    return 0;

  *base_out = slot->offset;
  *end_out = (int64_t)slot->offset + slot->size;
  return 1;
}

int stackoff_same_slot(IROperand a, IROperand b)
{
  if (irop_get_tag(a) != IROP_TAG_STACKOFF || irop_get_tag(b) != IROP_TAG_STACKOFF)
    return 0;
  return a.u.imm32 == b.u.imm32 && a.is_local == b.is_local && a.is_llocal == b.is_llocal;
}

int operand_references_slot(IROperand op, IROperand slot)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  return op.u.imm32 == slot.u.imm32 && op.is_local == slot.is_local && op.is_llocal == slot.is_llocal;
}

int is_stack_address_operand(IROperand op)
{
  return op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF;
}

int find_deref_use_operand(TCCIRState *ir, int consumer_idx, int32_t vreg, int *which_out)
{
  IRQuadCompact *q = &ir->compact_instructions[consumer_idx];
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int matches = 0;
  int which = 0;

  if (cfg->has_src1)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval && irop_has_vreg(s) && irop_get_vreg(s) == vreg)
    {
      matches++;
      which = 1;
    }
  }
  if (cfg->has_src2)
  {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (s.is_lval && irop_has_vreg(s) && irop_get_vreg(s) == vreg)
    {
      matches++;
      which = 2;
    }
  }
  if (cfg->has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval && irop_has_vreg(d) && irop_get_vreg(d) == vreg)
    {
      matches++;
      which = 0;
    }
  }
  if (matches != 1)
    return 0;
  *which_out = which;
  return 1;
}
