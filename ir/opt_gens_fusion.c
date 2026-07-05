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

static int ir_gen_is_mla_mul_op(TccIrOp op)
{
  return op == TCCIR_OP_MUL || op == TCCIR_OP_UMULL || op == TCCIR_OP_SMULL;
}

static int ir_gen_is_long_mla_mul_op(TccIrOp op)
{
  return op == TCCIR_OP_UMULL || op == TCCIR_OP_SMULL;
}

static int ir_gen_operand_aliases_accum_low(IROptCtx *ctx, IROperand op, IROperand accum_op, int use_idx, int depth)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (depth > 6)
    return 0;
  if (irop_get_vreg(op) == irop_get_vreg(accum_op))
    return 1;
  if (!irop_has_vreg(op))
    return 0;

  int def_idx = ir_opt_du_def(du, irop_get_vreg(op), use_idx);
  if (def_idx < 0)
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LOAD)
    return 0;

  return ir_gen_operand_aliases_accum_low(ctx, tcc_ir_op_get_src1(ir, dq), accum_op, def_idx, depth + 1);
}

static int ir_gen_operand_derived_from_memory(IROptCtx *ctx, IROperand op, int use_idx, int depth)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (depth > 8)
    return 1;
  if (ir_xform_operand_reads_memory(op))
    return 1;
  if (!irop_has_vreg(op))
    return 0;

  int def_idx = ir_opt_du_def(du, irop_get_vreg(op), use_idx);
  if (def_idx < 0)
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op == TCCIR_OP_LOAD || dq->op == TCCIR_OP_LOAD_INDEXED ||
      dq->op == TCCIR_OP_LOAD_POSTINC)
    return 1;

  if (irop_config[dq->op].has_src1 &&
      ir_gen_operand_derived_from_memory(ctx, tcc_ir_op_get_src1(ir, dq), def_idx, depth + 1))
    return 1;
  if (irop_config[dq->op].has_src2 &&
      ir_gen_operand_derived_from_memory(ctx, tcc_ir_op_get_src2(ir, dq), def_idx, depth + 1))
    return 1;
  if (dq->op == TCCIR_OP_MLA &&
      ir_gen_operand_derived_from_memory(ctx, tcc_ir_op_get_accum(ir, dq), def_idx, depth + 1))
    return 1;

  return 0;
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
    if (idx >= 0 && ir_gen_is_mla_mul_op(ir->compact_instructions[idx].op)) {
      mul_result_vr = vr;
      accum_op = add_src1;
      mul_idx = idx;
      mul_q = &ir->compact_instructions[mul_idx];
    }
  }
  if (!mul_q && irop_has_vreg(add_src1)) {
    int32_t vr = irop_get_vreg(add_src1);
    int idx = ir_opt_du_def(du, vr, i);
    if (idx >= 0 && ir_gen_is_mla_mul_op(ir->compact_instructions[idx].op)) {
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

  TccIrOp old_mul_op = mul_q->op;
  IROperand ms1 = tcc_ir_op_get_src1(ir, mul_q);
  IROperand ms2 = tcc_ir_op_get_src2(ir, mul_q);
  const int long_mla = ir_gen_is_long_mla_mul_op(old_mul_op);

  int dup_mul = 0;
  if (!long_mla && !ms1.is_lval && !ms2.is_lval && !irop_is_immediate(ms1) && !irop_is_immediate(ms2)) {
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
      (!long_mla && (irop_is_immediate(ms1) || irop_is_immediate(ms2))) || dup_mul ||
      ir_opt_du_uses(du, mul_result_vr) != 1)
    return 0;

  if (!ir_xform_same_block(ir, mul_idx, i))
    return 0;

  /* Fusion moves the ADD's accumulator into the earlier MUL slot and hides the
   * three-source expression behind one MLA.  Several downstream passes and RA
   * helpers now understand the explicit accumulator, but memory-derived inputs
   * still create stale-value paths after copy/const propagation on aggregate
   * fuzz cases.  Leave those as separate MUL+ADD until the full pipeline can
   * model the hidden data dependency as precisely as normal src1/src2 uses. */
  if (ir_gen_operand_derived_from_memory(ctx, ms1, mul_idx, 0) ||
      ir_gen_operand_derived_from_memory(ctx, ms2, mul_idx, 0) ||
      ir_gen_operand_derived_from_memory(ctx, accum_op, i, 0))
    return 0;

  int32_t accum_vr = irop_get_vreg(accum_op);
  if (accum_vr >= 0) {
    int adef = ir_opt_du_def(du, accum_vr, i);
    if (adef >= 0 && adef >= mul_idx)
      return 0;
  }

  /* The MLA lands at the MUL's position, hoisting the ADD's accumulator
   * read up to it.  A memory-read accumulator (fused lvalue load) must not
   * skip stores between the MUL and the ADD (mirror of the SSA-side
   * mul-operand sink guard; volatile fuzz seed 5053 family). */
  if (ir_xform_operand_reads_memory(accum_op) &&
      (q->is_jump_target || !ir_xform_range_preserves_memory(ir, mul_idx, i)))
    return 0;

  IROperand final_dest = add_dest;
  int store_idx = -1;
  if (long_mla && irop_has_vreg(add_dest) && ir_opt_du_uses(du, irop_get_vreg(add_dest)) == 1) {
    int next = i + 1;
    while (next < ir->next_instruction_index && ir->compact_instructions[next].op == TCCIR_OP_NOP)
      next++;
    if (next < ir->next_instruction_index && ir->compact_instructions[next].op == TCCIR_OP_STORE &&
        !ir->compact_instructions[next].is_jump_target && ir_xform_same_block(ir, i, next)) {
      IRQuadCompact *sq = &ir->compact_instructions[next];
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);
      if (irop_get_vreg(st_src) == irop_get_vreg(add_dest) && irop_get_vreg(st_dest) == accum_vr) {
        final_dest = st_dest;
        store_idx = next;
      }
    }
  }
  /* A 64-bit MLA is lowered only to SMLAL/UMLAL, which accumulate in place:
   * the destination register pair must equal the accumulator pair.  We only
   * reach that form when the result is stored straight back to the
   * accumulator's own slot (store_idx >= 0, so final_dest's vreg == accum_vr).
   * Without such a store-back, final_dest is a fresh temp distinct from the
   * accumulator and tcc_gen_machine_mlal_accum_mop cannot lower it, which used
   * to abort codegen with "unable to lower 64-bit MLA".  Leave those cases as
   * SMULL/UMULL + 64-bit ADD; the SMULL/UMULL codegen path still applies its
   * own SMLAL peephole (with a safe fallback) for the genuinely in-place ones. */
  if (long_mla && store_idx < 0)
    return 0;

  if (long_mla)
    final_dest.is_unsigned = (old_mul_op == TCCIR_OP_UMULL);

  if (long_mla) {
    if (ir_gen_operand_aliases_accum_low(ctx, ms1, accum_op, mul_idx, 0))
      tcc_ir_set_src1(ir, mul_idx, accum_op);
    if (ir_gen_operand_aliases_accum_low(ctx, ms2, accum_op, mul_idx, 0))
      tcc_ir_set_src2(ir, mul_idx, accum_op);
  }

  mul_q->op = TCCIR_OP_MLA;

  /* The MLA has four operands (dest, src1, src2, accum) but the original MUL
   * only allocated three slots.  Growing the block in-place at operand_base+3
   * can overwrite operands of instructions whose operand blocks were allocated
   * between the MUL and the ADD, so move the whole operand block to a fresh
   * 4-slot region at the end of the pool. */
  {
    int new_base = ir->iroperand_pool_count;
    tcc_ir_pool_ensure(ir, 4);
    tcc_ir_pool_add(ir, final_dest);
    tcc_ir_pool_add(ir, tcc_ir_op_get_src1(ir, mul_q));
    tcc_ir_pool_add(ir, tcc_ir_op_get_src2(ir, mul_q));
    tcc_ir_pool_add(ir, accum_op);
    mul_q->operand_base = new_base;
  }

  q->op = TCCIR_OP_NOP;
  if (store_idx >= 0)
    ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
  return 1;
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
  IROperand index_op = IROP_NONE;
  int shl_idx = -1;
  IRQuadCompact *shl_q = NULL;
  int shift_amount = 0;

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

  if (shl_idx >= 0) {
    /* Scaled index: base + (index << scale), scale in 1..3 (int/short/long). */
    if (ir_opt_du_uses(du, offset_vr) != 1)
      return 0;

    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    if (!shl_src2.is_const)
      return 0;

    shift_amount = shl_src2.u.imm32;
    if (shift_amount < 1 || shift_amount > 3)
      return 0;

    index_op = tcc_ir_op_get_src1(ir, shl_q);
    if (index_op.is_local || index_op.is_llocal)
      return 0;
    if (base_op.is_llocal || base_op.is_lval)
      return 0;

    for (int j = shl_idx + 1; j < i; j++) {
      TccIrOp bop = ir->compact_instructions[j].op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF || bop == TCCIR_OP_NOP)
        return 0;
    }
  } else {
    /* Unscaled register index: base + index (scale 0).  Matches byte-array
     * accesses `arr[i]` (and any element type where no SHL is generated):
     * ARM encodes these as LDRB/STRB/LDR Rt,[Rn,Rm], folding the separate
     * `ADD addr,base,index` into the load/store's addressing mode.  Both ADD
     * operands must be plain registers — a constant operand is the
     * displacement case handled by the disp-fusion pass. */
    if (add_idx >= i)
      return 0;
    if (!irop_has_vreg(add_src1) || !irop_has_vreg(add_src2))
      return 0;
    if (add_src1.is_const || add_src2.is_const)
      return 0;

    base_op = add_src1;
    index_op = add_src2;
    shift_amount = 0;

    /* The index may be a plain register value or a stack-local lvalue (the
     * register allocator promotes a VT_LOCAL|VT_LVAL operand to a register, or
     * the backend loads it via mach_ensure_in_reg).  Reject double-indirection
     * (is_llocal) and bare pointer lvalues (is_lval without is_local), which
     * would need an extra dereference the index slot cannot express.  The base
     * must be a plain pointer register. */
    if (index_op.is_llocal || (index_op.is_lval && !index_op.is_local))
      return 0;
    if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
      return 0;

    /* The ADD must reach the memory op with no intervening control flow and
     * no redefinition of either address component, so the fused load/store
     * recomputes the same effective address. */
    int32_t base_vr = irop_get_vreg(base_op);
    int32_t index_vr = irop_get_vreg(index_op);
    for (int j = add_idx + 1; j < i; j++) {
      IRQuadCompact *bq = &ir->compact_instructions[j];
      if (bq->op == TCCIR_OP_JUMP || bq->op == TCCIR_OP_JUMPIF || bq->op == TCCIR_OP_NOP)
        return 0;
      IROperand bd = tcc_ir_op_get_dest(ir, bq);
      if (irop_has_vreg(bd)) {
        int32_t dvr = irop_get_vreg(bd);
        if (dvr == base_vr || dvr == index_vr)
          return 0;
      }
    }
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

  if (shl_idx >= 0)
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

static int ir_gen_deref_indexed_fusion(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (!tcc_state->opt_indexed_memory)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[i];

  /* CMP is intentionally excluded: folding a deref operand into a CMP rewrites
   * the read as a LOAD_INDEXED that a downstream dead-store/alias pass fails to
   * recognize as a use, which can delete the producing stores (miscompiles
   * gcc-torture loop-11).  Safe folding here would need an intervening-store
   * guard + alias-aware DSE — left as a future lever. */
  if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_LOAD_INDEXED ||
      q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
      q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
      q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE ||
      q->op == TCCIR_OP_RETURNVOID)
    return 0;

  int operand_positions[2] = {0, 0};
  int num_deref = 0;

  /* Only a genuine pointer dereference (address held in a register/temp) can be
   * folded into a load's addressing mode.  An is_local/is_llocal lvalue is a
   * stack-variable access whose def assigns the variable's *value*, not its
   * address (e.g. `q = p + 4; r = q - i` — the `+4` is q's value, not an
   * address to load), so folding it would corrupt the variable read. */
  if (irop_config[q->op].has_src1) {
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (s1.is_lval && !s1.is_local && !s1.is_llocal && irop_has_vreg(s1))
      operand_positions[num_deref++] = 1;
  }
  if (irop_config[q->op].has_src2) {
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (s2.is_lval && !s2.is_local && !s2.is_llocal && irop_has_vreg(s2))
      operand_positions[num_deref++] = 2;
  }

  if (num_deref == 0)
    return 0;

  int total_changes = 0;
  for (int d = 0; d < num_deref; d++) {
    int src_pos = operand_positions[d];
    IROperand deref_op = (src_pos == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);

    int32_t addr_vr = irop_get_vreg(deref_op);
    if (addr_vr < 0)
      continue;
    if (ir_opt_du_uses(du, addr_vr) != 1)
      continue;

    int add_idx = ir_opt_du_def(du, addr_vr, i);
    if (add_idx < 0)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

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
        offset_vr = vr1; base_op = add_src2; shl_idx = idx1;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }
    if (shl_idx < 0 && irop_has_vreg(add_src2)) {
      int32_t vr2 = irop_get_vreg(add_src2);
      int idx2 = ir_opt_du_def(du, vr2, add_idx);
      if (idx2 >= 0 && ir->compact_instructions[idx2].op == TCCIR_OP_SHL) {
        offset_vr = vr2; base_op = add_src1; shl_idx = idx2;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }
    IROperand index_op;
    int scale_amount;

    if (shl_idx >= 0) {
      /* Scaled register index: base + (index << scale), scale 1..3. */
      if (ir_opt_du_uses(du, offset_vr) != 1)
        continue;

      IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
      if (!shl_src2.is_const)
        continue;
      scale_amount = shl_src2.u.imm32;
      if (scale_amount < 1 || scale_amount > 3)
        continue;

      index_op = tcc_ir_op_get_src1(ir, shl_q);
      if (index_op.is_llocal)
        continue;
      if (base_op.is_llocal || base_op.is_lval)
        continue;

      if (!ir_xform_same_block(ir, shl_idx, i))
        continue;
    } else {
      /* Constant displacement: base + #imm  ->  LOAD_INDEXED [base, #imm] (scale 0).
       * Mirrors the standalone disp-fusion pass, but here the load is a deref
       * embedded as an arithmetic operand (e.g. `t <- *(p + 4) & 1`), so no
       * explicit LOAD op exists for that pass to rewrite.  Folds the address
       * ADD into the load's addressing mode.  This is the dominant cost in
       * unrolled element-wise code (GCC `vector_size` ops, struct copies). */
      if (!tcc_state->opt_disp_fusion)
        continue;

      int imm_disp;
      if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG &&
          irop_has_vreg(add_src1)) {
        base_op = add_src1;
        imm_disp = (int)add_src2.u.imm32;
      } else if (irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG &&
                 irop_has_vreg(add_src2)) {
        base_op = add_src2;
        imm_disp = (int)add_src1.u.imm32;
      } else {
        continue;
      }

      /* Thumb-2 ldr/str displacement range (positive imm12 / negative imm8). */
      if (imm_disp > 4095 || imm_disp < -255)
        continue;
      if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
        continue;

      {
        int access_btype = deref_op.btype;
        if (access_btype == IROP_BTYPE_INT64 || access_btype == IROP_BTYPE_FLOAT64 ||
            access_btype == IROP_BTYPE_STRUCT)
          continue;
      }

      if (!ir_xform_same_block(ir, add_idx, i))
        continue;

      /* base must not be redefined between the ADD and this use, else the fused
       * [base, #imm] would recompute from a stale base. */
      {
        int32_t base_vr = irop_get_vreg(base_op);
        int redef = 0;
        for (int j = add_idx + 1; j < i; j++) {
          IRQuadCompact *bq = &ir->compact_instructions[j];
          IROperand bd = tcc_ir_op_get_dest(ir, bq);
          if (irop_has_vreg(bd) && irop_get_vreg(bd) == base_vr) {
            redef = 1;
            break;
          }
        }
        if (redef)
          continue;
      }

      index_op = irop_make_imm32(0, imm_disp, IROP_BTYPE_INT32);
      scale_amount = 0;
    }

    int32_t loaded_vr = tcc_ir_vreg_alloc_temp(ir);
    if (loaded_vr < 0)
      continue;
    /* Grow the operand pool rather than bailing when full: large unrolled
     * element-wise code (GCC vector_size ops) folds dozens of derefs and would
     * otherwise leave the later ones as `add;ldr` once the pool hit capacity. */
    tcc_ir_pool_ensure(ir, 4);

    int new_base_idx = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    IROperand loaded_op = irop_make_vreg(loaded_vr, deref_op.btype ? deref_op.btype : IROP_BTYPE_INT32);
    if (shl_idx < 0)
      loaded_op.is_unsigned = deref_op.is_unsigned;
    IROperand base_clean = base_op;
    base_clean.is_lval = 0;
    IROperand scale_imm = irop_make_imm32(0, scale_amount, IROP_BTYPE_INT32);

    ir->iroperand_pool[new_base_idx + 0] = loaded_op;
    ir->iroperand_pool[new_base_idx + 1] = base_clean;
    ir->iroperand_pool[new_base_idx + 2] = index_op;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;

    add_q->op = TCCIR_OP_LOAD_INDEXED;
    add_q->operand_base = new_base_idx;
    if (shl_q)
      shl_q->op = TCCIR_OP_NOP;

    IROperand clean_op = loaded_op;
    q = &ir->compact_instructions[i];
    if (src_pos == 1)
      tcc_ir_op_set_src1(ir, q, clean_op);
    else
      tcc_ir_op_set_src2(ir, q, clean_op);

    total_changes++;
  }
  return total_changes;
}

const IROptGen fusion_deref_indexed_gens[] = {
    {-1, ir_gen_deref_indexed_fusion, "deref_indexed_fusion", 1},
};

const int fusion_deref_indexed_gens_count = 1;

static int ir_gen_disp_fusion(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (!tcc_state->opt_disp_fusion)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[i];

  int is_store = 0;
  int is_load = 0;
  IROperand addr_op = IROP_NONE;

  if (q->op == TCCIR_OP_LOAD) {
    is_load = 1;
    addr_op = tcc_ir_op_get_src1(ir, q);
  } else if (q->op == TCCIR_OP_STORE) {
    is_store = 1;
    addr_op = tcc_ir_op_get_dest(ir, q);
  } else if (q->op == TCCIR_OP_ASSIGN) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_lval)
      return 0;
    is_load = 1;
    addr_op = src1;
  } else {
    return 0;
  }

  if (!irop_has_vreg(addr_op))
    return 0;

  int32_t addr_vr = irop_get_vreg(addr_op);

  if (is_load && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
    return 0;

  {
    int access_btype = addr_op.btype;
    if (access_btype == IROP_BTYPE_INT64 || access_btype == IROP_BTYPE_FLOAT64 ||
        access_btype == IROP_BTYPE_STRUCT)
      return 0;
  }

  if (is_load) {
    IROperand dest_op = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest_op);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
  }

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

  IROperand base_op;
  int imm;
  if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG && irop_has_vreg(add_src1)) {
    base_op = add_src1;
    imm = (int)add_src2.u.imm32;
  } else if (irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG &&
             irop_has_vreg(add_src2)) {
    base_op = add_src2;
    imm = (int)add_src1.u.imm32;
  } else {
    return 0;
  }

  if (imm > 4095 || imm < -255)
    return 0;
  /* A deref'd base (`*ptr + imm`) cannot be folded: STORE_INDEXED/LOAD_INDEXED
   * use the base register's value, so stripping is_lval would drop the
   * pointer load (e.g. `s1->ifdef_stack_ptr[-1] = c` storing into the
   * struct field address instead of through the pointer). */
  if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
    return 0;
  if (!ir_xform_same_block(ir, add_idx, i))
    return 0;

  IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
  IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);

  {
    int32_t base_vr = irop_get_vreg(base_op);
    if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP &&
        ir_opt_du_uses(du, base_vr) == 1) {
      int copy_idx = ir_opt_du_def(du, base_vr, add_idx);
      if (copy_idx >= 0) {
        IRQuadCompact *copy_q = &ir->compact_instructions[copy_idx];
        if (copy_q->op == TCCIR_OP_ASSIGN) {
          IROperand copy_dest = tcc_ir_op_get_dest(ir, copy_q);
          IROperand copy_src = tcc_ir_op_get_src1(ir, copy_q);
          if (!copy_dest.is_lval && !copy_src.is_lval && irop_has_vreg(copy_src)) {
            base_op = copy_src;
            copy_q->op = TCCIR_OP_NOP;
          }
        }
      }
    }
  }

  tcc_ir_pool_ensure(ir, 4);
  int new_base_idx = ir->iroperand_pool_count;
  if (new_base_idx + 4 > ir->iroperand_pool_capacity)
    return 0;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);

  IROperand index_imm = irop_make_imm32(0, imm, IROP_BTYPE_INT32);
  IROperand scale_imm = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

  if (is_store) {
    IROperand base_for_store = base_op;
    base_for_store.is_lval = 0;
    ir->iroperand_pool[new_base_idx + 0] = base_for_store;
    ir->iroperand_pool[new_base_idx + 1] = orig_src1;
    ir->iroperand_pool[new_base_idx + 2] = index_imm;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    q->op = TCCIR_OP_STORE_INDEXED;
  } else {
    IROperand base_for_load = base_op;
    base_for_load.is_lval = 0;
    IROperand new_dest = orig_dest;
    if (q->op == TCCIR_OP_ASSIGN) {
      new_dest.btype = addr_op.btype;
      new_dest.is_unsigned = addr_op.is_unsigned;
    }
    ir->iroperand_pool[new_base_idx + 0] = new_dest;
    ir->iroperand_pool[new_base_idx + 1] = base_for_load;
    ir->iroperand_pool[new_base_idx + 2] = index_imm;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    q->op = TCCIR_OP_LOAD_INDEXED;
  }
  q->operand_base = new_base_idx;

  add_q->op = TCCIR_OP_NOP;
  return 1;
}

const IROptGen fusion_disp_gens[] = {
    {TCCIR_OP_LOAD, ir_gen_disp_fusion, "disp_load_fusion", 1},
    {TCCIR_OP_STORE, ir_gen_disp_fusion, "disp_store_fusion", 1},
    {TCCIR_OP_ASSIGN, ir_gen_disp_fusion, "disp_assign_fusion", 1},
};

const int fusion_disp_gens_count = sizeof(fusion_disp_gens) / sizeof(fusion_disp_gens[0]);

static int ir_gen_indexed_chain(IROptCtx *ctx, int i)
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
  if (add_q->op != TCCIR_OP_ADD)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

  IROperand new_base;
  int imm1;
  if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG && irop_has_vreg(add_src1)) {
    new_base = add_src1; imm1 = (int)add_src2.u.imm32;
  } else if (irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG && irop_has_vreg(add_src2)) {
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
  ir->iroperand_pool[q->operand_base + base_slot] = new_base;
  ir->iroperand_pool[q->operand_base + 2] = irop_make_imm32(0, (int32_t)imm_total, IROP_BTYPE_INT32);

  add_q->op = TCCIR_OP_NOP;
  return 1;
}

const IROptGen fusion_chain_gens[] = {
    {TCCIR_OP_LOAD_INDEXED, ir_gen_indexed_chain, "indexed_chain_load", 1},
    {TCCIR_OP_STORE_INDEXED, ir_gen_indexed_chain, "indexed_chain_store", 1},
};

const int fusion_chain_gens_count = sizeof(fusion_chain_gens) / sizeof(fusion_chain_gens[0]);

static int ir_gen_indexed_pair_reorder(IROptCtx *ctx, int i)
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
      if (irop_config[pq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, pq)) == q3_dv_vr)
        conflict = 1;
      if (!conflict && irop_config[pq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, pq)) == q3_dv_vr)
        conflict = 1;
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
    {TCCIR_OP_LOAD_INDEXED, ir_gen_indexed_pair_reorder, "pair_reorder_load", 0},
    {TCCIR_OP_STORE_INDEXED, ir_gen_indexed_pair_reorder, "pair_reorder_store", 0},
};

const int fusion_pair_reorder_gens_count = sizeof(fusion_pair_reorder_gens) / sizeof(fusion_pair_reorder_gens[0]);
