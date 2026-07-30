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

  /* Memory-derived inputs create stale-value paths after copy/const prop once
   * the three-source expression is hidden behind one MLA; leave those as
   * separate MUL+ADD. */
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

  /* The MLA lands at the MUL's position, hoisting the accumulator read up to
   * it: a memory-read accumulator must not skip stores between the MUL and the
   * ADD. */
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
  /* 64-bit MLA lowers only to SMLAL/UMLAL, which accumulate in place: the dest
   * pair must equal the accumulator pair, reached only when the result stores
   * straight back to the accumulator's slot (store_idx >= 0).  Otherwise
   * final_dest is a fresh temp that cannot be lowered; leave as SMULL/UMULL +
   * 64-bit ADD. */
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

  /* MLA needs four operands but the MUL allocated three; growing in place would
   * overwrite neighbouring operand blocks, so move to a fresh 4-slot region. */
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
    /* Unscaled register index: base + index (scale 0), folded into
     * LDR/STR Rt,[Rn,Rm].  Operands are either two plain registers or a
     * global address plus a register — the scaled path already admits a
     * SYMREF base and the backend materializes it, so the byte/short case
     * must not be stricter (it was, which kept `for (i) s += gv[i]` over a
     * global as ADD+DEREF and thereby blocked loop rotation's indirect-lvalue
     * guard).  A constant operand is the displacement case handled
     * elsewhere. */
    if (add_idx >= i)
      return 0;
    int src1_sym = irop_get_tag(add_src1) == IROP_TAG_SYMREF;
    int src2_sym = irop_get_tag(add_src2) == IROP_TAG_SYMREF;
    if (src1_sym && src2_sym)
      return 0;
    if (src1_sym || src2_sym) {
      base_op = src1_sym ? add_src1 : add_src2;
      index_op = src1_sym ? add_src2 : add_src1;
      if (!irop_has_vreg(index_op) || index_op.is_const)
        return 0;
    } else {
      if (!irop_has_vreg(add_src1) || !irop_has_vreg(add_src2))
        return 0;
      if (add_src1.is_const || add_src2.is_const)
        return 0;
      base_op = add_src1;
      index_op = add_src2;
    }
    shift_amount = 0;

    /* Index may be a plain register or a stack-local lvalue.  Reject
     * double-indirection (is_llocal) and bare pointer lvalues (is_lval without
     * is_local), which need an extra dereference the index slot cannot express;
     * base must be a plain pointer register. */
    if (index_op.is_llocal || (index_op.is_lval && !index_op.is_local))
      return 0;
    if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
      return 0;

    /* The ADD must reach the memory op with no intervening control flow and
     * no redefinition of either address component, so the fused load/store
     * recomputes the same effective address.  A SYMREF base is a constant
     * address — only the register components can be redefined. */
    int32_t base_vr = irop_has_vreg(base_op) ? irop_get_vreg(base_op) : -1;
    int32_t index_vr = irop_get_vreg(index_op);
    for (int j = add_idx + 1; j < i; j++) {
      IRQuadCompact *bq = &ir->compact_instructions[j];
      if (bq->op == TCCIR_OP_JUMP || bq->op == TCCIR_OP_JUMPIF || bq->op == TCCIR_OP_NOP)
        return 0;
      IROperand bd = tcc_ir_op_get_dest(ir, bq);
      if (irop_has_vreg(bd)) {
        int32_t dvr = irop_get_vreg(bd);
        if ((base_vr >= 0 && dvr == base_vr) || dvr == index_vr)
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
  /* Transfer the packed-access mark from the deref operand being replaced
   * (LOAD: src1, STORE: dest) onto the base: the backend's 64-bit indexed
   * lowering uses LDRD/STRD, which fault on the unaligned addresses a packed
   * member chain can produce. */
  base_op_clean.aux |= (is_store ? orig_dest.aux : orig_src1.aux) & IROP_AUX_UNDERALIGN;
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

  /* CMP is intentionally excluded: folding a deref into a CMP rewrites the read
   * as a LOAD_INDEXED that downstream DSE/alias fails to see as a use and can
   * delete the producing stores (miscompiles gcc-torture loop-11). */
  if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_LOAD_INDEXED ||
      q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
      q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
      q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE ||
      q->op == TCCIR_OP_RETURNVOID)
    return 0;

  int operand_positions[2] = {0, 0};
  int num_deref = 0;

  /* Only a genuine pointer dereference (address in a register/temp) can be
   * folded into a load's addressing mode.  An is_local/is_llocal lvalue reads a
   * stack variable's value, not an address, so folding would corrupt it. */
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
      /* Constant displacement: base + #imm -> LOAD_INDEXED [base, #imm].  Here
       * the load is a deref embedded as an arithmetic operand (e.g. `*(p+4) & 1`),
       * so no explicit LOAD op exists for the standalone disp-fusion pass. */
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
    /* Grow the pool rather than bail when full, so large unrolled code doesn't
     * leave later derefs unfolded once capacity is hit. */
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
    /* Same packed-access transfer as in ir_gen_indexed_memory_fusion. */
    base_clean.aux |= deref_op.aux & IROP_AUX_UNDERALIGN;
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
