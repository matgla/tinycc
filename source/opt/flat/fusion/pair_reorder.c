/*
 *  TCC IR - Reorder adjacent word-strided indexed load/store pairs into program order (pre-SSA engine)
 *
 *  Two LOAD/STORE_INDEXED to the same base with immediate indices 4 bytes apart, separated only by
 *  reorder-safe instructions, are pulled adjacent so the backend can emit an LDRD/STRD pair. The
 *  later access bubbles up past intervening ops as long as none consume its destination value.
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
#include "opt_xform.h"
#include "opt_dsl.h"
#include "opt/flat/pair_reorder.h"

OPT_GEN_FLAT(pair_reorder, TCCIR_OP_LOAD_INDEXED)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  IRQuadCompact *q1 = &ir->compact_instructions[i];

  if (i + 2 >= n)
    return 0;

  int q1_is_load = (q1->op == TCCIR_OP_LOAD_INDEXED);

  IROperand q1_scale = ir->iroperand_pool[q1->operand_base + 3];
  IROperand q1_index = ir->iroperand_pool[q1->operand_base + 2];
  if (irop_get_tag(q1_scale) != IROP_TAG_IMM32 || q1_scale.u.imm32 != 0)
    return 0;
  if (irop_get_tag(q1_index) != IROP_TAG_IMM32)
    return 0;

  int q1_base_slot = q1_is_load ? 1 : 0;
  IROperand q1_base = ir->iroperand_pool[q1->operand_base + q1_base_slot];
  int32_t q1_base_vr = irop_get_vreg(q1_base);
  if (q1_base_vr < 0)
    return 0;

  const int window = 12;
  int q3_idx = -1;
  int blocked = 0;
  for (int k = i + 1; k < n && (k - i) <= window; k++) {
    IRQuadCompact *cq = &ir->compact_instructions[k];
    if (cq->op == TCCIR_OP_NOP)
      continue;
    if (cq->is_jump_target) { blocked = 1; break; }
    if (cq->op == q1->op) { q3_idx = k; break; }
    int safe = 0;
    if (cq->op == TCCIR_OP_FUNCPARAMVAL) {
      safe = 1;
    } else if (cq->op == TCCIR_OP_ASSIGN) {
      IROperand a_dest = tcc_ir_op_get_dest(ir, cq);
      IROperand a_src1 = tcc_ir_op_get_src1(ir, cq);
      if (!a_dest.is_lval && !a_src1.is_lval && irop_get_vreg(a_dest) != q1_base_vr)
        safe = 1;
    }
    if (!safe) { blocked = 1; break; }
  }
  if (q3_idx < 0 || blocked)
    return 0;

  IRQuadCompact *q3 = &ir->compact_instructions[q3_idx];

  IROperand q3_scale = ir->iroperand_pool[q3->operand_base + 3];
  IROperand q3_index = ir->iroperand_pool[q3->operand_base + 2];
  if (irop_get_tag(q3_scale) != IROP_TAG_IMM32 || q3_scale.u.imm32 != 0)
    return 0;
  if (irop_get_tag(q3_index) != IROP_TAG_IMM32)
    return 0;

  int q3_base_slot = q1_is_load ? 1 : 0;
  IROperand q3_base = ir->iroperand_pool[q3->operand_base + q3_base_slot];
  if (irop_get_vreg(q3_base) != q1_base_vr)
    return 0;

  int32_t imm1 = q1_index.u.imm32;
  int32_t imm2 = q3_index.u.imm32;
  if (imm1 + 4 != imm2 && imm2 + 4 != imm1)
    return 0;
  if ((imm1 & 3) != 0 || (imm2 & 3) != 0)
    return 0;

  IROperand q3_dv = q1_is_load ? ir->iroperand_pool[q3->operand_base + 0]
                               : ir->iroperand_pool[q3->operand_base + 1];
  int32_t q3_dv_vr = irop_get_vreg(q3_dv);

  int swap_pos = q3_idx;
  int target_pos = i + 1;
  while (swap_pos > target_pos) {
    int prev = swap_pos - 1;
    while (prev > i && ir->compact_instructions[prev].op == TCCIR_OP_NOP)
      prev--;
    if (prev <= i)
      break;
    IRQuadCompact *pq = &ir->compact_instructions[prev];
    int conflict = 0;
    if (q3_dv_vr >= 0) {
      if (q1_is_load) {
        if (irop_config[pq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, pq)) == q3_dv_vr)
          conflict = 1;
        if (!conflict && irop_config[pq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, pq)) == q3_dv_vr)
          conflict = 1;
      } else if (irop_config[pq->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, pq)) == q3_dv_vr) {
        conflict = 1;
      }
    }
    if (conflict)
      break;
    IRQuadCompact tmp = *pq;
    *pq = ir->compact_instructions[swap_pos];
    ir->compact_instructions[swap_pos] = tmp;
    swap_pos = prev;
  }

  return (swap_pos < q3_idx) ? 1 : 0;
}

const IROptGen fusion_pair_reorder_gens[] = {
    {TCCIR_OP_LOAD_INDEXED, opt_dsl_dispatch_pair_reorder_flat, "pair_reorder_load", 0},
    {TCCIR_OP_STORE_INDEXED, opt_dsl_dispatch_pair_reorder_flat, "pair_reorder_store", 0},
};

const int fusion_pair_reorder_gens_count = sizeof(fusion_pair_reorder_gens) / sizeof(fusion_pair_reorder_gens[0]);

int tcc_ir_opt_gens_pair_reorder_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_pair_reorder_gens, fusion_pair_reorder_gens_count);
}
