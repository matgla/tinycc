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


int tcc_ir_opt_add_deref_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!irop_is_immediate(src2))
      continue;
    int32_t imm = (int32_t)irop_get_imm64_ex(ir, src2);
    if (imm < 0 || imm > 4095)
      continue;
    int32_t base_vr = irop_get_vreg(src1);
    if (base_vr < 0)
      continue;
    if (src1.is_local || src1.is_llocal)
      continue;
    /* Only fold PARAM bases: LOAD_INDEXED can expose stack loads to const
     * propagation that may wrongly fold across calls aliasing memory. PARAM
     * vregs point to caller-owned memory, safe from this. Peep-through: a
     * TEMP whose only def is an ASSIGN copy from a PARAM counts as that PARAM
     * base (copy_prop doesn't always run before this pass). */
    if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_PARAM)
    {
      if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* Bounded backward scan for `ASSIGN T <- PARAM` near the ADD; the
       * ~16-instr window avoids O(n^2) on stress tests. Bail on any
       * branch/store/call before the def to keep semantics local. */
      int copy_idx = -1;
      int max_back = 16;
      for (int j = i - 1; j >= 0 && (i - j) <= max_back; j--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[j];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_JUMP || cq->op == TCCIR_OP_JUMPIF ||
            cq->op == TCCIR_OP_STORE || cq->op == TCCIR_OP_STORE_INDEXED ||
            cq->op == TCCIR_OP_STORE_POSTINC || cq->op == TCCIR_OP_FUNCCALLVAL ||
            cq->op == TCCIR_OP_FUNCCALLVOID)
          break;
        if (irop_config[cq->op].has_dest)
        {
          IROperand cd = tcc_ir_op_get_dest(ir, cq);
          if (irop_get_vreg(cd) == base_vr && !cd.is_lval)
          {
            copy_idx = j;
            break;
          }
        }
      }
      if (copy_idx < 0)
        continue;
      IRQuadCompact *cq = &ir->compact_instructions[copy_idx];
      if (cq->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cd = tcc_ir_op_get_dest(ir, cq);
      if (cs1.is_lval || cd.is_lval)
        continue;
      int32_t cs1_vr = irop_get_vreg(cs1);
      if (cs1_vr < 0 || TCCIR_DECODE_VREG_TYPE(cs1_vr) != TCCIR_VREG_TYPE_PARAM)
        continue;
      /* Use the PARAM source as base; leave the copy for DCE. Extra readers
       * of the TEMP base are fine since the copy stays put. */
      src1 = cs1;
      base_vr = cs1_vr;
    }

    /* Fast pre-filter: skip if T has != 1 use (O(1) via shared DU). */
    if (ir_opt_du_uses(&du, dest_vr) != 1)
      continue;

    int use_idx = -1;
    int use_is_deref = 0;
    int use_in_src2 = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[uq->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        { use_idx = j; use_is_deref = s.is_lval; use_in_src2 = 0; break; }
      }
      if (irop_config[uq->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        { use_idx = j; use_is_deref = s.is_lval; use_in_src2 = 1; break; }
      }
      if ((uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED) &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == dest_vr)
      { use_idx = j; break; }
    }

    if (!use_is_deref || use_idx < 0)
      continue;

    /* No branch between ADD and its deref use: a branch could route through
     * a path storing to [base+imm], making the early load see stale data. */
    {
      int cross_block = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        TccIrOp bop = ir->compact_instructions[j].op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
        {
          cross_block = 1;
          break;
        }
      }
      if (cross_block)
        continue;
    }

    /* Fold moves the load up to the ADD site; a store/call between them
     * could make it see stale data. Bail if so. */
    {
      int has_side_effect = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE_POSTINC ||
            sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
        {
          has_side_effect = 1;
          break;
        }
      }
      if (has_side_effect)
        continue;
    }

    /* The DEREF use's btype sets the load width; the ADD dest's pointer
     * btype may differ from the loaded value type. */
    IRQuadCompact *uq_pre = &ir->compact_instructions[use_idx];
    IROperand use_op = use_in_src2 ? tcc_ir_op_get_src2(ir, uq_pre) : tcc_ir_op_get_src1(ir, uq_pre);
    int load_btype = irop_get_btype(use_op);

    /* Skip 64-bit and struct loads: LOAD_INDEXED uses LDRD which requires
     * 4-byte alignment.  Packed structs can place 64-bit fields at
     * unaligned offsets, causing a HardFault. */
    if (load_btype == IROP_BTYPE_INT64 || load_btype == IROP_BTYPE_FLOAT64 || load_btype == IROP_BTYPE_STRUCT)
      continue;

    IROperand load_dest = dest;
    load_dest.btype = load_btype;

    /* Convert ADD to LOAD_INDEXED: allocate 4 contiguous pool entries */
    IROperand scale_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    int new_base = tcc_ir_pool_add(ir, load_dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
    tcc_ir_pool_add(ir, scale_op);
    q->operand_base = new_base;
    q->op = TCCIR_OP_LOAD_INDEXED;

    /* Clear DEREF on the use site — the value is now loaded, not a pointer. */
    IRQuadCompact *uq = &ir->compact_instructions[use_idx];
    if (irop_config[uq->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src1(ir, use_idx, s);
      }
    }
    if (irop_config[uq->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src2(ir, use_idx, s);
      }
    }

    changes++;
  }

  tcc_free(du.def);
  return changes;
}
