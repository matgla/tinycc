/*
 *  TCC IR - Fusion generator table (pre-SSA engine)
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
#include "opt_gens_fusion.h"

static int ir_gen_rotate_fusion(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;
  IRQuadCompact *q = &ir->compact_instructions[i];

  IROperand or_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand or_src2 = tcc_ir_op_get_src2(ir, q);

  if (!irop_has_vreg(or_src1) || !irop_has_vreg(or_src2))
    return 0;

  int32_t vr1 = irop_get_vreg(or_src1);
  int32_t vr2 = irop_get_vreg(or_src2);

  int idx1 = ir_opt_du_def(du, vr1, i);
  int idx2 = ir_opt_du_def(du, vr2, i);
  if (idx1 < 0 || idx2 < 0)
    return 0;

  IRQuadCompact *q1 = &ir->compact_instructions[idx1];
  IRQuadCompact *q2 = &ir->compact_instructions[idx2];

  IRQuadCompact *shl_q, *shr_q;
  int shl_idx, shr_idx;
  int32_t shl_vr, shr_vr;

  if (q1->op == TCCIR_OP_SHL && q2->op == TCCIR_OP_SHR) {
    shl_q = q1; shr_q = q2;
    shl_idx = idx1; shr_idx = idx2;
    shl_vr = vr1; shr_vr = vr2;
  } else if (q1->op == TCCIR_OP_SHR && q2->op == TCCIR_OP_SHL) {
    shr_q = q1; shl_q = q2;
    shr_idx = idx1; shl_idx = idx2;
    shr_vr = vr1; shl_vr = vr2;
  } else {
    return 0;
  }

  if (ir_opt_du_uses(du, shl_vr) != 1 || ir_opt_du_uses(du, shr_vr) != 1)
    return 0;

  IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
  IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
  IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
  IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);

  if (!irop_is_immediate(shl_src2) || !irop_is_immediate(shr_src2))
    return 0;

  int64_t shl_amt = irop_get_imm64_ex(ir, shl_src2);
  int64_t shr_amt = irop_get_imm64_ex(ir, shr_src2);

  if (shl_amt <= 0 || shl_amt >= 32 || shr_amt <= 0 || shr_amt >= 32)
    return 0;
  if (shl_amt + shr_amt != 32)
    return 0;

  if (!irop_has_vreg(shl_src1) || !irop_has_vreg(shr_src1))
    return 0;
  if (irop_get_vreg(shl_src1) != irop_get_vreg(shr_src1))
    return 0;

  int min_idx = shl_idx < shr_idx ? shl_idx : shr_idx;
  if (!ir_xform_same_block(ir, min_idx, i))
    return 0;

  IROperand or_dest = tcc_ir_op_get_dest(ir, q);
  IROperand ror_imm = irop_make_imm32(0, (int32_t)shr_amt, shr_src2.btype);

  q->op = TCCIR_OP_ROR;
  tcc_ir_set_dest(ir, i, or_dest);
  tcc_ir_set_src1(ir, i, shr_src1);
  tcc_ir_set_src2(ir, i, ror_imm);

  shl_q->op = TCCIR_OP_NOP;
  shr_q->op = TCCIR_OP_NOP;

  LOG_IR_GEN("OPTIMIZE: Rotate fusion SHL(%lld)+SHR(%lld)+OR → ROR(%lld) at i=%d",
             (long long)shl_amt, (long long)shr_amt, (long long)shr_amt, i);
  return 1;
}

static int ir_gen_mla_fusion(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (!tcc_state->opt_mla_fusion)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[i];

  IROperand add_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, q);
  IROperand add_dest = tcc_ir_op_get_dest(ir, q);

  int32_t mul_result_vr = -1;
  IROperand accum_op;
  int mul_idx = -1;
  IRQuadCompact *mul_q = NULL;

  if (irop_has_vreg(add_src2)) {
    int32_t vr = irop_get_vreg(add_src2);
    int idx = ir_opt_du_def(du, vr, i);
    if (idx >= 0 && ir->compact_instructions[idx].op == TCCIR_OP_MUL) {
      mul_result_vr = vr;
      accum_op = add_src1;
      mul_idx = idx;
      mul_q = &ir->compact_instructions[mul_idx];
    }
  }
  if (!mul_q && irop_has_vreg(add_src1)) {
    int32_t vr = irop_get_vreg(add_src1);
    int idx = ir_opt_du_def(du, vr, i);
    if (idx >= 0 && ir->compact_instructions[idx].op == TCCIR_OP_MUL) {
      mul_result_vr = vr;
      accum_op = add_src2;
      mul_idx = idx;
      mul_q = &ir->compact_instructions[mul_idx];
    }
  }

  if (!mul_q)
    return 0;

  if (irop_get_tag(accum_op) == IROP_TAG_SYMREF || irop_get_tag(add_dest) == IROP_TAG_SYMREF ||
      irop_get_tag(add_src1) == IROP_TAG_SYMREF || irop_get_tag(add_src2) == IROP_TAG_SYMREF)
    return 0;
  if (irop_get_tag(accum_op) == IROP_TAG_STACKOFF && !accum_op.is_lval)
    return 0;

  IROperand ms1 = tcc_ir_op_get_src1(ir, mul_q);
  IROperand ms2 = tcc_ir_op_get_src2(ir, mul_q);

  int dup_mul = 0;
  if (!ms1.is_lval && !ms2.is_lval && !irop_is_immediate(ms1) && !irop_is_immediate(ms2)) {
    int32_t ms1_vr = irop_get_vreg(ms1);
    int32_t ms2_vr = irop_get_vreg(ms2);
    if (ms1_vr >= 0 && ms2_vr >= 0) {
      int n = ir->next_instruction_index;
      for (int k = 0; k < n && !dup_mul; k++) {
        if (k == mul_idx)
          continue;
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op != TCCIR_OP_MUL)
          continue;
        IROperand ks1 = tcc_ir_op_get_src1(ir, kq);
        IROperand ks2 = tcc_ir_op_get_src2(ir, kq);
        if (((irop_get_vreg(ks1) == ms1_vr && irop_get_vreg(ks2) == ms2_vr) ||
             (irop_get_vreg(ks1) == ms2_vr && irop_get_vreg(ks2) == ms1_vr)) &&
            !ks1.is_lval && !ks2.is_lval)
          dup_mul = 1;
      }
    }
  }

  if ((ms1.is_lval && !ms1.is_local && !ms1.is_llocal) || (ms2.is_lval && !ms2.is_local && !ms2.is_llocal) ||
      irop_is_immediate(ms1) || irop_is_immediate(ms2) || dup_mul ||
      ir_opt_du_uses(du, mul_result_vr) != 1)
    return 0;

  if (!ir_xform_same_block(ir, mul_idx, i))
    return 0;

  int32_t accum_vr = irop_get_vreg(accum_op);
  if (accum_vr >= 0) {
    int adef = ir_opt_du_def(du, accum_vr, i);
    if (adef >= 0 && adef >= mul_idx)
      return 0;
  }

  mul_q->op = TCCIR_OP_MLA;
  int mul_dest_idx = mul_q->operand_base;
  int add_dest_idx = q->operand_base;
  if (mul_dest_idx >= 0 && mul_dest_idx < ir->iroperand_pool_count && add_dest_idx >= 0 &&
      add_dest_idx < ir->iroperand_pool_count)
    ir->iroperand_pool[mul_dest_idx] = ir->iroperand_pool[add_dest_idx];

  int accum_idx = mul_q->operand_base + 3;
  while (ir->iroperand_pool_count <= accum_idx)
    tcc_ir_pool_add(ir, IROP_NONE);
  if (accum_idx < ir->iroperand_pool_capacity) {
    ir->iroperand_pool[accum_idx] = accum_op;
    q->op = TCCIR_OP_NOP;
    return 1;
  }

  mul_q->op = TCCIR_OP_MUL;
  return 0;
}

static int ir_gen_indexed_memory_fusion(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (!tcc_state->opt_indexed_memory)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[i];

  int is_store = (q->op == TCCIR_OP_STORE);
  IROperand addr_op = is_store ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);

  if (!irop_has_vreg(addr_op))
    return 0;

  int32_t addr_vr = irop_get_vreg(addr_op);
  if (!is_store && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
    return 0;

  int add_idx = ir_opt_du_def(du, addr_vr, i);
  if (add_idx < 0)
    return 0;

  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  if (add_q->op != TCCIR_OP_ADD)
    return 0;

  if (ir_opt_du_uses(du, addr_vr) != 1)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
  int32_t offset_vr = -1;
  IROperand base_op = IROP_NONE;
  int shl_idx = -1;
  IRQuadCompact *shl_q = NULL;

  if (irop_has_vreg(add_src1)) {
    int32_t vr1 = irop_get_vreg(add_src1);
    int idx1 = ir_opt_du_def(du, vr1, add_idx);
    if (idx1 >= 0 && ir->compact_instructions[idx1].op == TCCIR_OP_SHL) {
      offset_vr = vr1;
      base_op = add_src2;
      shl_idx = idx1;
      shl_q = &ir->compact_instructions[shl_idx];
    }
  }
  if (shl_idx < 0 && irop_has_vreg(add_src2)) {
    int32_t vr2 = irop_get_vreg(add_src2);
    int idx2 = ir_opt_du_def(du, vr2, add_idx);
    if (idx2 >= 0 && ir->compact_instructions[idx2].op == TCCIR_OP_SHL) {
      offset_vr = vr2;
      base_op = add_src1;
      shl_idx = idx2;
      shl_q = &ir->compact_instructions[shl_idx];
    }
  }
  if (shl_idx < 0)
    return 0;

  if (ir_opt_du_uses(du, offset_vr) != 1)
    return 0;

  IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
  if (!shl_src2.is_const)
    return 0;

  int shift_amount = shl_src2.u.imm32;
  if (shift_amount < 1 || shift_amount > 3)
    return 0;

  IROperand index_op = tcc_ir_op_get_src1(ir, shl_q);
  if (index_op.is_local || index_op.is_llocal)
    return 0;
  if (base_op.is_llocal || base_op.is_lval)
    return 0;

  for (int j = shl_idx + 1; j < i; j++) {
    TccIrOp bop = ir->compact_instructions[j].op;
    if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF || bop == TCCIR_OP_NOP)
      return 0;
  }

  IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
  IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);

  q->op = is_store ? TCCIR_OP_STORE_INDEXED : TCCIR_OP_LOAD_INDEXED;

  int new_base_idx = ir->iroperand_pool_count;
  if (new_base_idx + 4 > ir->iroperand_pool_capacity) {
    q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD;
    return 0;
  }

  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  q->operand_base = new_base_idx;

  IROperand base_op_clean = base_op;
  IROperand index_op_clean = index_op;
  base_op_clean.is_lval = 0;
  IROperand scale_imm = irop_make_imm32(0, shift_amount, IROP_BTYPE_INT32);

  if (is_store) {
    ir->iroperand_pool[new_base_idx + 0] = base_op_clean;
    ir->iroperand_pool[new_base_idx + 1] = orig_src1;
    ir->iroperand_pool[new_base_idx + 2] = index_op_clean;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
  } else {
    ir->iroperand_pool[new_base_idx + 0] = orig_dest;
    ir->iroperand_pool[new_base_idx + 1] = base_op_clean;
    ir->iroperand_pool[new_base_idx + 2] = index_op_clean;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
  }

  shl_q->op = TCCIR_OP_NOP;
  add_q->op = TCCIR_OP_NOP;
  return 1;
}

const IROptGen fusion_gens[] = {
    {TCCIR_OP_OR, ir_gen_rotate_fusion, "rotate_fusion", 1},
    {TCCIR_OP_ADD, ir_gen_mla_fusion, "mla_fusion", 1},
    {TCCIR_OP_LOAD, ir_gen_indexed_memory_fusion, "indexed_load_fusion", 1},
    {TCCIR_OP_STORE, ir_gen_indexed_memory_fusion, "indexed_store_fusion", 1},
};

const int fusion_gens_count = sizeof(fusion_gens) / sizeof(fusion_gens[0]);
