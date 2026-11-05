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
  return a.u.imm32 == b.u.imm32 && irop_is_local(a) == irop_is_local(b) && irop_is_llocal(a) == irop_is_llocal(b);
}

int operand_references_slot(IROperand op, IROperand slot)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  return op.u.imm32 == slot.u.imm32 && irop_is_local(op) == irop_is_local(slot) && irop_is_llocal(op) == irop_is_llocal(slot);
}

int is_stack_address_operand(IROperand op)
{
  return irop_is_local(op) && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF;
}

int find_deref_use_operand(TCCIRState *ir, int consumer_idx, int32_t vreg, int *which_out)
{
  IRQuadCompact *q = &ir->compact_instructions[consumer_idx];
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int matches = 0;
  int which = 0;

  if (cfg->has_src1)
  {
    if (tcc_ir_op_src1_is_lval(ir, q) && tcc_ir_op_src1_has_vreg(ir, q) && tcc_ir_op_src1_vreg(ir, q) == vreg)
    {
      matches++;
      which = 1;
    }
  }
  if (cfg->has_src2)
  {
    if (tcc_ir_op_src2_is_lval(ir, q) && tcc_ir_op_src2_has_vreg(ir, q) && tcc_ir_op_src2_vreg(ir, q) == vreg)
    {
      matches++;
      which = 2;
    }
  }
  if (cfg->has_dest)
  {
    if (tcc_ir_op_dest_is_lval(ir, q) && tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == vreg)
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

int ir_opt_addr_walk(TCCIRState *ir, int32_t vr, int before_idx, const IRAddrWalk *w, IRAddrKey *out)
{
  int64_t acc = 0;
  for (int hop = 0; hop < w->max_hops; hop++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (p >= w->n_temps)
      return 0;
    int d;
    if (w->flags & IR_AW_LATEST_DEF)
    {
      if (w->def_count && w->def_count[p] != 1)
        return 0;
      d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
    }
    else
      d = w->def_at ? w->def_at[p] : -1;
    if (d < 0)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    int sub = dq->op == TCCIR_OP_SUB && (w->flags & IR_AW_SUB);
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN && !sub)
      return 0;
    if (dq->op == TCCIR_OP_ADD || sub)
    {
      if (!tcc_ir_op_src2_is_imm(ir, dq))
        return 0;
      int64_t c = tcc_ir_op_src2_imm(ir, dq);
      acc += sub ? -c : c;
    }
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if ((w->flags & IR_AW_SYMREF) && irop_is_sym(s1) && !s1.is_lval)
    {
      IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
      if (!sr || !sr->sym)
        return 0;
      out->sym = sr->sym;
      out->off = (int64_t)sr->addend + acc;
      out->def_idx = d;
      return 1;
    }
    /* A direct frame slot: no vreg, or (IR_AW_TEMP_SLOT) a frontend temporary's
     * negative one.  One with a VAR's vreg is `&VAR`, stepped through below and
     * refused there as a non-TEMP. */
    int s1vr = irop_get_tag(s1) == IROP_TAG_STACKOFF ? irop_get_vreg(s1) : 0;
    if (irop_get_tag(s1) == IROP_TAG_STACKOFF && irop_is_local(s1) && !s1.is_lval &&
        (s1vr == -1 || (s1vr < 0 && (w->flags & IR_AW_TEMP_SLOT))))
    {
      out->sym = NULL;
      out->off = (int64_t)irop_get_stack_offset(s1) + acc;
      out->def_idx = d;
      return 1;
    }
    if (s1.is_lval)
      return 0;
    if (!(w->flags & IR_AW_ANY_VREG) && irop_get_tag(s1) != IROP_TAG_VREG)
      return 0;
    int32_t nv = irop_get_vreg(s1);
    if (nv < 0)
      return 0;
    vr = nv;
    before_idx = d;
  }
  return 0;
}
