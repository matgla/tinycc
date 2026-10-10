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
#include "opt_xform.h"
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

  /* If the accumulator is defined by a SHL/SHR, the ARM backend can fold the
   * shift into the ADD's barrel-shifter operand (e.g. `add Rd, Rn, Rm, lsl
   * #N`). MLA has no barrel-shifter on its accumulator, so fusing would
   * defeat that lowering and produce wrong results for bitfield arithmetic
   * (test gcc.c-torture/execute/20000113-1). Keep the MUL/ADD form so the
   * backend can pick the better encoding. */
  int32_t accum_vr_chk = irop_get_vreg(accum);
  if (accum_vr_chk >= 0) {
    IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, accum_vr_chk);
    if (avi && avi->def_instr >= 0) {
      int def_op = ir->compact_instructions[avi->def_instr].op;
      if (def_op == TCCIR_OP_SHL || def_op == TCCIR_OP_SAR ||
          def_op == TCCIR_OP_SHR)
        return 0;
    }
  }

  /* Skip if barrel-shift fusion already absorbed a shift into the ADD's
   * src2 operand: that op was rewritten to consume the SHR's input vreg
   * with the shift kind/amount recorded in ir->barrel_shifts[].  The
   * original SHR def is now a NOP, so the def_op check above doesn't
   * fire — without this guard the MLA fusion would drop the shift. */
  if (ir->barrel_shifts && add_q->orig_index >= 0 &&
      add_q->orig_index < ir->barrel_shifts_len &&
      ir->barrel_shifts[add_q->orig_index] != 0)
    return 0;

  /* Place the MLA at the ADD's position. By SSA dominance, MUL's inputs and
   * the accumulator are all defined before the ADD, so this is always valid
   * for register operands.  Placing the MLA at the MUL's position would
   * require the accumulator to dominate the MUL — that's the rarer case. */
  IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
  IROperand mul_src1 = tcc_ir_op_get_src1(ir, mul_q);
  IROperand mul_src2 = tcc_ir_op_get_src2(ir, mul_q);

  /* A MUL source that reads memory would be re-read at the ADD's site;
   * any store to that location in between changes the loaded value
   * (volatile fuzz seed 5053: `vv11 = st.f0 * u5` before a loop that
   * updates st.f0, product consumed after the loop). */
  if ((ir_xform_operand_reads_memory(mul_src1) || ir_xform_operand_reads_memory(mul_src2)) &&
      (add_q->is_jump_target || !ir_xform_range_preserves_memory(ir, instr_idx, add_idx)))
    return 0;

  /* Allocate fresh pool space for the MLA's 4 operands (dest, src1, src2,
   * accum). Reusing the ADD's operand_base would clobber the next
   * instruction's operands at base+2 and base+3. */
  int nb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (nb + 3 >= ir->iroperand_pool_capacity)
    return 0;

  add_q->op = TCCIR_OP_MLA;
  add_q->operand_base = nb;
  ir->iroperand_pool[nb + 0] = add_dest;
  ir->iroperand_pool[nb + 1] = mul_src1;
  ir->iroperand_pool[nb + 2] = mul_src2;
  ir->iroperand_pool[nb + 3] = accum;

  /* MUL's result is no longer used; NOP it. */
  ssa_opt_nop_instr(ctx, instr_idx);

  /* The ADD already had uses recorded for mul_vr and accum_vr at add_idx.
   * After the rewrite, the MLA at add_idx uses mul_src1, mul_src2, accum.
   * Add uses for mul_src1/mul_src2 (previously they were used by the now-
   * NOP'd MUL only), and remove the dead use of mul_vr. */
  IRSSAVregInfo *s1vi = ssa_opt_vinfo(ctx, irop_get_vreg(mul_src1));
  if (s1vi)
    ssa_opt_add_use_instr(s1vi, add_idx);
  IRSSAVregInfo *s2vi = ssa_opt_vinfo(ctx, irop_get_vreg(mul_src2));
  if (s2vi)
    ssa_opt_add_use_instr(s2vi, add_idx);

  IRSSAVregInfo *mvi = ssa_opt_vinfo(ctx, mul_vr);
  if (mvi) {
    ssa_opt_remove_use_instr(mvi, add_idx);
    mvi->def_instr = -1;
  }

  return 1;
}

/* ============================================================================
 * Operand stability for the SHL+ADD -> LOAD/STORE_INDEXED fusions
 *
 * The fused access reads idx and base at the memory op instead of at the SHL /
 * ADD.  That is only the same value when neither was redefined in between.  The
 * SSA builder does not rename everything: a reassigned PARAM (`P1 <-- P2
 * [STORE]`, the entry value being its first def) or a register-promoted local's
 * in-place `T <-- x [STORE]` is a second def under the same name.
 * ============================================================================ */

/* Is `op` read-stable anywhere: no vreg, or exactly one definition in total. */
static int arm_operand_single_def(IRSSAOptCtx *ctx, IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 1;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi)
    return 0;
  int total = ssa_opt_def_total(vi);
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM)
    total++;                 /* the value it enters the function with */
  return total <= 1;
}

/* Does the instruction at idx write the vreg `vr` (a deref dest writes through it, not to it)? */
static int arm_instr_defines_vreg(TCCIRState *ir, int idx, int32_t vr)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (!irop_config[q->op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  return !d.is_lval && irop_get_vreg(d) == vr;
}

/* May the memory op at mem_idx read `idx` and `base` instead of the SHL at shl_idx / the ADD
 * (which read them there)?  Single-def operands always can.  A redefinable one needs the three
 * instructions in one block and no def of it in between. */
static int arm_indexed_operands_hold(IRSSAOptCtx *ctx, int shl_idx, int mem_idx, IROperand idx,
                                     IROperand base)
{
  if (arm_operand_single_def(ctx, idx) && arm_operand_single_def(ctx, base))
    return 1;
  TCCIRState *ir = ctx->ir;
  if (!ctx->cfg || shl_idx >= mem_idx || ctx->cfg->instr_to_block[shl_idx] != ctx->cfg->instr_to_block[mem_idx])
    return 0;
  int32_t ivr = irop_get_vreg(idx), bvr = irop_get_vreg(base);
  for (int i = shl_idx + 1; i < mem_idx; i++)
  {
    if ((ivr >= 0 && arm_instr_defines_vreg(ir, i, ivr)) || (bvr >= 0 && arm_instr_defines_vreg(ir, i, bvr)))
      return 0;
  }
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

  /* Bail if base would require its own deref (e.g. stack-spilled VLA
   * pointer represented as StackLoc[N] with is_lval=1). LOAD_INDEXED
   * treats its base as a single address, not as an lvalue to be loaded. */
  if (base.is_lval)
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

  /* Find the LOAD that uses the ADD result.  A LOAD_INDEXED whose offset is
   * #0 (mem_inline's [addr, #0] piece of an align(1) memcpy) is the fused
   * form with a zero displacement, so it fuses like a plain LOAD does; its
   * src1 is already a plain address operand, not an lvalue. */
  int load_idx = add_vi->uses[0].idx;
  IRQuadCompact *load_q = &ir->compact_instructions[load_idx];
  const TccIrOp load_op = load_q->op;
  if (load_op == TCCIR_OP_LOAD_INDEXED) {
    IROperand load_off = tcc_ir_op_get_src2(ir, load_q);
    if (load_off.tag != IROP_TAG_IMM32 || irop_get_imm64_ex(ir, load_off) != 0)
      return 0;
  } else if (load_op != TCCIR_OP_LOAD) {
    return 0;
  }

  IROperand load_src = tcc_ir_op_get_src1(ir, load_q);
  if (irop_get_vreg(load_src) != add_vr)
    return 0;
  if (load_op == TCCIR_OP_LOAD && !load_src.is_lval)
    return 0;

  IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
  if (!arm_indexed_operands_hold(ctx, instr_idx, load_idx, shl_src1, base))
    return 0;

  /* Rewrite LOAD → LOAD_INDEXED(base, index, scale); the width moves from
   * the deref onto the dest.  A LOAD_INDEXED #0 input already carries the
   * access width on its dest and a plain address in src1, so nothing moves. */
  IROperand load_dest = tcc_ir_op_get_dest(ir, load_q);
  if (load_op == TCCIR_OP_LOAD && !irop_indexed_load_dest(&load_dest, load_src))
    return 0;

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
    load_q->op = load_op;
    return 0;
  }
  load_q->operand_base = lb;

  /* base: clear lval since LOAD_INDEXED handles the deref, but carry the
   * replaced access's access marks (alignment, volatility) onto it: the plain
   * LOAD carries them on its deref src, the LOAD_INDEXED #0 on its dest. */
  base.is_lval = 0;
  irop_carry_access_marks(&base, load_op == TCCIR_OP_LOAD ? load_src : load_dest);

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

  /* Bail if base would require its own deref (e.g. stack-spilled VLA
   * pointer represented as StackLoc[N] with is_lval=1). STORE_INDEXED
   * treats its base as a single address, not as an lvalue to be loaded. */
  if (base.is_lval)
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

  /* A STORE_INDEXED whose offset is #0 (mem_inline's [addr, #0] piece of an
   * align(1) memcpy) is the fused form with a zero displacement, so it fuses
   * like a plain STORE does; its dest already holds a plain address operand. */
  int store_idx = add_vi->uses[0].idx;
  IRQuadCompact *store_q = &ir->compact_instructions[store_idx];
  const TccIrOp store_op = store_q->op;
  if (store_op == TCCIR_OP_STORE_INDEXED) {
    IROperand store_off = tcc_ir_op_get_src2(ir, store_q);
    if (store_off.tag != IROP_TAG_IMM32 || irop_get_imm64_ex(ir, store_off) != 0)
      return 0;
  } else if (store_op != TCCIR_OP_STORE) {
    return 0;
  }

  IROperand store_dest = tcc_ir_op_get_dest(ir, store_q);
  if (irop_get_vreg(store_dest) != add_vr)
    return 0;

  IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
  if (!arm_indexed_operands_hold(ctx, instr_idx, store_idx, shl_src1, base))
    return 0;

  /* Rewrite STORE → STORE_INDEXED(base, src, index, scale) */
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
    store_q->op = store_op;
    return 0;
  }
  store_q->operand_base = sb;

  base.is_lval = 0;
  irop_carry_access_marks(&base, store_dest);

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
 * ssa_gen_arm_fuse_load_through_add_imm
 *
 * Pattern: t_lea = ADD(base, #imm); t_val = LOAD(t_lea_deref)
 * Result:  t_val = LOAD_INDEXED(base, #imm, scale=0)
 *
 * Unlike the SHL-based fusion this does NOT require single-use of t_lea —
 * multiple LOADs through the same address each get rewritten, and DCE
 * cleans up the dead ADD if it ends up with no users. Mapping to
 * LOAD_INDEXED with scale=0 + immediate index also enables the LDRD
 * pairing peephole in ir/codegen.c which only fires on adjacent
 * LOAD_INDEXED instructions with matching base + offset+4.
 *
 * Range guard: only fire when the immediate fits the [Rn, #imm]
 * encoding (`abs(imm) <= 4095` for the word forms). Beyond that, the
 * backend would materialize the immediate into a register and lose the
 * benefit of the fusion.
 * ============================================================================ */

static int arm_extract_add_imm_base(TCCIRState *ir, IRSSAOptCtx *ctx,
                                    int32_t lea_vr, IROperand *out_base,
                                    int32_t *out_imm, int *out_lea_idx)
{
  if (lea_vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, lea_vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;

  /* Every use of the LEA temp must be an ADDRESS use (deref) — i.e. it
   * appears as the pointer operand of a LOAD or STORE. If it has a
   * "value" use (used as data for ADD/SUB/MUL/ASSIGN/etc., or as the
   * source value of a STORE), the LEA is a real pointer that participates
   * in further computation — typically a loop-carried induction variable.
   * Rewriting one address use to LOAD_INDEXED(base, #imm) extends base's
   * liveness past the LEA, and the regalloc may then coalesce base with
   * the post-update phi-copy temp, producing wrong addresses.
   *
   * When every use is an address use, after we rewrite them all the LEA
   * becomes dead and DCE cleans up the ADD — no lifetime extension. */
  for (int u = 0; u < vi->use_count; u++) {
    IRSSAUse use = vi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      return 0;
    IRQuadCompact *uq = &ir->compact_instructions[use.idx];
    if (uq->op == TCCIR_OP_LOAD) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (!s.is_lval || irop_get_vreg(s) != lea_vr)
        return 0;
    } else if (uq->op == TCCIR_OP_STORE) {
      IROperand d = tcc_ir_op_get_dest(ir, uq);
      if (irop_get_vreg(d) != lea_vr)
        return 0;
      /* If the LEA is being used as the STORE's value (not the address),
       * reject. */
      IROperand sv = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(sv) == lea_vr)
        return 0;
    } else {
      return 0;
    }
  }

  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ADD)
    return 0;

  IROperand a = tcc_ir_op_get_src1(ir, dq);
  IROperand b = tcc_ir_op_get_src2(ir, dq);

  IROperand base_op;
  IROperand imm_op;
  if (a.tag == IROP_TAG_IMM32 && b.tag != IROP_TAG_IMM32) {
    imm_op = a; base_op = b;
  } else if (b.tag == IROP_TAG_IMM32 && a.tag != IROP_TAG_IMM32) {
    imm_op = b; base_op = a;
  } else {
    return 0;
  }

  if (base_op.is_lval)
    return 0;
  /* Refuse SYMREF bases here — LOAD_INDEXED with a SYMREF base + imm
   * isn't materially better than the existing LEA, and the backend's
   * fast path for symbol+offset uses different code. */
  if (base_op.tag != IROP_TAG_VREG)
    return 0;

  int32_t imm = irop_get_imm32(imm_op);
  int abs_imm = imm < 0 ? -imm : imm;
  if (abs_imm > 4095)
    return 0;

  *out_base = base_op;
  *out_imm = imm;
  *out_lea_idx = vi->def_instr;
  return 1;
}

int ssa_gen_arm_fuse_load_through_add_imm(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *load_q = &ir->compact_instructions[instr_idx];
  if (load_q->op != TCCIR_OP_LOAD)
    return 0;

  IROperand load_dest_chk = tcc_ir_op_get_dest(ir, load_q);
  /* 64-bit loads: skip. The STORE/LOAD handlers for 64-bit pointer-deref
   * deliberately use two 32-bit ops (not LDRD/STRD) to tolerate unaligned
   * packed-struct addresses; the LOAD_INDEXED/STORE_INDEXED 64-bit paths
   * use LDRD/STRD which faults on misalignment. Don't fuse here. */
  if (irop_get_btype(load_dest_chk) == IROP_BTYPE_INT64 ||
      irop_get_btype(load_dest_chk) == IROP_BTYPE_FLOAT64)
    return 0;

  IROperand load_src = tcc_ir_op_get_src1(ir, load_q);
  if (!load_src.is_lval)
    return 0;
  if (load_src.is_local || load_src.is_llocal)
    return 0;
  if (load_src.tag != IROP_TAG_VREG)
    return 0;

  int32_t lea_vr = irop_get_vreg(load_src);
  IROperand base;
  int32_t imm;
  int lea_idx;
  if (!arm_extract_add_imm_base(ir, ctx, lea_vr, &base, &imm, &lea_idx))
    return 0;

  /* LOAD_INDEXED takes its width from the dest; a LOAD's is its deref's. */
  IROperand load_dest = tcc_ir_op_get_dest(ir, load_q);
  if (!irop_indexed_load_dest(&load_dest, load_src))
    return 0;

  /* Build the new operand pool entry for LOAD_INDEXED(base, imm, scale=0). */
  int lb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (lb + 3 >= ir->iroperand_pool_capacity)
    return 0;

  IROperand idx_op = irop_make_imm32(0, imm, load_dest.btype);
  IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  IROperand base_clean = base;
  base_clean.is_lval = 0;
  irop_carry_access_marks(&base_clean, load_src);

  load_q->op = TCCIR_OP_LOAD_INDEXED;
  load_q->operand_base = lb;
  ir->iroperand_pool[lb + 0] = load_dest;
  ir->iroperand_pool[lb + 1] = base_clean;
  ir->iroperand_pool[lb + 2] = idx_op;
  ir->iroperand_pool[lb + 3] = scale_op;

  /* Use-def chain maintenance:
   *   - Drop the use of lea_vr from this LOAD (no longer references it).
   *   - Add a use of base_vr at this LOAD.
   * The defining ADD becomes dead when lea_vr.use_count hits 0; DCE will
   * remove it. */
  IRSSAVregInfo *lea_vi = ssa_opt_vinfo(ctx, lea_vr);
  if (lea_vi)
    ssa_opt_remove_use_instr(lea_vi, instr_idx);

  int32_t base_vr = irop_get_vreg(base_clean);
  IRSSAVregInfo *base_vi = ssa_opt_vinfo(ctx, base_vr);
  if (base_vi)
    ssa_opt_add_use_instr(base_vi, instr_idx);

  return 1;
}

/* ============================================================================
 * arm_addreg_indexed: t = ADD(a, b) feeding only memory accesses
 *
 * Pattern: t = ADD(a, b); x = LOAD(*t) / STORE(*t) <- v   (a, b registers)
 * Result:  x = LOAD_INDEXED(a, b, #0) / STORE_INDEXED(a, v, b, #0)
 *
 * LDR/STR (and the byte/halfword forms) take `[Rn, Rm]`; the flat indexed
 * fusion runs before param_home_fwd / SRA turn by-value slice params and
 * Zig's `&slice.ptr[i]` into register bases, so the address stayed an ADD
 * and a plain access: `adds r2, r0, r1; ldrb r3, [r2]` per byte of a
 * string scan (mem.findSentinel), of Tokenizer.next, of eqlBytes.  Every use
 * of t must be such an access (then the ADD dies); 64-bit and floating
 * values are left alone -- LDRD/STRD/VLDR/VSTR have no register offset.
 * zig.c -O2: -12.6M instructions, -1.7 KB (10-08 plan, W4).
 * ============================================================================ */

static int arm_addreg_value_ok(IROperand v)
{
  const int bt = irop_get_btype(v);
  return bt != IROP_BTYPE_INT64 && bt != IROP_BTYPE_FLOAT32 && bt != IROP_BTYPE_FLOAT64;
}

/* t (lea_vr) = ADD(a, b) of two registers, single-def, every use an address
 * of a LOAD / STORE, and a, b still hold their values at mem_idx. */
static int arm_addreg_base(IRSSAOptCtx *ctx, int32_t lea_vr, int mem_idx, IROperand *out_a, IROperand *out_b)
{
  TCCIRState *ir = ctx->ir;
  if (tcc_ir_opt_pass_disabled("arm_addreg_indexed"))
    return 0;
  if (lea_vr < 0 || TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, lea_vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  for (int u = 0; u < vi->use_count; u++)
  {
    IRSSAUse use = vi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      return 0;
    IRQuadCompact *uq = &ir->compact_instructions[use.idx];
    if (uq->op == TCCIR_OP_LOAD)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, uq);
      if (!s1.is_lval || irop_get_vreg(s1) != lea_vr)
        return 0;
    }
    else if (uq->op == TCCIR_OP_STORE)
    {
      IROperand d = tcc_ir_op_get_dest(ir, uq);
      if (irop_get_vreg(d) != lea_vr || irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == lea_vr)
        return 0;
    }
    else
      return 0;
  }
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ADD)
    return 0;
  if (ir->barrel_shifts && dq->orig_index >= 0 && dq->orig_index < ir->barrel_shifts_len &&
      ir->barrel_shifts[dq->orig_index] != 0)
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, dq);
  IROperand b = tcc_ir_op_get_src2(ir, dq);
  if (a.tag != IROP_TAG_VREG || b.tag != IROP_TAG_VREG || a.is_lval || b.is_lval || a.is_local || b.is_local ||
      a.is_llocal || b.is_llocal || a.is_sym || b.is_sym)
    return 0;
  if (irop_is_64bit(a) || irop_is_64bit(b) || irop_is_64bit(tcc_ir_op_get_dest(ir, dq)))
    return 0;
  if (!arm_indexed_operands_hold(ctx, vi->def_instr, mem_idx, b, a))
    return 0;
  *out_a = a;
  *out_b = b;
  return 1;
}

/* Swap t's use at mem_idx for uses of a and b. */
static void arm_addreg_retarget_uses(IRSSAOptCtx *ctx, int32_t lea_vr, int mem_idx, IROperand a, IROperand b)
{
  IRSSAVregInfo *lvi = ssa_opt_vinfo(ctx, lea_vr);
  if (lvi)
    ssa_opt_remove_use_instr(lvi, mem_idx);
  IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, irop_get_vreg(a));
  if (avi)
    ssa_opt_add_use_instr(avi, mem_idx);
  IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, irop_get_vreg(b));
  if (bvi)
    ssa_opt_add_use_instr(bvi, mem_idx);
}

static int ssa_gen_arm_fuse_load_through_add_reg(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_LOAD)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);
  if (!arm_addreg_value_ok(dest) || !src.is_lval || src.is_local || src.is_llocal || src.tag != IROP_TAG_VREG ||
      tcc_ir_access_is_volatile(ir, src))
    return 0;
  int32_t lea_vr = irop_get_vreg(src);
  IROperand a, b;
  if (!arm_addreg_base(ctx, lea_vr, instr_idx, &a, &b))
    return 0;
  int nb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (nb + 3 >= ir->iroperand_pool_capacity)
    return 0;
  a.is_lval = 0;
  irop_carry_access_marks(&a, src);
  q->op = TCCIR_OP_LOAD_INDEXED;
  q->operand_base = nb;
  ir->iroperand_pool[nb + 0] = dest;
  ir->iroperand_pool[nb + 1] = a;
  ir->iroperand_pool[nb + 2] = b;
  ir->iroperand_pool[nb + 3] = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  arm_addreg_retarget_uses(ctx, lea_vr, instr_idx, a, b);
  return 1;
}

static int ssa_gen_arm_fuse_store_through_add_reg(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_STORE)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand val = tcc_ir_op_get_src1(ir, q);
  if (!arm_addreg_value_ok(dest) || !arm_addreg_value_ok(val) || dest.is_local || dest.is_llocal ||
      dest.tag != IROP_TAG_VREG || tcc_ir_access_is_volatile(ir, dest))
    return 0;
  int32_t lea_vr = irop_get_vreg(dest);
  IROperand a, b;
  if (!arm_addreg_base(ctx, lea_vr, instr_idx, &a, &b))
    return 0;
  int nb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (nb + 3 >= ir->iroperand_pool_capacity)
    return 0;
  a.is_lval = 0;
  irop_carry_access_marks(&a, dest);
  q->op = TCCIR_OP_STORE_INDEXED;
  q->operand_base = nb;
  ir->iroperand_pool[nb + 0] = a;
  ir->iroperand_pool[nb + 1] = val;
  ir->iroperand_pool[nb + 2] = b;
  ir->iroperand_pool[nb + 3] = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  arm_addreg_retarget_uses(ctx, lea_vr, instr_idx, a, b);
  return 1;
}

static int ssa_gen_arm_fuse_load_combined(IRSSAOptCtx *ctx, int instr_idx)
{
  int r = ssa_gen_arm_fuse_load_through_add_imm(ctx, instr_idx);
  if (r > 0)
    return r;
  return ssa_gen_arm_fuse_load_through_add_reg(ctx, instr_idx);
}

/* ============================================================================
 * ssa_gen_arm_fuse_store_through_add_imm
 *
 * Mirror of the load variant for STORE.
 * Pattern: t_lea = ADD(base, #imm); STORE(t_lea_deref, val)
 * Result:  STORE_INDEXED(base, val, #imm, scale=0)
 * ============================================================================ */

int ssa_gen_arm_fuse_store_through_add_imm(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *store_q = &ir->compact_instructions[instr_idx];
  if (store_q->op != TCCIR_OP_STORE)
    return 0;

  IROperand store_src = tcc_ir_op_get_src1(ir, store_q);
  /* See LOAD variant: skip 64-bit to avoid STRD on packed/misaligned addresses. */
  if (irop_get_btype(store_src) == IROP_BTYPE_INT64 ||
      irop_get_btype(store_src) == IROP_BTYPE_FLOAT64)
    return 0;

  IROperand store_dest = tcc_ir_op_get_dest(ir, store_q);
  if (store_dest.is_local || store_dest.is_llocal)
    return 0;
  if (store_dest.tag != IROP_TAG_VREG)
    return 0;

  int32_t lea_vr = irop_get_vreg(store_dest);
  IROperand base;
  int32_t imm;
  int lea_idx;
  if (!arm_extract_add_imm_base(ir, ctx, lea_vr, &base, &imm, &lea_idx))
    return 0;

  int sb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (sb + 3 >= ir->iroperand_pool_capacity)
    return 0;

  IROperand idx_op = irop_make_imm32(0, imm, store_src.btype);
  IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  IROperand base_clean = base;
  base_clean.is_lval = 0;
  irop_carry_access_marks(&base_clean, store_dest);

  store_q->op = TCCIR_OP_STORE_INDEXED;
  store_q->operand_base = sb;
  ir->iroperand_pool[sb + 0] = base_clean;
  ir->iroperand_pool[sb + 1] = store_src;
  ir->iroperand_pool[sb + 2] = idx_op;
  ir->iroperand_pool[sb + 3] = scale_op;

  IRSSAVregInfo *lea_vi = ssa_opt_vinfo(ctx, lea_vr);
  if (lea_vi)
    ssa_opt_remove_use_instr(lea_vi, instr_idx);

  int32_t base_vr = irop_get_vreg(base_clean);
  IRSSAVregInfo *base_vi = ssa_opt_vinfo(ctx, base_vr);
  if (base_vi)
    ssa_opt_add_use_instr(base_vi, instr_idx);

  return 1;
}

/* ============================================================================
 * ssa_gen_arm_fuse_mla_accum_through_add_imm
 *
 * Pattern: t_lea = ADD(base, #imm); MLA dest, src1, src2 + t_lea_deref
 *          where t_lea is single-use (only as MLA's accum deref).
 * Result:  t_lea = LOAD_INDEXED(base, #imm, scale=0)
 *          MLA dest, src1, src2 + t_lea  (accum non-deref)
 *
 * The MLA accumulator carries a memory-deref operand directly in the IR —
 * codegen materialises it as `LEA + LDR` (2 insns).  Rewriting the LEA's
 * defining ADD into LOAD_INDEXED collapses both into a single
 * `LDR rD, [base, #imm]`, saving one instruction.  This mirrors the
 * LOAD-side fusion but reuses the LEA's instruction slot for the LOAD
 * (no IR insertion needed).
 *
 * The transform is destructive on t_lea's value (it no longer holds an
 * address, but the loaded value), so it only fires when t_lea is used
 * exactly once and that use is the MLA accum deref.
 * ============================================================================ */

int ssa_gen_arm_fuse_mla_accum_through_add_imm(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *mla_q = &ir->compact_instructions[instr_idx];
  if (mla_q->op != TCCIR_OP_MLA)
    return 0;

  /* Accum is at operand_base + 3. */
  IROperand accum = ir->iroperand_pool[mla_q->operand_base + 3];
  if (!accum.is_lval || accum.is_llocal || accum.is_local || accum.is_sym)
    return 0;
  if (accum.tag != IROP_TAG_VREG)
    return 0;
  int32_t lea_vr = irop_get_vreg(accum);
  if (lea_vr < 0 || TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* 64-bit accumulators aren't supported by MLA on Cortex-M; this also
   * sidesteps the LDRD-alignment trap that the LOAD-side fusion guards
   * against. */
  IROperand mla_dest = ir->iroperand_pool[mla_q->operand_base + 0];
  if (irop_get_btype(mla_dest) == IROP_BTYPE_INT64)
    return 0;
  if (irop_get_btype(accum) == IROP_BTYPE_INT64 ||
      irop_get_btype(accum) == IROP_BTYPE_FLOAT64)
    return 0;

  /* t_lea must be single-use (only this MLA's accum) and defined by ADD
   * with a register base + immediate offset that fits the LDR encoding. */
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, lea_vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0 || vi->use_count != 1)
    return 0;
  if (vi->uses[0].kind != SSA_USE_INSTR || vi->uses[0].idx != instr_idx)
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ADD)
    return 0;

  IROperand a = tcc_ir_op_get_src1(ir, dq);
  IROperand b = tcc_ir_op_get_src2(ir, dq);
  IROperand base_op, imm_op;
  if (a.tag == IROP_TAG_IMM32 && b.tag != IROP_TAG_IMM32) {
    imm_op = a; base_op = b;
  } else if (b.tag == IROP_TAG_IMM32 && a.tag != IROP_TAG_IMM32) {
    imm_op = b; base_op = a;
  } else {
    return 0;
  }
  if (base_op.is_lval || base_op.tag != IROP_TAG_VREG)
    return 0;

  int32_t imm = irop_get_imm32(imm_op);
  int abs_imm = imm < 0 ? -imm : imm;
  if (abs_imm > 4095)
    return 0;

  /* Same hoist hazard as the STORE-source sibling: this fuses the MLA's
   * accumulator deref by turning the address-computing ADD itself into a
   * LOAD_INDEXED, i.e. the load is RELOCATED upward from this MLA to the
   * ADD's definition site.  That hoist is only sound when nothing between the
   * two positions can write the loaded memory or divert control flow.  LICM /
   * GVN can place the defining ADD before an intervening aliasing store, call,
   * or branch (e.g. an invariant `t = base + #imm` hoisted above a store that
   * may alias `*t`); hoisting the load to the ADD would then read the pre-store
   * value.  Bail on any intervening memory clobber or control-flow op; a join
   * target between the two (a loop back-edge landing on a fallthrough block
   * boundary emits no JUMP of its own) also means a path from the ADD to the
   * use wraps around later stores, so it bails too — the hoist stays inside
   * one straight-line basic block for real. */
  {
    int didx = vi->def_instr;
    if (didx >= instr_idx)
      return 0;
    for (int j = didx + 1; j < instr_idx; j++) {
      if (ir->compact_instructions[j].is_jump_target)
        return 0;
      switch (ir->compact_instructions[j].op) {
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_VLA_ALLOC:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
        return 0;
      default:
        break;
      }
    }
  }

  IROperand lea_dest = tcc_ir_op_get_dest(ir, dq);
  IROperand base_clean = base_op;
  base_clean.is_lval = 0;
  irop_carry_access_marks(&base_clean, accum);

  /* Rewrite the defining ADD into LOAD_INDEXED(base, #imm, scale=0). */
  int lb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (lb + 3 >= ir->iroperand_pool_capacity)
    return 0;

  IROperand idx_op = irop_make_imm32(0, imm, irop_get_btype(lea_dest));
  IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  dq->op = TCCIR_OP_LOAD_INDEXED;
  dq->operand_base = lb;
  ir->iroperand_pool[lb + 0] = lea_dest;
  ir->iroperand_pool[lb + 1] = base_clean;
  ir->iroperand_pool[lb + 2] = idx_op;
  ir->iroperand_pool[lb + 3] = scale_op;

  /* Update use-def chain: the ADD used to read base+imm; now it reads
   * base directly with an embedded immediate index.  Drop the base's
   * existing use at vi->def_instr (already there from the ADD form) —
   * actually LOAD_INDEXED still uses base at this same instruction, so
   * leave the existing use record in place. */

  /* Rewrite MLA's accum: clear is_lval so the MLA reads t_lea as a value. */
  accum.is_lval = 0;
  ir->iroperand_pool[mla_q->operand_base + 3] = accum;

  return 1;
}

/* ============================================================================
 * ssa_gen_arm_fuse_store_src_through_add_imm
 *
 * Pattern: t_lea = ADD(base, #imm); STORE(V, *t_lea_DEREF)
 *          where t_lea is single-use (only as the STORE's src deref).
 * Result:  t_lea = LOAD_INDEXED(base, #imm, scale=0); STORE(V, t_lea)
 *
 * This is the SRC-side mirror of fuse_store_through_add_imm (which handles
 * *t_lea = val — t_lea as the STORE *destination* address).  Inlined
 * helpers like check1 produce `V <- c->field [STORE]` patterns where the
 * field-address LEA's only use is the STORE's deref source — a pure
 * address use that should fuse to a single `ldr [base, #imm]`.  Mirrors
 * fuse_mla_accum_through_add_imm: rewrites the LEA's slot to
 * LOAD_INDEXED, then clears is_lval on the STORE's src.
 *
 * Skips 64-bit (LDRD alignment, see [feedback_lea_fusion_addr_only]) and
 * requires the LEA's only use to be this STORE's src1 deref — same
 * invariant as the MLA-accum variant.
 * ============================================================================ */

int ssa_gen_arm_fuse_store_src_through_add_imm(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *store_q = &ir->compact_instructions[instr_idx];
  if (store_q->op != TCCIR_OP_STORE)
    return 0;

  IROperand store_src = tcc_ir_op_get_src1(ir, store_q);
  if (!store_src.is_lval || store_src.is_llocal || store_src.is_local || store_src.is_sym)
    return 0;
  if (store_src.tag != IROP_TAG_VREG)
    return 0;

  int store_btype = irop_get_btype(store_src);
  if (store_btype == IROP_BTYPE_INT64 || store_btype == IROP_BTYPE_FLOAT64)
    return 0;

  int32_t lea_vr = irop_get_vreg(store_src);
  if (lea_vr < 0 || TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, lea_vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0 || vi->use_count != 1)
    return 0;
  if (vi->uses[0].kind != SSA_USE_INSTR || vi->uses[0].idx != instr_idx)
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ADD)
    return 0;

  IROperand a = tcc_ir_op_get_src1(ir, dq);
  IROperand b = tcc_ir_op_get_src2(ir, dq);
  IROperand base_op, imm_op;
  if (a.tag == IROP_TAG_IMM32 && b.tag != IROP_TAG_IMM32) {
    imm_op = a; base_op = b;
  } else if (b.tag == IROP_TAG_IMM32 && a.tag != IROP_TAG_IMM32) {
    imm_op = b; base_op = a;
  } else {
    return 0;
  }
  if (base_op.is_lval || base_op.tag != IROP_TAG_VREG)
    return 0;

  int32_t imm = irop_get_imm32(imm_op);
  int abs_imm = imm < 0 ? -imm : imm;
  if (abs_imm > 4095)
    return 0;

  /* Unlike the LOAD variant (which rewrites the load op in place), this fuses
   * the deref *source* of a STORE by turning the address-computing ADD into the
   * LOAD_INDEXED — i.e. the load is RELOCATED upward from this STORE to the
   * ADD's definition site.  That hoist is only sound when nothing between the
   * two positions can write the loaded memory or divert control flow.  GVN can
   * CSE the address so the defining ADD sits before a later store to the same
   * slot (fuzz seed 2137: `arr[i]` read, `arr[i]=v`, then an unrolled re-read of
   * arr[i] whose address was CSE'd back to the first read's LEA) — the hoisted
   * load would then read the pre-store value.  Bail on any intervening memory
   * clobber or control-flow op (the latter also restricts the hoist to a single
   * straight-line basic block). */
  {
    int didx = vi->def_instr;
    if (didx >= instr_idx)
      return 0;
    for (int j = didx + 1; j < instr_idx; j++) {
      /* A join target inside the range is an edge entering between the ADD
       * and the use — most importantly a loop back-edge landing on a
       * fallthrough block boundary, which emits no JUMP of its own.  A path
       * from the ADD to the use then wraps around stores executed after the
       * ADD (LICM hoists the invariant `t = base + #imm` above a store to
       * `*t` inside the loop; fuzz seed 2395), and the linear scan below
       * cannot see them. */
      if (ir->compact_instructions[j].is_jump_target)
        return 0;
      switch (ir->compact_instructions[j].op) {
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_VLA_ALLOC:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
        return 0;
      default:
        break;
      }
    }
  }

  IROperand lea_dest = tcc_ir_op_get_dest(ir, dq);
  /* Update btype to match the loaded value (the LEA dest was a pointer-typed
   * INT32; after fusion it holds the loaded value). */
  IROperand lea_dest_new = lea_dest;
  lea_dest_new.btype = store_btype;

  IROperand base_clean = base_op;
  base_clean.is_lval = 0;
  irop_carry_access_marks(&base_clean, store_src);

  int lb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (lb + 3 >= ir->iroperand_pool_capacity)
    return 0;

  IROperand idx_op = irop_make_imm32(0, imm, store_btype);
  IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  dq->op = TCCIR_OP_LOAD_INDEXED;
  dq->operand_base = lb;
  ir->iroperand_pool[lb + 0] = lea_dest_new;
  ir->iroperand_pool[lb + 1] = base_clean;
  ir->iroperand_pool[lb + 2] = idx_op;
  ir->iroperand_pool[lb + 3] = scale_op;

  /* Clear is_lval on the STORE's src so the codegen reads t_lea as a value
   * (the loaded data) instead of dereferencing it again. */
  IROperand new_src = store_src;
  new_src.is_lval = 0;
  new_src.btype = store_btype;
  tcc_ir_set_src1(ir, instr_idx, new_src);

  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

/* Combined dispatcher: the gen-table runner breaks after the first matching
 * entry regardless of return value, so two gens for TCCIR_OP_SHL would never
 * both get a chance. Try LOAD_INDEXED first; if it doesn't fire, fall through
 * to STORE_INDEXED. */
static int ssa_gen_arm_fuse_shl_indexed(IRSSAOptCtx *ctx, int instr_idx)
{
  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(ctx, instr_idx);
  if (r > 0)
    return r;
  return ssa_gen_arm_fuse_shl_add_to_store_indexed(ctx, instr_idx);
}

/* STORE dispatcher: try the dest-side address fusion first (the original
 * "store through LEA"), then the src-side fusion that handles the inlined
 * `V <- *(base + #imm) [STORE]` pattern. */
static int ssa_gen_arm_fuse_store_add_imm_combined(IRSSAOptCtx *ctx, int instr_idx)
{
  int r = ssa_gen_arm_fuse_store_through_add_imm(ctx, instr_idx);
  if (r > 0)
    return r;
  r = ssa_gen_arm_fuse_store_src_through_add_imm(ctx, instr_idx);
  if (r > 0)
    return r;
  return ssa_gen_arm_fuse_store_through_add_reg(ctx, instr_idx);
}

/* ============================================================================
 * ssa_gen_arm_mla_zero_product
 *
 * Pattern: t = MLA(a, #0, c) or MLA(#0, b, c)  ->  t = ASSIGN c
 *
 * The fusion above runs while a multiplicand is still a register holding 0;
 * var_imm_prop turns it into #0 afterwards, and nothing folded the product.
 * zig.h's 128-bit multiply leaves exactly these: the cross terms of a u64
 * widened to u128, whose high half is constant 0 (Wyhash's mum).
 * ============================================================================ */

static int ssa_gen_arm_mla_zero_product(IRSSAOptCtx *ctx, int instr_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_MLA)
    return 0;
  const int base = q->operand_base;
  IROperand dest = ir->iroperand_pool[base + 0];
  IROperand m1 = ir->iroperand_pool[base + 1];
  IROperand m2 = ir->iroperand_pool[base + 2];
  IROperand accum = ir->iroperand_pool[base + 3];
  if (irop_get_btype(dest) == IROP_BTYPE_INT64 || irop_get_btype(accum) == IROP_BTYPE_INT64 ||
      irop_get_btype(accum) == IROP_BTYPE_FLOAT64)
    return 0;
  if (!((irop_is_immediate(m1) && !m1.is_lval && irop_get_imm64_ex(ir, m1) == 0) ||
        (irop_is_immediate(m2) && !m2.is_lval && irop_get_imm64_ex(ir, m2) == 0)))
    return 0;
  /* An operand that reads memory must still be read (volatile): leave it. */
  if (m1.is_lval || m2.is_lval)
    return 0;
  if (ir->barrel_shifts && q->orig_index >= 0 && q->orig_index < ir->barrel_shifts_len &&
      ir->barrel_shifts[q->orig_index] != 0)
    return 0;
  for (int k = 1; k <= 2; k++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(k == 1 ? m1 : m2));
    if (vi)
      ssa_opt_remove_use_instr(vi, instr_idx);
  }
  /* ASSIGN dest <- accum: the 3-operand layout ([dest, src1, src2]) at a
   * fresh pool slot, as the fusion allocates one for its 4. */
  int nb = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  if (nb + 2 >= ir->iroperand_pool_capacity)
    return 0;
  ir->iroperand_pool[nb + 0] = dest;
  ir->iroperand_pool[nb + 1] = accum;
  ir->iroperand_pool[nb + 2] = IROP_NONE;
  q->op = TCCIR_OP_ASSIGN;
  q->operand_base = nb;
  return 1;
}

static const IRSSAOptGen ssa_gen_arm[] = {
  { TCCIR_OP_MUL,   ssa_gen_arm_fuse_mul_add_to_mla,             "arm_mla_fusion" },
  { TCCIR_OP_MUL,   ssa_gen_arm_reduce_mul_to_shift,             "arm_mul_to_shl" },
  { TCCIR_OP_SHL,   ssa_gen_arm_fuse_shl_indexed,                "arm_shl_indexed" },
  { TCCIR_OP_LOAD,  ssa_gen_arm_fuse_load_combined,              "arm_load_add_imm" },
  { TCCIR_OP_STORE, ssa_gen_arm_fuse_store_add_imm_combined,     "arm_store_add_imm" },
  { TCCIR_OP_MLA,   ssa_gen_arm_mla_zero_product,                "arm_mla_zero_product" },
  { TCCIR_OP_MLA,   ssa_gen_arm_fuse_mla_accum_through_add_imm,  "arm_mla_accum_add_imm" },
};

void tcc_ir_ssa_opt_arm_register(void)
{
  tcc_ir_ssa_opt_register_target(ssa_gen_arm,
                                 sizeof(ssa_gen_arm) / sizeof(ssa_gen_arm[0]));
}
