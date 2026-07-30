/*
 *  TCC IR - Complex-constant param folding (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* Fold two const-float component stores + complex FUNCPARAMVAL into a packed 64-bit immediate. */
int tcc_ir_opt_complex_const_param_fold(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF)
      continue;
    if (!src1.is_lval || !src1.is_complex)
      continue;
    /* Only handle _Complex float (8 bytes packed: real_u32 | imag_u32 << 32) */
    if (src1.btype != IROP_BTYPE_FLOAT32)
      continue;
    /* Skip stack offsets carrying a vreg (spill slots), not raw locals. */
    if (irop_get_vreg(src1) != -1)
      continue;
    /* Skip incoming-arg stack slots; they alias caller-allocated memory. */
    if (src1.is_param)
      continue;

    int real_off = (int)irop_get_stack_offset(src1);
    int imag_off = real_off + 4;

    int real_store_idx = -1;
    int imag_store_idx = -1;
    uint32_t real_bits = 0;
    uint32_t imag_bits = 0;
    int conflict = 0;

    for (int j = 0; j < n && !conflict; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP)
        continue;

      /* dest: detect a 4-byte constant STORE/ASSIGN to either component slot. */
      if (irop_config[p->op].has_dest)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, p);
        if (irop_get_tag(dest) == IROP_TAG_STACKOFF && irop_get_vreg(dest) == -1 && dest.is_lval && !dest.is_param)
        {
          int doff = (int)irop_get_stack_offset(dest);
          if (doff == real_off || doff == imag_off)
          {
            if ((p->op != TCCIR_OP_STORE && p->op != TCCIR_OP_ASSIGN) || dest.is_complex ||
                dest.btype != IROP_BTYPE_FLOAT32)
            {
              conflict = 1;
              break;
            }
            IROperand sv = tcc_ir_op_get_src1(ir, p);
            int tag = irop_get_tag(sv);
            if (sv.is_sym || sv.is_lval || sv.is_complex)
            {
              conflict = 1;
              break;
            }
            if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_F32)
            {
              conflict = 1;
              break;
            }
            uint32_t bits = (uint32_t)irop_get_imm64_ex(ir, sv);
            if (doff == real_off)
            {
              if (real_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              real_store_idx = j;
              real_bits = bits;
            }
            else
            {
              if (imag_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              imag_store_idx = j;
              imag_bits = bits;
            }
            continue;
          }
        }
      }

      /* Any other reference to the 8-byte slot disqualifies the fold. */
      for (int s = 0; s < 2; s++)
      {
        if (s == 0 && !irop_config[p->op].has_src1)
          continue;
        if (s == 1 && !irop_config[p->op].has_src2)
          continue;
        IROperand src = s ? tcc_ir_op_get_src2(ir, p) : tcc_ir_op_get_src1(ir, p);
        if (irop_get_tag(src) != IROP_TAG_STACKOFF)
          continue;
        if (irop_get_vreg(src) != -1)
          continue;
        int soff = (int)irop_get_stack_offset(src);
        if (soff >= real_off && soff < imag_off + 4)
        {
          conflict = 1;
          break;
        }
      }
    }

    if (conflict || real_store_idx < 0 || imag_store_idx < 0)
      continue;

    /* Pack {real, imag} into a 64-bit complex-float immediate. */
    uint64_t packed = (uint64_t)real_bits | ((uint64_t)imag_bits << 32);
    uint32_t pool_idx = tcc_ir_pool_add_i64(ir, (int64_t)packed);
    IROperand new_src1 = irop_make_i64(-1, pool_idx, IROP_BTYPE_FLOAT32);
    new_src1.is_complex = 1;
    new_src1.is_lval = 0;

    tcc_ir_set_src1(ir, i, new_src1);
    ir->compact_instructions[real_store_idx].op = TCCIR_OP_NOP;
    ir->compact_instructions[imag_store_idx].op = TCCIR_OP_NOP;

    LOG_IR_GEN("=== COMPLEX CONST PARAM FOLD: stack[%d..%d] folded into PARAM at i=%d ===", real_off, real_off + 7, i);
    changes++;
  }

  return changes;
}

