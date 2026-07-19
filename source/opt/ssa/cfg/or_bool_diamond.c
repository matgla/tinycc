/*
 *  TCC IR - OR-bool diamond fold (ssa:or_bool_diamond residual)
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
#include "opt_du.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt/flat/branch.h"

/* ssa:or_bool_diamond — fold `acc |= (cond ? 1 : 0)` slot materialization
 * into per-arm ORs; see docs/plan_legacy_or_bool_diamond_ssa.md. */
int ssa_opt_or_bool_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 6)
    return 0;

  for (int i_or = 3; i_or < n; i_or++)
  {
    IRQuadCompact *q_or = &ir->compact_instructions[i_or];
    if (q_or->op != TCCIR_OP_OR)
      continue;

    /* One OR operand must be a no-vreg STACKOFF slot, the other the accumulator. */
    IROperand or_dest = tcc_ir_op_get_dest(ir, q_or);
    IROperand or_src1 = tcc_ir_op_get_src1(ir, q_or);
    IROperand or_src2 = tcc_ir_op_get_src2(ir, q_or);
    int s1_is_slot = (irop_get_tag(or_src1) == IROP_TAG_STACKOFF && irop_get_vreg(or_src1) < 0);
    int s2_is_slot = (irop_get_tag(or_src2) == IROP_TAG_STACKOFF && irop_get_vreg(or_src2) < 0);
    IROperand slot;
    if (s2_is_slot && !s1_is_slot)
      slot = or_src2;
    else if (s1_is_slot && !s2_is_slot)
      slot = or_src1;
    else
      continue;

    /* Falling-into-merge STORE: ir[i_or - 1] writes `slot` with an immediate. */
    int i_st_f = i_or - 1;
    if (i_st_f < 0)
      continue;
    IRQuadCompact *q_st_f = &ir->compact_instructions[i_st_f];
    if (q_st_f->op != TCCIR_OP_STORE)
      continue;
    if (!stackoff_same_slot(tcc_ir_op_get_dest(ir, q_st_f), slot))
      continue;
    IROperand st_f_src = tcc_ir_op_get_src1(ir, q_st_f);
    if (!irop_is_immediate(st_f_src))
      continue;
    int64_t val_f = irop_get_imm64_ex(ir, st_f_src);

    /* Unique JUMP targeting i_or; the STORE just before it writes the true-arm value. */
    int i_jmp = -1;
    int multi = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt != i_or)
        continue;
      if (qj->op != TCCIR_OP_JUMP)
      {
        multi = 1;
        break;
      }
      if (i_jmp >= 0)
      {
        multi = 1;
        break;
      }
      i_jmp = j;
    }
    if (multi || i_jmp <= 0)
      continue;

    int i_st_t = i_jmp - 1;
    IRQuadCompact *q_st_t = &ir->compact_instructions[i_st_t];
    if (q_st_t->op != TCCIR_OP_STORE)
      continue;
    if (!stackoff_same_slot(tcc_ir_op_get_dest(ir, q_st_t), slot))
      continue;
    IROperand st_t_src = tcc_ir_op_get_src1(ir, q_st_t);
    if (!irop_is_immediate(st_t_src))
      continue;
    int64_t val_t = irop_get_imm64_ex(ir, st_t_src);

    /* Only handle val_t=1, val_f=0 for now (the common `bool |= 1` shape). */
    if (val_t != 1 || val_f != 0)
      continue;

    /* Find the JUMPIF whose target is i_st_f (the false-branch STORE). */
    int i_jmpif = -1;
    for (int j = 0; j < i_st_t; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt == i_st_f)
      {
        i_jmpif = j;
        break;
      }
    }
    if (i_jmpif < 0)
      continue;

    /* i_st_f must be a jump target only from i_jmpif (no other jumps in). */
    int extra_target = 0;
    for (int j = 0; j < n && !extra_target; j++)
    {
      if (j == i_jmpif || j == i_jmp)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt == i_st_f || tgt == i_or)
        extra_target = 1;
    }
    if (extra_target)
      continue;
    /* SWITCH_TABLE targets too. */
    for (int t = 0; t < ir->num_switch_tables && !extra_target; t++)
    {
      TCCIRSwitchTable *st = &ir->switch_tables[t];
      for (int k = 0; k < st->num_entries; k++)
        if (st->targets[k] == i_st_f || st->targets[k] == i_or)
          extra_target = 1;
      if (st->default_target == i_st_f || st->default_target == i_or)
        extra_target = 1;
    }
    if (extra_target)
      continue;

    /* Slot used only at i_st_t/i_st_f/i_or; VARs sharing the offset don't count (non-overlapping slot reuse). */
    int extra_use = 0;
#define ORBD_REFS_SLOT(op_) (operand_references_slot((op_), slot) && irop_get_vreg(op_) < 0)
    for (int j = 0; j < n && !extra_use; j++)
    {
      if (j == i_st_t || j == i_st_f || j == i_or)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qj->op].has_dest && ORBD_REFS_SLOT(tcc_ir_op_get_dest(ir, qj)))
        extra_use = 1;
      if (irop_config[qj->op].has_src1 && ORBD_REFS_SLOT(tcc_ir_op_get_src1(ir, qj)))
        extra_use = 1;
      if (irop_config[qj->op].has_src2 && ORBD_REFS_SLOT(tcc_ir_op_get_src2(ir, qj)))
        extra_use = 1;
    }
#undef ORBD_REFS_SLOT
    if (extra_use)
      continue;

    /* Sanity: i_st_t and the true arm must come after i_jmpif. */
    if (i_st_t <= i_jmpif)
      continue;

    LOG_IR_GEN("OPTIMIZE: OR bool diamond at i_or=%d (jmpif=%d, st_t=%d, jmp=%d, st_f=%d)", i_or, i_jmpif, i_st_t, i_jmp,
               i_st_f);

    IROperand acc_dest = or_dest;
    /* Accumulator may itself be a vreg-carrying STACKOFF VAR — don't filter on tag. */
    IROperand acc_src = s2_is_slot ? or_src1 : or_src2;
    int acc_btype = irop_get_btype(acc_dest);
    IROperand one_imm = irop_make_imm32(-1, 1, acc_btype);

    /* True arm: STORE (2 ops) becomes OR (3 ops) — needs fresh pool operand slots. */
    tcc_ir_pool_ensure(ir, 3);
    uint32_t new_base = (uint32_t)ir->iroperand_pool_count;
    ir->iroperand_pool[new_base + 0] = acc_dest;
    ir->iroperand_pool[new_base + 1] = acc_src;
    ir->iroperand_pool[new_base + 2] = one_imm;
    ir->iroperand_pool_count += 3;
    q_st_t->op = TCCIR_OP_OR;
    q_st_t->operand_base = new_base;

    /* False arm: dst = src (src|0); ASSIGN and STORE share {dest,src1}, edit in place. */
    q_st_f->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_dest(ir, i_st_f, acc_dest);
    tcc_ir_set_src1(ir, i_st_f, acc_src);

    ir->compact_instructions[i_or].op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

