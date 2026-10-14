/*
 *  TCC IR - Late Barrel Shift Fusion (runs just before codegen)
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


/* See docs/bugs/indexed-address-shapes-not-fused.md for fusion and safety constraints. */

/* Every JUMP/JUMPIF/switch target, plus anything already flagged. */
static uint8_t *barrel_build_target_map(TCCIRState *ir, int n)
{
  uint8_t *map = tcc_mallocz((size_t)n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
      map[i] = 1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= 0 && t < n)
        map[t] = 1;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int table_id = (int)tcc_ir_op_src2_imm(ir, q);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          if (table->targets[j] >= 0 && table->targets[j] < n)
            map[table->targets[j]] = 1;
        if (table->default_target >= 0 && table->default_target < n)
          map[table->default_target] = 1;
      }
    }
  }
  return map;
}

void tcc_ir_barrel_shift_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return;

  ir->barrel_shifts = tcc_mallocz(ir->max_orig_index + 1);
  ir->barrel_shifts_len = ir->max_orig_index + 1;

  IROptDU du;
  ir_opt_du_build(ir, &du);
  uint8_t *is_target = barrel_build_target_map(ir, n);

  /* Vregs that reach a memory operation's address: an is_lval operand (a real
   * pointer -- is_local/is_llocal reads a stack variable's value, not an
   * address), or any operand of an already-formed indexed/post-inc/LEA access,
   * whose base is src1 for loads but dest for stores.  Marking those ops'
   * value operand too only forfeits a fusion, never breaks one.  One O(n)
   * prepass keeps the per-candidate test O(1). */
  uint8_t *deref_base = du.total > 0 ? tcc_mallocz((size_t)du.total) : NULL;
  if (deref_base)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int addressing = 0;
      switch (q->op)
      {
      case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_LOAD_POSTINC: case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_LEA:
        addressing = 1;
        break;
      default:
        break;
      }
      for (int k = 0; k < 3; k++)
      {
        if (k == 0 ? !irop_config[q->op].has_dest
                   : k == 1 ? !irop_config[q->op].has_src1
                            : !irop_config[q->op].has_src2)
          continue;
        IROperand s = tcc_ir_op_get_slot(ir, q, k);
        if (!irop_has_vreg(s))
          continue;
        if (!addressing && (!s.is_lval || s.is_local || s.is_llocal))
          continue;
        int idx = ir_opt_du_idx(&du, irop_get_vreg(s));
        if (idx >= 0)
          deref_base[idx] = 1;
      }
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    int commutative = 0;
    switch (q->op)
    {
    case TCCIR_OP_ADD: case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
      commutative = 1;
      break;
    case TCCIR_OP_SUB: case TCCIR_OP_CMP:
      break;
    default:
      continue;
    }

    /* Try fusing on src2 first; for commutative ops, also try src1 (swapping
     * operands so the shift lands on src2 where the backend expects it). */
    for (int attempt = 0; attempt < (commutative ? 2 : 1); attempt++) {
      IROperand src2 = tcc_ir_op_get_src1_or_2(ir, q, attempt == 0);
      if (!irop_has_vreg(src2))
        continue;

      /* The shifted value must be consumed as a value.  A dereferenced
       * operand is a memory read at the shift result's address; replacing it
       * with the shift source would silently delete that load.  The local
       * forms likewise carry an implicit memory access. */
      if (src2.is_lval || src2.is_local || src2.is_llocal)
        continue;

      int32_t vr2 = irop_get_vreg(src2);
      int shift_idx = ir_opt_du_def(&du, vr2, i);
      if (shift_idx < 0)
        continue;

      IRQuadCompact *sq = &ir->compact_instructions[shift_idx];
      int stype;
      switch (sq->op) {
      case TCCIR_OP_SHL:
        stype = 1; break;
      case TCCIR_OP_SHR: stype = 2; break;
      case TCCIR_OP_SAR: stype = 3; break;
      case TCCIR_OP_ROR: stype = 4; break;
      default: continue;
      }

      if (ir_opt_du_uses(&du, vr2) != 1 || !ir_opt_du_is_single_def(&du, vr2))
        continue;

      IROperand shift_dest = tcc_ir_op_get_dest(ir, sq);
      if (shift_dest.btype == IROP_BTYPE_INT64)
        continue;

      IROperand consumer_dest = tcc_ir_op_get_dest(ir, q);
      if (consumer_dest.btype == IROP_BTYPE_INT64)
        continue;
      if (src2.btype == IROP_BTYPE_INT64)
        continue;

      if (!tcc_ir_op_src2_is_imm(ir, sq))
        continue;

      int64_t amount = tcc_ir_op_src2_imm(ir, sq);
      if (amount < 0 || amount > 31)
        continue;

      /* A zero-amount right shift/rotate is an identity in the IR (x >> 0 == x),
       * but ARM's barrel shifter encodes an immediate field of 0 for LSR/ASR as
       * shift-by-32 (yielding 0 / sign-extend) and for ROR as RRX — NOT the
       * shift-by-0 we mean.  Only LSL #0 (stype 1) is a true no-op operand, so
       * refuse to fuse `x SHR/SAR/ROR #0`; leave the standalone shift for the
       * backend's shift-by-0 identity fold (arm-thumb-gen.c) to lower as MOV. */
      if (amount == 0 && stype != 1)
        continue;

      /* Single-use addresses belong to the addressing selector; shared ones need an ADD. */
      if (stype == 1 && q->op == TCCIR_OP_ADD) {
        IROperand ad = tcc_ir_op_get_dest(ir, q);
        int aidx = irop_has_vreg(ad) ? ir_opt_du_idx(&du, irop_get_vreg(ad)) : -1;
        int shared_result = irop_is_vreg_value(ad) && ir_opt_du_is_single_def(&du, irop_get_vreg(ad)) &&
                            ir_opt_du_uses(&du, irop_get_vreg(ad)) > 1;
        if (!shared_result && ((amount >= 1 && amount <= 3) || !deref_base || aidx < 0 || deref_base[aidx]))
          continue;
      }

      IROperand shift_src1 = tcc_ir_op_get_src1(ir, sq);
      if (!irop_is_vreg_value(shift_src1))
        continue;

      /* A local's home reads its value only when aliases and volatile accesses cannot change it. */
      IRLiveInterval *shift_live = tcc_ir_try_get_live_interval(ir, irop_get_vreg(shift_src1));
      if ((shift_src1.is_local && !shift_live) ||
          (shift_live && (shift_live->addrtaken || shift_live->is_volatile)) ||
          tcc_ir_access_is_volatile(ir, shift_src1))
        continue;

      int32_t shift_src_vr = irop_get_vreg(shift_src1);

      IROperand other = tcc_ir_op_get_src1_or_2(ir, q, attempt != 0);
      if (!irop_has_vreg(other))
        continue;

      int safe = !is_target[i];
      for (int j = shift_idx + 1; j < i && safe; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        TccIrOp bop = jq->op;
        if (is_target[j])
          safe = 0;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
          safe = 0;
        if (bop == TCCIR_OP_NOP)
          continue;
        if (irop_config[bop].has_dest)
        {
          if (tcc_ir_op_dest_has_vreg(ir, jq) && tcc_ir_op_dest_vreg(ir, jq) == shift_src_vr)
            safe = 0;
        }
      }
      if (!safe)
        continue;

      /* For the swap path: rewrite src1 to the non-shift operand so the
       * backend sees `op dest, other, shifted`. The shift's source vreg
       * goes into src2 in both paths. */
      if (attempt == 1)
        tcc_ir_set_src1(ir, i, other);
      tcc_ir_set_src2(ir, i, shift_src1);
      ir->barrel_shifts[q->orig_index] = (uint8_t)((stype << 5) | (int)amount);
      sq->op = TCCIR_OP_NOP;
      break;
    }
  }

  tcc_free(is_target);
  tcc_free(deref_base);
  tcc_free(du.def);
}
