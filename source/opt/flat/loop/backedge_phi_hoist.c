/*
 *  TCC IR - Back-edge phi-copy hoisting into an inverted latch branch
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
#include "opt_utils.h"
#include "opt_loop_utils.h"

int tcc_ir_opt_backedge_phi_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 4) return 0;

  int changes = 0;

  for (int i = 0; i < n - 2; i++) {
    IRQuadCompact *jif = &ir->compact_instructions[i];
    if (jif->op != TCCIR_OP_JUMPIF)
      continue;

    int exit_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jif));
    int cond = (int)tcc_ir_op_get_src1(ir, jif).u.imm32;

    if (exit_target <= i)
      continue;

    int num_assigns = 0;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_ASSIGN)
        num_assigns++;
      else
        break;
    }
    if (num_assigns == 0 || num_assigns > 8)
      continue;

    int jump_idx = i + 1 + num_assigns;
    if (jump_idx >= n)
      continue;
    IRQuadCompact *jmp = &ir->compact_instructions[jump_idx];
    if (jmp->op != TCCIR_OP_JUMP)
      continue;

    int body_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jmp));
    if (body_target >= i)
      continue;

    /* exit target must stay fall-through reachable past the JUMP */
    if (exit_target < jump_idx)
      continue;

    if (i == 0)
      continue;
    IRQuadCompact *cmp_q = &ir->compact_instructions[i - 1];
    if (cmp_q->op != TCCIR_OP_CMP)
      continue;

    /* spilled operands lower to load/store sequences that disturb pending flags */
    int safe = 1;
    for (int j = 0; j < num_assigns && safe; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      IROperand adst = tcc_ir_op_get_dest(ir, aq);
      IROperand asrc = tcc_ir_op_get_src1(ir, aq);
      int32_t adst_vr = irop_get_vreg(adst);
      int32_t asrc_vr = irop_get_vreg(asrc);

      if (adst_vr >= 0) {
        int spilled = 0;
        for (int k = 0; k < ir->ls.next_interval_index; k++) {
          if (ir->ls.intervals[k].vreg == (uint32_t)adst_vr) {
            if (ir->ls.intervals[k].stack_location != 0 || ir->ls.intervals[k].r0 < 0)
              spilled = 1;
            break;
          }
        }
        if (spilled) safe = 0;
      }
      if (safe && asrc_vr >= 0) {
        int spilled = 0;
        for (int k = 0; k < ir->ls.next_interval_index; k++) {
          if (ir->ls.intervals[k].vreg == (uint32_t)asrc_vr) {
            if (ir->ls.intervals[k].stack_location != 0 || ir->ls.intervals[k].r0 < 0)
              spilled = 1;
            break;
          }
        }
        if (spilled) safe = 0;
      }
    }

    /* an ASSIGN dest feeding the CMP would change the flags it already set */
    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);
    int32_t cmp_vr1 = irop_get_vreg(cmp_src1);
    int32_t cmp_vr2 = irop_get_vreg(cmp_src2);
    for (int j = 0; j < num_assigns && safe; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      IROperand adst = tcc_ir_op_get_dest(ir, aq);
      int32_t adst_vr = irop_get_vreg(adst);
      if (adst_vr >= 0 && (adst_vr == cmp_vr1 || adst_vr == cmp_vr2))
        safe = 0;
    }

    /* a dest used after exit_target before redefinition still needs its pre-ASSIGN value */
    for (int j = 0; j < num_assigns && safe; j++) {
      IROperand adst = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i + 1 + j]);
      int32_t adst_vr = irop_get_vreg(adst);
      if (adst_vr < 0) continue;
      for (int k = exit_target; k < n && safe; k++) {
        IRQuadCompact *eq = &ir->compact_instructions[k];
        if (eq->op == TCCIR_OP_NOP) continue;
        if (irop_config[eq->op].has_src1) {
          if (irop_get_vreg(tcc_ir_op_get_src1(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && irop_config[eq->op].has_src2) {
          if (irop_get_vreg(tcc_ir_op_get_src2(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && eq->op == TCCIR_OP_MLA) {
          if (irop_get_vreg(tcc_ir_op_get_accum(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && irop_config[eq->op].has_dest &&
            irop_get_vreg(tcc_ir_op_get_dest(ir, eq)) == adst_vr) {
          /* STORE-family and is_lval dests hold the address: a use, not a def */
          if (eq->op == TCCIR_OP_STORE || eq->op == TCCIR_OP_STORE_INDEXED ||
              eq->op == TCCIR_OP_STORE_POSTINC ||
              tcc_ir_op_get_dest(ir, eq).is_lval)
            safe = 0;
          else
            break; /* genuine redefinition kills the prior value */
        }
      }
    }

    if (!safe)
      continue;

    /* Side entries into the ASSIGN run (a shared latch reached by several
     * `continue` edges) are retargeted to body_target below, which SKIPS the
     * copies.  That is only sound when every copy is a physical no-op
     * (register allocation gave dest and src the same register — the common
     * graph-coalesced case).  A surviving real `mov` skipped on those edges
     * leaves the loop phi un-updated (20050502-1: `i` never incremented on
     * the y/z break-check continue paths).  Switch-table entries can't be
     * retargeted here at all, so any of those force a bail regardless. */
    {
      int side_entry = 0;
      for (int k = 0; k < n && !side_entry; k++) {
        if (k >= i && k <= jump_idx)
          continue;
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op != TCCIR_OP_JUMP && kq->op != TCCIR_OP_JUMPIF)
          continue;
        int kt = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, kq));
        if (kt >= i + 1 && kt < jump_idx)
          side_entry = 1;
      }
      int switch_entry = 0;
      for (int t = 0; t < ir->num_switch_tables && !switch_entry; t++) {
        TCCIRSwitchTable *tbl = &ir->switch_tables[t];
        if (tbl->default_target >= i + 1 && tbl->default_target <= jump_idx)
          switch_entry = 1;
        for (int j = 0; j < tbl->num_entries && !switch_entry; j++)
          if (tbl->targets[j] >= i + 1 && tbl->targets[j] <= jump_idx)
            switch_entry = 1;
      }
      if (switch_entry)
        continue;
      if (side_entry) {
        int all_noop = 1;
        for (int j = 0; j < num_assigns && all_noop; j++) {
          IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
          int32_t adst_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, aq));
          int32_t asrc_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, aq));
          if (adst_vr < 0 || asrc_vr < 0) { all_noop = 0; break; }
          int dst_reg = -2, dst_reg1 = -2, src_reg = -3, src_reg1 = -3;
          for (int k = 0; k < ir->ls.next_interval_index; k++) {
            LSLiveInterval *li = &ir->ls.intervals[k];
            if (li->vreg == (uint32_t)adst_vr) {
              if (li->stack_location != 0 || li->r0 < 0) { all_noop = 0; break; }
              dst_reg = li->r0; dst_reg1 = li->r1;
            }
            if (li->vreg == (uint32_t)asrc_vr) {
              if (li->stack_location != 0 || li->r0 < 0) { all_noop = 0; break; }
              src_reg = li->r0; src_reg1 = li->r1;
            }
          }
          if (dst_reg != src_reg || dst_reg1 != src_reg1)
            all_noop = 0;
        }
        if (!all_noop)
          continue;
      }
    }

    int inv_cond = invert_condition(cond);
    if (inv_cond < 0)
      continue;

    uint32_t jif_opbase = jif->operand_base;

    uint32_t assign_opbases[8];
    for (int j = 0; j < num_assigns; j++)
      assign_opbases[j] = ir->compact_instructions[i + 1 + j].operand_base;

    for (int j = 0; j < num_assigns; j++) {
      IRQuadCompact *q = &ir->compact_instructions[i + j];
      q->op = TCCIR_OP_ASSIGN;
      q->operand_base = assign_opbases[j];
      q->is_jump_target = (j == 0) ? jif->is_jump_target : 0;
    }

    /* reuse the original JUMPIF's operand pool slot */
    {
      int jif_pos = i + num_assigns;
      IRQuadCompact *q = &ir->compact_instructions[jif_pos];
      q->op = TCCIR_OP_JUMPIF;
      q->operand_base = jif_opbase;
      q->is_jump_target = 0;
      IROperand dest_op = {0};
      dest_op.tag = IROP_TAG_IMM32;
      dest_op.u.imm32 = body_target;
      tcc_ir_op_set_dest(ir, q, dest_op);
      IROperand cond_op = {0};
      cond_op.tag = IROP_TAG_IMM32;
      cond_op.u.imm32 = inv_cond;
      tcc_ir_op_set_src1(ir, q, cond_op);
    }

    /* stale targets into [i+1..jump_idx] meant body_target; left alone they land on the relocated JUMPIF and re-use its flags */
    for (int k = 0; k < n; k++) {
      if (k >= i && k <= jump_idx)
        continue;
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op != TCCIR_OP_JUMP && kq->op != TCCIR_OP_JUMPIF)
        continue;
      int kt = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, kq));
      if (kt < i + 1 || kt > jump_idx)
        continue;
      IROperand kd = {0};
      kd.tag = IROP_TAG_IMM32;
      kd.u.imm32 = body_target;
      tcc_ir_op_set_dest(ir, kq, kd);
    }

    ir->compact_instructions[jump_idx].op = TCCIR_OP_NOP;

    if (body_target >= 0 && body_target < n)
      ir->compact_instructions[body_target].is_jump_target = 1;

    changes++;
  }

  return changes;
}
