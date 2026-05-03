/*
 *  TCC IR - SSA Target-Specific Optimization Generators (ARM Thumb-2)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "ssa_opt_arm.h"

/* ============================================================================
 * ssa_gen_arm_fuse_mul_add_to_mla
 *
 * Pattern: t1 = MUL(a, b); t2 = ADD(t1, c)  where t1 has single use
 * Result:  t2 = MLA(a, b, c); NOP the MUL
 *
 * ARM Thumb-2 MLA executes in 1 cycle vs MUL(1) + ADD(1) = 2 cycles.
 * ============================================================================ */

int ssa_gen_arm_fuse_mul_add_to_mla(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *mul_q = &ir->compact_instructions[instr_idx];

  IROperand mul_dest = tcc_ir_op_get_dest(ir, mul_q);
  int32_t mul_vr = irop_get_vreg(mul_dest);
  if (mul_vr < 0)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, mul_vr);
  if (!vi || vi->use_count != 1)
    return 0;

  IRSSAUse *use = &vi->uses[0];
  if (use->kind != SSA_USE_INSTR)
    return 0;

  int add_idx = use->idx;
  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  if (add_q->op != TCCIR_OP_ADD)
    return 0;

  /* 64-bit MLA not supported on Cortex-M */
  if (mul_dest.btype == IROP_BTYPE_INT64)
    return 0;

  /* Identify which ADD operand is the MUL result and which is the accumulator */
  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
  IROperand accum;

  if (irop_get_vreg(add_src1) == mul_vr)
    accum = add_src2;
  else if (irop_get_vreg(add_src2) == mul_vr)
    accum = add_src1;
  else
    return 0;

  /* Accumulator must be available before the MUL (if it's a vreg) */
  int32_t accum_vr = irop_get_vreg(accum);
  if (accum_vr >= 0) {
    IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, accum_vr);
    if (avi && avi->def_instr >= instr_idx)
      return 0;
  }

  /* Rewrite: MUL → MLA, ADD → NOP */
  IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);

  mul_q->op = TCCIR_OP_MLA;

  /* MLA dest = ADD's dest */
  int mul_dest_slot = mul_q->operand_base;
  int add_dest_slot = add_q->operand_base;
  if (mul_dest_slot >= 0 && mul_dest_slot < ir->iroperand_pool_count &&
      add_dest_slot >= 0 && add_dest_slot < ir->iroperand_pool_count) {
    ir->iroperand_pool[mul_dest_slot] = ir->iroperand_pool[add_dest_slot];
  }

  /* Store accumulator at operand_base + 3 */
  int accum_slot = mul_q->operand_base + 3;
  while (ir->iroperand_pool_count <= accum_slot)
    tcc_ir_pool_add(ir, IROP_NONE);
  if (accum_slot >= ir->iroperand_pool_capacity) {
    mul_q->op = TCCIR_OP_MUL;
    return 0;
  }
  ir->iroperand_pool[accum_slot] = accum;

  /* Transfer dest vreg info from ADD to MLA */
  int32_t add_dest_vr = irop_get_vreg(add_dest);
  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, add_dest_vr);
  if (dvi)
    dvi->def_instr = instr_idx;

  /* Clear MUL result use (it's consumed by the fusion) */
  vi->use_count = 0;
  vi->def_instr = -1;

  /* Add accum use for the MLA instruction */
  IRSSAVregInfo *avi2 = ssa_opt_vinfo(ctx, accum_vr);
  if (avi2)
    ssa_opt_add_use_instr(avi2, instr_idx);

  /* NOP the ADD (remove its operand uses first) */
  ssa_opt_nop_instr(ctx, add_idx);

  return 1;
}

/* ============================================================================
 * ssa_gen_arm_fuse_shl_add_to_load_indexed
 *
 * Pattern: t1 = SHL(idx, #scale); t2 = ADD(base, t1); t3 = LOAD(t2)
 *          where t1 and t2 are single-use
 * Result:  t3 = LOAD_INDEXED(base, idx, #scale); NOP SHL, ADD
 *
 * Maps directly to ARM Thumb-2: LDR Rd, [Rn, Rm, LSL #scale]
 * ============================================================================ */

int ssa_gen_arm_fuse_shl_add_to_load_indexed(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *shl_q = &ir->compact_instructions[instr_idx];

  /* SHL must have immediate scale */
  IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
  if (shl_src2.tag != IROP_TAG_IMM32)
    return 0;
  int32_t scale = (int32_t)irop_get_imm64_ex(ir, shl_src2);
  if (scale < 0 || scale > 3)
    return 0;

  IROperand shl_dest = tcc_ir_op_get_dest(ir, shl_q);
  int32_t shl_vr = irop_get_vreg(shl_dest);
  if (shl_vr < 0)
    return 0;

  IRSSAVregInfo *shl_vi = ssa_opt_vinfo(ctx, shl_vr);
  if (!shl_vi || shl_vi->use_count != 1)
    return 0;
  if (shl_vi->uses[0].kind != SSA_USE_INSTR)
    return 0;

  /* Find the ADD that uses the SHL result */
  int add_idx = shl_vi->uses[0].idx;
  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  if (add_q->op != TCCIR_OP_ADD)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
  IROperand base;

  if (irop_get_vreg(add_src1) == shl_vr)
    base = add_src2;
  else if (irop_get_vreg(add_src2) == shl_vr)
    base = add_src1;
  else
    return 0;

  IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
  int32_t add_vr = irop_get_vreg(add_dest);
  if (add_vr < 0)
    return 0;

  IRSSAVregInfo *add_vi = ssa_opt_vinfo(ctx, add_vr);
  if (!add_vi || add_vi->use_count != 1)
    return 0;
  if (add_vi->uses[0].kind != SSA_USE_INSTR)
    return 0;

  /* Find the LOAD that uses the ADD result */
  int load_idx = add_vi->uses[0].idx;
  IRQuadCompact *load_q = &ir->compact_instructions[load_idx];
  if (load_q->op != TCCIR_OP_LOAD)
    return 0;

  IROperand load_src = tcc_ir_op_get_src1(ir, load_q);
  if (irop_get_vreg(load_src) != add_vr)
    return 0;
  if (!load_src.is_lval)
    return 0;

  /* Rewrite LOAD → LOAD_INDEXED(base, index, scale) */
  IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
  IROperand load_dest = tcc_ir_op_get_dest(ir, load_q);

  load_q->op = TCCIR_OP_LOAD_INDEXED;

  /* Allocate NEW pool space for 4 operands (dest, base, index, scale).
   * The original LOAD only had 2 slots; reusing operand_base would overwrite
   * the next instruction's operands at pool[lb+2] and pool[lb+3]. */
  int lb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (lb + 3 >= ir->iroperand_pool_capacity) {
    load_q->op = TCCIR_OP_LOAD;
    return 0;
  }
  load_q->operand_base = lb;

  /* base: clear lval since LOAD_INDEXED handles the deref */
  base.is_lval = 0;

  ir->iroperand_pool[lb + 0] = load_dest;
  ir->iroperand_pool[lb + 1] = base;
  ir->iroperand_pool[lb + 2] = shl_src1;
  ir->iroperand_pool[lb + 3] = shl_src2;

  /* Update use-def chains */
  int32_t base_vr = irop_get_vreg(base);
  IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, base_vr);
  if (bvi)
    ssa_opt_add_use_instr(bvi, load_idx);

  int32_t idx_vr = irop_get_vreg(shl_src1);
  IRSSAVregInfo *ivi = ssa_opt_vinfo(ctx, idx_vr);
  if (ivi)
    ssa_opt_add_use_instr(ivi, load_idx);

  /* Clear intermediate vreg info */
  shl_vi->use_count = 0;
  shl_vi->def_instr = -1;
  add_vi->use_count = 0;
  add_vi->def_instr = -1;

  /* NOP SHL and ADD */
  ssa_opt_nop_instr(ctx, instr_idx);
  ssa_opt_nop_instr(ctx, add_idx);

  return 1;
}

/* ============================================================================
 * ssa_gen_arm_fuse_shl_add_to_store_indexed
 *
 * Pattern: t1 = SHL(idx, #scale); t2 = ADD(base, t1); STORE(t2, val)
 *          where t1 and t2 are single-use
 * Result:  STORE_INDEXED(base, val, idx, #scale); NOP SHL, ADD
 *
 * Maps to ARM Thumb-2: STR Rd, [Rn, Rm, LSL #scale]
 * ============================================================================ */

int ssa_gen_arm_fuse_shl_add_to_store_indexed(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *shl_q = &ir->compact_instructions[instr_idx];

  IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
  if (shl_src2.tag != IROP_TAG_IMM32)
    return 0;
  int32_t scale = (int32_t)irop_get_imm64_ex(ir, shl_src2);
  if (scale < 0 || scale > 3)
    return 0;

  IROperand shl_dest = tcc_ir_op_get_dest(ir, shl_q);
  int32_t shl_vr = irop_get_vreg(shl_dest);
  if (shl_vr < 0)
    return 0;

  IRSSAVregInfo *shl_vi = ssa_opt_vinfo(ctx, shl_vr);
  if (!shl_vi || shl_vi->use_count != 1)
    return 0;
  if (shl_vi->uses[0].kind != SSA_USE_INSTR)
    return 0;

  int add_idx = shl_vi->uses[0].idx;
  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  if (add_q->op != TCCIR_OP_ADD)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
  IROperand base;

  if (irop_get_vreg(add_src1) == shl_vr)
    base = add_src2;
  else if (irop_get_vreg(add_src2) == shl_vr)
    base = add_src1;
  else
    return 0;

  IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
  int32_t add_vr = irop_get_vreg(add_dest);
  if (add_vr < 0)
    return 0;

  IRSSAVregInfo *add_vi = ssa_opt_vinfo(ctx, add_vr);
  if (!add_vi || add_vi->use_count != 1)
    return 0;
  if (add_vi->uses[0].kind != SSA_USE_INSTR)
    return 0;

  int store_idx = add_vi->uses[0].idx;
  IRQuadCompact *store_q = &ir->compact_instructions[store_idx];
  if (store_q->op != TCCIR_OP_STORE)
    return 0;

  IROperand store_dest = tcc_ir_op_get_dest(ir, store_q);
  if (irop_get_vreg(store_dest) != add_vr)
    return 0;

  /* Rewrite STORE → STORE_INDEXED(base, src, index, scale) */
  IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
  IROperand store_src = tcc_ir_op_get_src1(ir, store_q);

  store_q->op = TCCIR_OP_STORE_INDEXED;

  /* Allocate NEW pool space for 4 operands (base, value, index, scale).
   * The original STORE only had 2 slots; reusing operand_base would overwrite
   * the next instruction's operands at pool[sb+2] and pool[sb+3]. */
  int sb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (sb + 3 >= ir->iroperand_pool_capacity) {
    store_q->op = TCCIR_OP_STORE;
    return 0;
  }
  store_q->operand_base = sb;

  base.is_lval = 0;

  ir->iroperand_pool[sb + 0] = base;
  ir->iroperand_pool[sb + 1] = store_src;
  ir->iroperand_pool[sb + 2] = shl_src1;
  ir->iroperand_pool[sb + 3] = shl_src2;

  /* Update use-def chains */
  int32_t base_vr = irop_get_vreg(base);
  IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, base_vr);
  if (bvi)
    ssa_opt_add_use_instr(bvi, store_idx);

  int32_t idx_vr = irop_get_vreg(shl_src1);
  IRSSAVregInfo *ivi = ssa_opt_vinfo(ctx, idx_vr);
  if (ivi)
    ssa_opt_add_use_instr(ivi, store_idx);

  shl_vi->use_count = 0;
  shl_vi->def_instr = -1;
  add_vi->use_count = 0;
  add_vi->def_instr = -1;

  ssa_opt_nop_instr(ctx, instr_idx);
  ssa_opt_nop_instr(ctx, add_idx);

  return 1;
}

/* ============================================================================
 * ssa_gen_arm_reduce_mul_to_shift
 *
 * Pattern: dest = MUL(src, #pow2)  or MUL(#pow2, src)
 * Result:  dest = SHL(src, #log2(pow2))
 *
 * SHL is 1-cycle single-issue vs MUL which uses the multiplier pipeline.
 * ============================================================================ */

int ssa_gen_arm_reduce_mul_to_shift(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand imm_op, var_op;

  if (src2.tag == IROP_TAG_IMM32) {
    imm_op = src2;
    var_op = src1;
  } else if (src1.tag == IROP_TAG_IMM32) {
    imm_op = src1;
    var_op = src2;
  } else {
    return 0;
  }

  int64_t val = irop_get_imm64_ex(ir, imm_op);
  if (val <= 0 || (val & (val - 1)) != 0)
    return 0;

  int shift = 0;
  int64_t v = val;
  while (v > 1) { shift++; v >>= 1; }

  q->op = TCCIR_OP_SHL;
  imm_op.u.imm32 = shift;
  tcc_ir_op_set_src1(ir, q, var_op);
  tcc_ir_op_set_src2(ir, q, imm_op);

  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen ssa_gen_arm[] = {
  { TCCIR_OP_MUL, ssa_gen_arm_fuse_mul_add_to_mla,            "arm_mla_fusion" },
  { TCCIR_OP_MUL, ssa_gen_arm_reduce_mul_to_shift,            "arm_mul_to_shl" },
  { TCCIR_OP_SHL, ssa_gen_arm_fuse_shl_add_to_load_indexed,   "arm_load_indexed" },
  { TCCIR_OP_SHL, ssa_gen_arm_fuse_shl_add_to_store_indexed,  "arm_store_indexed" },
};

void tcc_ir_ssa_opt_arm_register(void)
{
  tcc_ir_ssa_opt_register_target(ssa_gen_arm,
                                 sizeof(ssa_gen_arm) / sizeof(ssa_gen_arm[0]));
}
