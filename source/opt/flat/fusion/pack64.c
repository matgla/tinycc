/*
 *  TCC IR - 64-bit Register Pair Optimization
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
#include "opt_utils.h"


int tcc_ir_opt_pack64(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 4)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_OR)
      continue;
    IROperand or_dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_btype(or_dest) != IROP_BTYPE_INT64)
      continue;
    if (or_dest.is_lval)
      continue;

    IROperand or_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand or_src2 = tcc_ir_op_get_src2(ir, q);

    for (int swap = 0; swap < 2; swap++)
    {
      IROperand shl_op = swap ? or_src2 : or_src1;
      IROperand zl_op = swap ? or_src1 : or_src2;

      int32_t shl_vr = irop_get_vreg(shl_op);
      int32_t zl_vr = irop_get_vreg(zl_op);
      if (TCCIR_DECODE_VREG_TYPE(shl_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(zl_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (ir_opt_du_uses(&du, shl_vr) != 1 || ir_opt_du_uses(&du, zl_vr) != 1)
        continue;
      if (!ir_opt_du_is_single_def(&du, shl_vr) || !ir_opt_du_is_single_def(&du, zl_vr))
        continue;
      int shl_def = ir_opt_du_def(&du, shl_vr, n);
      int zl_def = ir_opt_du_def(&du, zl_vr, n);
      if (shl_def < 0 || zl_def < 0)
        continue;

      IRQuadCompact *shl_q = &ir->compact_instructions[shl_def];
      if (shl_q->op != TCCIR_OP_SHL)
        continue;
      IROperand shl_amt = tcc_ir_op_get_src2(ir, shl_q);
      if (!irop_is_immediate(shl_amt) || irop_get_imm64_ex(ir, shl_amt) != 32)
        continue;
      IROperand shl_input = tcc_ir_op_get_src1(ir, shl_q);
      int32_t shl_input_vr = irop_get_vreg(shl_input);
      if (TCCIR_DECODE_VREG_TYPE(shl_input_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (ir_opt_du_uses(&du, shl_input_vr) != 1 || !ir_opt_du_is_single_def(&du, shl_input_vr))
        continue;
      int zh_def = ir_opt_du_def(&du, shl_input_vr, n);
      if (zh_def < 0)
        continue;

      IRQuadCompact *zh_q = &ir->compact_instructions[zh_def];
      IRQuadCompact *zl_q = &ir->compact_instructions[zl_def];
      if (zh_q->op != TCCIR_OP_ZEXT || zl_q->op != TCCIR_OP_ZEXT)
        continue;

      IROperand src_hi = tcc_ir_op_get_src1(ir, zh_q);
      IROperand src_lo = tcc_ir_op_get_src1(ir, zl_q);

      LOG_IR_GEN("OPTIMIZE: PACK64 fold at i=%d (zh=%d, sh=%d, zl=%d)", i,
                 zh_def, shl_def, zl_def);

      q->op = TCCIR_OP_PACK64;
      tcc_ir_set_src1(ir, i, src_lo);
      tcc_ir_set_src2(ir, i, src_hi);

      ir->compact_instructions[zh_def].op = TCCIR_OP_NOP;
      ir->compact_instructions[shl_def].op = TCCIR_OP_NOP;
      ir->compact_instructions[zl_def].op = TCCIR_OP_NOP;
      changes++;
      break;
    }
  }

  tcc_free(du.def);
  return changes;
}

/* Recognise two adjacent narrow stack STOREs (lo@[A], hi@[A+4]) feeding an
 * INT64 LOAD of [A] and rewrite the LOAD as PACK64(val_lo, val_hi) (ARM long
 * long / 8-byte aggregate param prologues).  When T feeds a consumer that
 * takes the register pair, PACK64 codegens to no-op MOVs.
 *
 * Safety: scans only the LOAD's straight-line region (stops at any jump
 * target / control-flow op); bails on any unrecognised intervening op. */
int tcc_ir_opt_pack64_from_stack_stores(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_btype(dest) != IROP_BTYPE_INT64)
      continue;
    if (dest.is_lval)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (src.tag != IROP_TAG_STACKOFF || !src.is_local || !src.is_lval)
      continue;
    /* Must be a plain stack-slot read, not a deref through a vreg/sym. */
    if (src.is_llocal || src.is_sym)
      continue;
    /* A STACKOFF is a *direct* slot read only when it has no vreg (get_vreg
     * == -1).  A VAR/PARAM spill encoding also has tag==STACKOFF/is_local/
     * is_lval, but its offset is only "where it would spill" — the value comes
     * from the vreg.  Matching STOREs by that phantom offset can grab an
     * unrelated variable's stores when the slot was reused. */
    if (irop_get_vreg(src) != -1)
      continue;

    int64_t addr_lo = irop_get_imm64_ex(ir, src);
    int64_t addr_hi = addr_lo + 4;

    /* Search backwards for the two narrow STOREs covering the 8-byte LOAD. */
    int lo_idx = -1, hi_idx = -1;
    IROperand lo_val = IROP_NONE, hi_val = IROP_NONE;

    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->is_jump_target)
        break;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
          jq->op == TCCIR_OP_SWITCH_TABLE)
        break;
      /* Calls and inline asm can clobber arbitrary memory — bail. */
      if (jq->op == TCCIR_OP_FUNCCALLVAL || jq->op == TCCIR_OP_FUNCCALLVOID ||
          jq->op == TCCIR_OP_INLINE_ASM || jq->op == TCCIR_OP_ASM_INPUT ||
          jq->op == TCCIR_OP_ASM_OUTPUT || jq->op == TCCIR_OP_VLA_ALLOC ||
          jq->op == TCCIR_OP_SETJMP || jq->op == TCCIR_OP_LONGJMP ||
          jq->op == TCCIR_OP_NL_SETJMP || jq->op == TCCIR_OP_NL_LONGJMP)
        break;

      /* Watch for any STORE to a stack slot overlapping [addr_lo, addr_lo+8). */
      if (jq->op == TCCIR_OP_STORE || jq->op == TCCIR_OP_STORE_INDEXED ||
          jq->op == TCCIR_OP_STORE_POSTINC || jq->op == TCCIR_OP_BLOCK_COPY)
      {
        IROperand jdst = tcc_ir_op_get_dest(ir, jq);
        if (jq->op == TCCIR_OP_STORE && jdst.tag == IROP_TAG_STACKOFF && jdst.is_local && jdst.is_lval &&
            !jdst.is_llocal && !jdst.is_sym && irop_get_vreg(jdst) == -1 && irop_get_btype(jdst) == IROP_BTYPE_INT32)
        {
          int64_t joff = irop_get_imm64_ex(ir, jdst);
          IROperand jsrc = tcc_ir_op_get_src1(ir, jq);
          if (joff == addr_lo && lo_idx < 0)
          {
            lo_idx = j;
            lo_val = jsrc;
            if (hi_idx >= 0) break;
            continue;
          }
          if (joff == addr_hi && hi_idx < 0)
          {
            hi_idx = j;
            hi_val = jsrc;
            if (lo_idx >= 0) break;
            continue;
          }
          /* STORE to some unrelated stack slot — fine to look past. */
          continue;
        }
        /* Indirect / wider / cross-form store — could alias [addr_lo,+8); bail. */
        break;
      }
    }

    if (lo_idx < 0 || hi_idx < 0)
      continue;

    /* Stored values must be 32-bit and non-lvalue (IMM32 ok). */
    if (lo_val.is_lval || hi_val.is_lval)
      continue;
    if (irop_get_btype(lo_val) != IROP_BTYPE_INT32 || irop_get_btype(hi_val) != IROP_BTYPE_INT32)
      continue;

    /* A vreg half must not be redefined between its STORE and the LOAD, so
     * the value stored is still the one the LOAD observes. */
    int latest_store = lo_idx > hi_idx ? lo_idx : hi_idx;
    int redef = 0;
    int32_t lo_vr = irop_has_vreg(lo_val) ? irop_get_vreg(lo_val) : -1;
    int32_t hi_vr = irop_has_vreg(hi_val) ? irop_get_vreg(hi_val) : -1;
    if (lo_vr >= 0 || hi_vr >= 0)
    {
      for (int k = latest_store + 1; k < i; k++)
      {
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[kq->op].has_dest)
          continue;
        if (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
            kq->op == TCCIR_OP_STORE_POSTINC)
          continue; /* memory store, doesn't redefine vregs */
        IROperand kd = tcc_ir_op_get_dest(ir, kq);
        if (kd.is_lval)
          continue;
        int32_t kdvr = irop_get_vreg(kd);
        if (kdvr < 0)
          continue;
        if (kdvr == lo_vr || kdvr == hi_vr)
        {
          redef = 1;
          break;
        }
      }
    }
    if (redef)
      continue;

    LOG_IR_GEN("OPTIMIZE: PACK64-FROM-STORES @i=%d (lo@%d, hi@%d, addr=%lld)", i, lo_idx, hi_idx, (long long)addr_lo);

    /* LOAD has 2 operand slots but PACK64 needs 3; reusing operand_base would
     * overflow the src2 write into the next instruction's slot, so allocate
     * fresh slots at the pool tail and re-point operand_base. */
    tcc_ir_pool_ensure(ir, 3);
    int new_base = ir->iroperand_pool_count;
    if (new_base + 3 > ir->iroperand_pool_capacity)
      continue;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    ir->iroperand_pool[new_base + 0] = dest;
    ir->iroperand_pool[new_base + 1] = lo_val;
    ir->iroperand_pool[new_base + 2] = hi_val;
    q->operand_base = new_base;
    q->op = TCCIR_OP_PACK64;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_pack64_from_stack_stores_ex(IROptCtx *ctx) { return tcc_ir_opt_pack64_from_stack_stores(ctx->ir); }

/* Return the common immediate written by every STORE/ASSIGN to var_vr, or
 * fail if the writers disagree or any is non-immediate. */
static int pack64_find_var_const_value(TCCIRState *ir, int n, int before_idx,
                                       int32_t var_vr, int64_t *out_imm)
{
  int have_value = 0;
  int64_t value = 0;
  for (int i = 0; i < before_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) != var_vr)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(s))
      return 0;
    int64_t v = irop_get_imm64_ex(ir, s);
    if (have_value && v != value)
      return 0;
    value = v;
    have_value = 1;
  }
  if (!have_value)
    return 0;
  *out_imm = value;
  return 1;
}

/* Walk the def-chain backwards for a compile-time constant (ASSIGN/LOAD
 * copies, plus SHL/SAR/SHR by an immediate; a VAR vreg resolves if every
 * store to it writes the same literal).  Returns 1 with *out set on success.
 *
 * Used only as a guard in pack64_implicit: if both OR halves resolve to
 * constants, const_prop would fold the SHL+OR to a literal, but PACK64 is
 * opaque to const_prop and would forfeit that fold.  Over-approximation is
 * safe — a wrong "yes" only skips a legitimate fold, never miscompiles. */
static int pack64_operand_resolves_const(TCCIRState *ir, IROptDU *du, int n,
                                         IROperand op, int boundary_idx,
                                         int budget, int64_t *out)
{
  for (int hop = 0; hop < 16 && budget > 0; hop++, budget--)
  {
    if (irop_is_immediate(op))
    {
      *out = irop_get_imm64_ex(ir, op);
      return 1;
    }
    int32_t vr = irop_get_vreg(op);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    if (!ir_opt_du_is_single_def(du, vr))
      return 0;
    int def_idx = ir_opt_du_def(du, vr, n);
    if (def_idx < 0)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[def_idx];
    if (dq->op == TCCIR_OP_SHL || dq->op == TCCIR_OP_SAR || dq->op == TCCIR_OP_SHR)
    {
      IROperand sh_amt = tcc_ir_op_get_src2(ir, dq);
      if (!irop_is_immediate(sh_amt))
        return 0;
      int64_t amt = irop_get_imm64_ex(ir, sh_amt);
      int64_t v;
      if (!pack64_operand_resolves_const(ir, du, n, tcc_ir_op_get_src1(ir, dq),
                                         boundary_idx, budget - 1, &v))
        return 0;
      int is_64 = (irop_get_btype(tcc_ir_op_get_dest(ir, dq)) == IROP_BTYPE_INT64);
      int mask = is_64 ? 63 : 31;
      int64_t r;
      if (dq->op == TCCIR_OP_SHL)
        r = (int64_t)((uint64_t)v << (amt & mask));
      else if (dq->op == TCCIR_OP_SHR)
        r = is_64 ? (int64_t)((uint64_t)v >> (amt & 63)) : (int64_t)((uint32_t)v >> (amt & 31));
      else
        r = is_64 ? v >> (amt & 63) : (int64_t)((int32_t)v >> (amt & 31));
      if (!is_64)
        r = (int64_t)(int32_t)(uint32_t)r;
      *out = r;
      return 1;
    }
    if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LOAD)
      return 0;
    IROperand ds = tcc_ir_op_get_src1(ir, dq);
    int32_t ds_vr = irop_get_vreg(ds);
    if (ds_vr >= 0 && TCCIR_DECODE_VREG_TYPE(ds_vr) == TCCIR_VREG_TYPE_VAR)
    {
      /* Bound the write scan by def_idx — later writes can't affect it. */
      int64_t var_imm = 0;
      if (!pack64_find_var_const_value(ir, n, def_idx, ds_vr, &var_imm))
        return 0;
      *out = var_imm;
      return 1;
    }
    if (ds.is_lval || ds.is_local || ds.is_llocal)
      return 0;
    op = ds;
  }
  return 0;
}

/* Fold the widening idiom `(X_hi SHL #32) OR X_lo` (both i32) into
 * PACK64(X_lo, X_hi): the OR's i32 operand is implicitly zero-extended so it
 * only contributes the low half.  Sister of tcc_ir_opt_pack64, which requires
 * explicit ZEXT defs; this catches the same shape without them. */
int tcc_ir_opt_pack64_implicit(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_OR)
      continue;
    IROperand or_dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_btype(or_dest) != IROP_BTYPE_INT64)
      continue;

    IROperand or_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand or_src2 = tcc_ir_op_get_src2(ir, q);

    for (int swap = 0; swap < 2; swap++)
    {
      IROperand shl_op = swap ? or_src2 : or_src1;
      IROperand lo_op = swap ? or_src1 : or_src2;

      int32_t shl_vr = irop_get_vreg(shl_op);
      if (TCCIR_DECODE_VREG_TYPE(shl_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (shl_op.is_lval || shl_op.is_sym)
        continue;
      if (ir_opt_du_uses(&du, shl_vr) != 1 || !ir_opt_du_is_single_def(&du, shl_vr))
        continue;
      int shl_def = ir_opt_du_def(&du, shl_vr, n);
      if (shl_def < 0)
        continue;

      IRQuadCompact *shl_q = &ir->compact_instructions[shl_def];
      if (shl_q->op != TCCIR_OP_SHL)
        continue;
      IROperand shl_amt = tcc_ir_op_get_src2(ir, shl_q);
      if (!irop_is_immediate(shl_amt) || irop_get_imm64_ex(ir, shl_amt) != 32)
        continue;
      IROperand shl_dest = tcc_ir_op_get_dest(ir, shl_q);
      if (irop_get_btype(shl_dest) != IROP_BTYPE_INT64)
        continue;

      /* SHL input becomes PACK64's hi; must be 32-bit or bits above bit 31
       * would survive PACK64's implicit truncation of the hi half. */
      IROperand shl_input = tcc_ir_op_get_src1(ir, shl_q);
      if (irop_get_btype(shl_input) == IROP_BTYPE_INT64)
        continue;

      /* Other OR operand becomes lo; must be 32-bit so its zero-extension is
       * hi=0 and doesn't corrupt the packed high half. */
      if (irop_get_btype(lo_op) == IROP_BTYPE_INT64)
        continue;

      /* Skip when both halves are constants: const_prop would fold the SHL+OR
       * to a literal, but PACK64 is opaque to it. */
      {
        int64_t dummy;
        if (pack64_operand_resolves_const(ir, &du, n, shl_input, i, 32, &dummy) &&
            pack64_operand_resolves_const(ir, &du, n, lo_op, i, 32, &dummy))
          continue;
      }

      LOG_IR_GEN("OPTIMIZE: PACK64_IMPLICIT fold at i=%d (shl_def=%d)", i, shl_def);

      q->op = TCCIR_OP_PACK64;
      tcc_ir_set_src1(ir, i, lo_op);
      tcc_ir_set_src2(ir, i, shl_input);

      ir->compact_instructions[shl_def].op = TCCIR_OP_NOP;
      changes++;
      break;
    }
  }

  tcc_free(du.def);
  return changes;
}
int tcc_ir_opt_pack64_ex(IROptCtx *ctx) { return tcc_ir_opt_pack64(ctx->ir); }
