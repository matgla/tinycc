/*
 *  TCC IR - Fold a chained ADD constant into an indexed load/store displacement (pre-SSA engine)
 *
 *  LOAD/STORE_INDEXED[base, #imm2] whose single-use base resolves to `new_base ± #imm1`
 *  collapses to LOAD/STORE_INDEXED[new_base, #(imm1+imm2)], NOPing the ADD/SUB, provided the
 *  merged displacement stays inside the addressing-mode immediate range and the base is not
 *  a local/lval whose folded form would change addressing semantics.
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
#include "opt/flat/indexed_chain.h"

OPT_GEN_FLAT(indexed_chain, TCCIR_OP_LOAD_INDEXED)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;
  IRQuadCompact *q = &ir->compact_instructions[i];

  int is_store = (q->op == TCCIR_OP_STORE_INDEXED);
  int base_slot = is_store ? 0 : 1;
  IROperand base_op = ir->iroperand_pool[q->operand_base + base_slot];
  IROperand index_op = ir->iroperand_pool[q->operand_base + 2];
  IROperand scale_op = ir->iroperand_pool[q->operand_base + 3];

  if (irop_get_tag(scale_op) != IROP_TAG_IMM32 || scale_op.u.imm32 != 0)
    return 0;
  if (irop_get_tag(index_op) != IROP_TAG_IMM32)
    return 0;
  int imm2 = (int)index_op.u.imm32;

  int32_t base_vr = irop_get_vreg(base_op);
  if (base_vr < 0)
    return 0;
  if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
    return 0;

  int add_idx = ir_opt_du_def(du, base_vr, i);
  if (add_idx < 0)
    return 0;
  if (ir_opt_du_uses(du, base_vr) != 1)
    return 0;

  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  int is_sub = (add_q->op == TCCIR_OP_SUB);
  if (add_q->op != TCCIR_OP_ADD && !is_sub)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

  IROperand new_base;
  int imm1;
  if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG && irop_has_vreg(add_src1)) {
    new_base = add_src1; imm1 = is_sub ? -(int)add_src2.u.imm32 : (int)add_src2.u.imm32;
  } else if (!is_sub && irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG && irop_has_vreg(add_src2)) {
    new_base = add_src2; imm1 = (int)add_src1.u.imm32;
  } else {
    return 0;
  }

  if (new_base.is_local || new_base.is_llocal)
    return 0;

  long long imm_total = (long long)imm1 + imm2;
  if (imm_total > 4095 || imm_total < -255)
    return 0;

  if (!ir_xform_same_block(ir, add_idx, i))
    return 0;

  new_base.is_lval = 0;
  new_base.btype = base_op.btype;
  /* The chained base addresses the same object: keep the packed-access mark
   * so the 64-bit lowering stays off LDRD/STRD. */
  new_base.aux |= base_op.aux & IROP_AUX_UNDERALIGN;
  ir->iroperand_pool[q->operand_base + base_slot] = new_base;
  ir->iroperand_pool[q->operand_base + 2] = irop_make_imm32(0, (int32_t)imm_total, IROP_BTYPE_INT32);

  add_q->op = TCCIR_OP_NOP;
  return 1;
}

const IROptGen fusion_chain_gens[] = {
    {TCCIR_OP_LOAD_INDEXED, opt_dsl_dispatch_indexed_chain_flat, "indexed_chain_load", 1},
    {TCCIR_OP_STORE_INDEXED, opt_dsl_dispatch_indexed_chain_flat, "indexed_chain_store", 1},
};

const int fusion_chain_gens_count = sizeof(fusion_chain_gens) / sizeof(fusion_chain_gens[0]);

int tcc_ir_opt_gens_chain_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_chain_gens, fusion_chain_gens_count);
}
