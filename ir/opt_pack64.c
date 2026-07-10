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

/* tcc_ir_opt_pack64_from_stack_stores:
 *
 * Recognise the pattern produced by ARM param prologues for long long /
 * 8-byte aggregate returns:
 *
 *   StackLoc[A]   <-- val_lo    [INT32 STORE]   ; param prologue spill (lo)
 *   StackLoc[A+4] <-- val_hi    [INT32 STORE]   ; param prologue spill (hi)
 *   ...   (no intervening writes/reads to [A,A+8) and no redef of val_lo/hi)
 *   T (INT64)     <-- StackLoc[A] [LOAD]         ; e.g. `return x;` in test2
 *
 * Rewrite the LOAD as:
 *
 *   T (INT64)     <-- PACK64(val_lo, val_hi)
 *
 * If T is the destination of an immediately-following RETURNVALUE (or any
 * other consumer that can take the register pair), the PACK64 codegen
 * degrades to register-aligned no-op MOVs, eliminating the spill+ldrd.
 *
 * Safety: scans linearly within the LOAD's owning straight-line region
 * (stops at any jump target / control-flow op).  Conservative — bail on
 * any unrecognised op between the stores and the LOAD. */
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
    /* The LOAD source must be a plain stack slot read (not a deref through
     * a vreg / sym).  Bail if the operand has any kind of indirection. */
    if (src.is_llocal || src.is_sym)
      continue;
    /* CRITICAL: a STACKOFF operand is only a *direct* stack-slot read when it
     * has no associated vreg (vreg_type == 0, i.e. irop_get_vreg == -1).  A VAR
     * or PARAM referenced through its potential spill encoding also has
     * tag==STACKOFF/is_local/is_lval, but its offset is mere "where it would
     * spill" metadata — the value is actually read from the vreg, not that slot
     * (see the IROP_TAG_STACKOFF note in tccir_operand.h).  Matching STOREs by
     * that phantom offset can grab an unrelated variable's stores when the slot
     * was reused (longlong fuzz seed 7: a u64 local whose spill home aliased an
     * array's live slot got folded to PACK64 of the array's elements). */
    if (irop_get_vreg(src) != -1)
      continue;

    int64_t addr_lo = irop_get_imm64_ex(ir, src);
    int64_t addr_hi = addr_lo + 4;

    /* Search backwards in the same straight-line region for the two
     * adjacent narrow STOREs that cover the 8-byte LOAD. */
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

    /* The two stored values must be 32-bit vregs (not lvalues, not constants
     * that const-fold would have already merged).  Allow IMM32 too — PACK64
     * is happy with either.  Reject lvalues. */
    if (lo_val.is_lval || hi_val.is_lval)
      continue;
    if (irop_get_btype(lo_val) != IROP_BTYPE_INT32 || irop_get_btype(hi_val) != IROP_BTYPE_INT32)
      continue;

    /* If lo_val or hi_val is a vreg, ensure it's not redefined between its
     * STORE and the LOAD (i.e. the vreg's value at the STORE is still
     * available at the LOAD).  Sub-i values not generated here. */
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

    /* LOAD has 2 operand slots (dest, src1); PACK64 needs 3 (dest, src1,
     * src2).  Reusing the existing operand_base would let the src2 write
     * overflow into the next instruction's dest slot.  Allocate fresh
     * slots at the pool tail and re-point operand_base. */
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

/* Look at all STOREs/ASSIGNs that write the given VAR-vreg and return their
 * common immediate value, or fail.  All writers must agree on the same
 * literal value for the LOAD's result to be statically known. */
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

/* Walk the def-chain backwards looking for a compile-time constant.  Handles
 * ASSIGN/LOAD copies, plus SHL/SAR/SHR with immediate shift amounts so the
 * frequent `(int8_t)x; ((int64_t)int_var)` cast chain folds.  When the chain
 * hits a VAR vreg (a local variable read via the spill slot), check if every
 * store to that VAR writes the same literal — if so, that's the value.
 *
 * Returns 1 with *out set on success; 0 otherwise.  Used as a *guard* in
 * pack64_implicit: if both halves of the OR resolve to constants, the
 * pre-PACK64 chain would const-fold to a literal, but PACK64 itself is
 * opaque to const_prop and the conversion forfeits that fold (this
 * regressed gcc.c-torture/compile/20040304-2.c from 1 to ~100 instructions).
 *
 * Used only as a guard — over-approximation (false negatives) only costs
 * missed pack64_implicit applications, never miscompiles.  Bug-tolerant by
 * design: if the walker incorrectly concludes "yes constant", we skip a
 * legitimate fold but the SHL+OR stays semantically equivalent.
 */
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
      /* Bound the write scan by the LOAD's own index — writes after it
       * have no bearing on the value it observed. */
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

/* tcc_ir_opt_pack64_implicit: fold the C-level signed/unsigned widening idiom
 * that lacks explicit ZEXT operations.
 *
 *   T_sh = X_hi SHL #32       ; i64 — X_hi is i32 (e.g. result of `X SAR #31`)
 *   T_or = T_sh OR X_lo       ; i64 — X_lo is i32 (the original low value)
 *
 * The OR's i32 operand is implicitly zero-extended to i64, so its high
 * contribution is 0.  T_sh contributes hi=X_hi, lo=0.  Combined: lo=X_lo,
 * hi=X_hi — exactly what PACK64 represents.
 *
 * Sister of [[tcc_ir_opt_pack64]]: that pass requires explicit ZEXT defs on
 * both halves; this one catches the same idiom when the frontend emitted the
 * SAR/SHL/OR chain without intermediate ZEXTs (the typical shape for
 * `arr[N] = (long long)int_var`).
 */
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

      /* The SHL operand must be a single-use TEMP. */
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

      /* The SHL's input becomes PACK64's hi operand.  It must be a 32-bit
       * value (otherwise the bits above bit 31 would survive the implicit
       * truncation that PACK64 performs on the hi half). */
      IROperand shl_input = tcc_ir_op_get_src1(ir, shl_q);
      if (irop_get_btype(shl_input) == IROP_BTYPE_INT64)
        continue;

      /* The other OR operand becomes PACK64's lo. It must also be 32-bit so
       * its implicit zero-extension into the i64 OR is hi=0 — otherwise the
       * non-zero hi bits would corrupt the packed high half. */
      if (irop_get_btype(lo_op) == IROP_BTYPE_INT64)
        continue;

      /* Skip when both halves trace to compile-time constants — const_prop
       * folds the original SHL+OR chain to a literal in that case, but
       * PACK64 is opaque to const_prop and the conversion forfeits that
       * fold (regresses gcc.c-torture/compile/20040304-2.c from 1 to
       * ~100 instructions, where ternary chains over a 0 tempA/tempB
       * resolve to a no-op function). */
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

/* tcc_ir_opt_pack64_tautology: fold `PACK64(low_half(X), X SHR #32)` into
 * `ASSIGN X`.  Recognises the case where C source code packs a 64-bit value
 * back together from its own halves — e.g.
 *
 *   uint64_t x = ((uint64_t)(v >> 32) << 32) | (uint32_t)v;   // == v
 *
 * After tcc_ir_opt_pack64 has produced a PACK64, the two source operands
 * are TEMPs whose defs reach back through ASSIGN/LOAD copies (and possibly
 * VAR ASSIGN steps for non-volatile locals) to:
 *
 *   T_lo's chain root: X [ASSIGN/LOAD]     ; u64 X, narrowing read
 *   T_hi's chain root: X SHR #32           ; u64 X, high half
 *   T_pk = T_lo PACK64 T_hi                 ; u64 dest
 *
 * Both halves reference the same u64 vreg X, so the pack is the identity.
 * Rewrite the PACK64 to `T_pk = X [ASSIGN]` and let downstream copy-prop +
 * identity-CMP folding eliminate any subsequent compare against X.
 */

/* Follow ASSIGN/LOAD copy chains starting from a vreg.  Returns the index
 * of the first defining instruction that is NOT a pure pass-through copy,
 * or -1 if the chain is ambiguous / hits a multiply-defined slot.
 *
 * The src1 of a "pure copy" is also a vreg; we continue tracing.  A copy
 * whose src1 is a constant/symbol stops the trace at that copy. */
/* prev_didx is the index of the last copy op we passed through; it is the
 * "chain endpoint" returned when the next vreg has no def (e.g. a PARAM). */
static int p64taut_trace_back(TCCIRState *ir, int *temp_def_idx, int max_temp_pos,
                              int *var_def_idx, int max_var_pos,
                              int32_t vreg)
{
  int prev_didx = -1;
  for (int hops = 0; hops < 32; hops++)
  {
    int type = TCCIR_DECODE_VREG_TYPE(vreg);
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    int didx = -1;
    if (type == TCCIR_VREG_TYPE_TEMP)
    {
      if (pos > max_temp_pos)
        return prev_didx;
      didx = temp_def_idx[pos];
    }
    else if (type == TCCIR_VREG_TYPE_VAR)
    {
      if (pos > max_var_pos)
        return prev_didx;
      didx = var_def_idx[pos];
    }
    else
    {
      /* PARAM or other — has no IR-defining op.  Stop and return the
       * previous copy index so the caller can see "this copy reads <vreg>". */
      return prev_didx;
    }
    if (didx < 0)
      return prev_didx;
    IRQuadCompact *q = &ir->compact_instructions[didx];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      return didx; /* hit a real producing op */
    /* Pure copy — try to continue through src1 if it's a "value reference"
     * (VAR storage read or TEMP value).  Anything else (deref of a
     * computed pointer, symbol deref, immediate) is a real memory access
     * we must not trace through. */
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t src_vr = irop_get_vreg(src1);
    int src_tag = irop_get_tag(src1);
    int is_value_copy = (src_vr >= 0) &&
                        ((src_tag == IROP_TAG_VREG && !src1.is_lval) ||
                         (src_tag == IROP_TAG_STACKOFF && src1.is_lval));
    if (!is_value_copy)
      return prev_didx >= 0 ? prev_didx : didx;
    prev_didx = didx;
    vreg = src_vr;
  }
  return -1;
}

int tcc_ir_opt_pack64_tautology(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  int max_temp_pos = 0, max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    int t = TCCIR_DECODE_VREG_TYPE(vr);
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (t == TCCIR_VREG_TYPE_TEMP && p > max_temp_pos)
      max_temp_pos = p;
    else if (t == TCCIR_VREG_TYPE_VAR && p > max_var_pos)
      max_var_pos = p;
  }

  int temp_stride = max_temp_pos + 1;
  int var_stride = max_var_pos + 1;
  int *temp_def_idx = tcc_malloc(temp_stride * sizeof(int));
  int *var_def_idx = tcc_malloc(var_stride * sizeof(int));
  uint16_t *temp_use_count = tcc_mallocz(temp_stride * sizeof(uint16_t));
  for (int i = 0; i < temp_stride; i++)
    temp_def_idx[i] = -1;
  for (int i = 0; i < var_stride; i++)
    var_def_idx[i] = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_temp_pos && temp_use_count[pos] < 0xFFFF)
          temp_use_count[pos]++;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_temp_pos && temp_use_count[pos] < 0xFFFF)
          temp_use_count[pos]++;
      }
    }
    if (irop_config[q->op].has_dest)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      /* STORE-like ops use dest as an address sink, not a vreg def. */
      int is_real_def = (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
                        q->op != TCCIR_OP_FUNCPARAMVAL);
      if (is_real_def)
      {
        int *tbl = NULL;
        int max_pos = -1;
        if (t == TCCIR_VREG_TYPE_TEMP) { tbl = temp_def_idx; max_pos = max_temp_pos; }
        else if (t == TCCIR_VREG_TYPE_VAR) { tbl = var_def_idx; max_pos = max_var_pos; }
        if (tbl && pos <= max_pos)
        {
          if (tbl[pos] >= 0)
            tbl[pos] = -2; /* multiply-defined */
          else
            tbl[pos] = i;
        }
      }
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_PACK64)
      continue;
    IROperand pk_dest = tcc_ir_op_get_dest(ir, q);
    if (pk_dest.is_lval)
      continue;

    IROperand lo_op = tcc_ir_op_get_src1(ir, q);
    IROperand hi_op = tcc_ir_op_get_src2(ir, q);
    int32_t lo_vr = irop_get_vreg(lo_op);
    int32_t hi_vr = irop_get_vreg(hi_op);
    if (lo_vr < 0 || hi_vr < 0)
      continue;

    /* Trace lo and hi back through ASSIGN/LOAD copy chains. */
    int lo_def_i = p64taut_trace_back(ir, temp_def_idx, max_temp_pos, var_def_idx, max_var_pos, lo_vr);
    int hi_def_i = p64taut_trace_back(ir, temp_def_idx, max_temp_pos, var_def_idx, max_var_pos, hi_vr);
    if (lo_def_i < 0 || hi_def_i < 0)
      continue;

    IRQuadCompact *lo_def = &ir->compact_instructions[lo_def_i];
    IRQuadCompact *hi_def = &ir->compact_instructions[hi_def_i];

    /* hi_def must be `T_hi = X SHR #32`. */
    if (hi_def->op != TCCIR_OP_SHR)
      continue;
    IROperand hi_src = tcc_ir_op_get_src1(ir, hi_def);
    IROperand hi_amt = tcc_ir_op_get_src2(ir, hi_def);
    if (!irop_is_immediate(hi_amt) || irop_get_imm64_ex(ir, hi_amt) != 32)
      continue;
    int32_t x_hi_vr = irop_get_vreg(hi_src);
    if (x_hi_vr < 0)
      continue;

    /* lo_def's chain root: an ASSIGN/LOAD pulling from X. */
    if (lo_def->op != TCCIR_OP_ASSIGN && lo_def->op != TCCIR_OP_LOAD)
      continue;
    IROperand lo_src = tcc_ir_op_get_src1(ir, lo_def);
    int32_t x_lo_vr = irop_get_vreg(lo_src);
    if (x_lo_vr < 0)
      continue;

    /* The two endpoints must reference X with matching access semantics.
     * If one reads X as an lvalue (is_lval=1, "value at storage") while the
     * other treats it as an address (is_lval=0, "address-of"), the pack
     * is NOT the identity — bail. */
    if (lo_src.is_lval != hi_src.is_lval)
      continue;

    if (x_lo_vr != x_hi_vr)
      continue;

    /* X must be a 64-bit value (else `X SHR #32` yields 0 and the pack is
     * not the identity). */
    IRLiveInterval *x_interval = tcc_ir_get_live_interval(ir, x_lo_vr);
    if (!x_interval || !(x_interval->is_llong || x_interval->is_double))
      continue;

    LOG_IR_GEN("OPTIMIZE: PACK64 tautology at i=%d (X vr=%d)", i, x_lo_vr);

    /* Rewrite PACK64 to ASSIGN dest = lo_src.  lo_src is the u64 reference
     * to X (as a deref/lvalue load) that lo_def used. */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, lo_src);
    changes++;

    /* Forward-substitute the PACK64 dest with X in subsequent uses within
     * the same basic block until the dest is redefined.  This lets the
     * existing identity-CMP fold catch `CMP X, X` patterns when the
     * resulting ASSIGN's dest is a VAR (which copy_prop does not track). */
    int32_t dest_vr = irop_get_vreg(pk_dest);
    if (dest_vr >= 0)
    {
      for (int j = i + 1; j < n; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        if (jq->is_jump_target)
          break;
        /* Stop on control-flow ops (preserve correctness across BBs). */
        if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
            jq->op == TCCIR_OP_RETURNVOID || jq->op == TCCIR_OP_RETURNVALUE)
          break;
        /* Substitute dest_vr → lo_src in src1 / src2. */
        if (irop_config[jq->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, jq);
          if (irop_get_vreg(s1) == dest_vr)
            tcc_ir_set_src1(ir, j, lo_src);
        }
        if (irop_config[jq->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, jq);
          if (irop_get_vreg(s2) == dest_vr)
            tcc_ir_set_src2(ir, j, lo_src);
        }
        /* Stop when this op redefines dest_vr. */
        if (irop_config[jq->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, jq);
          if (irop_get_vreg(d) == dest_vr)
            break;
        }
      }
    }
  }

  tcc_free(temp_def_idx);
  tcc_free(var_def_idx);
  tcc_free(temp_use_count);
  return changes;
}

int tcc_ir_opt_cmp_narrow_64(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* Build def-idx for TEMPs and VAR-STOREs. */
  int max_tmp_pos = 0;
  int max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_pos)
      max_tmp_pos = pos;
    if (type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
      max_var_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  int stride = max_tmp_pos + 1;
  int *def_idx = tcc_malloc(stride * sizeof(int));
  for (int i = 0; i < stride; i++)
    def_idx[i] = -1;
  /* Track VAR STORE definitions: last STORE to V at this position. */
  int var_stride = max_var_pos + 1;
  int *var_def_idx = NULL;
  if (var_stride > 0)
  {
    var_def_idx = tcc_malloc(var_stride * sizeof(int));
    for (int i = 0; i < var_stride; i++)
      var_def_idx[i] = -1;
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE && irop_config[q->op].has_dest)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && var_def_idx)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var_pos)
          var_def_idx[pos] = i;
      }
      continue;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_tmp_pos)
      def_idx[pos] = i;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    /* Both must be 64-bit. */
    if (irop_get_btype(src1) != IROP_BTYPE_INT64)
      continue;
    if (irop_get_btype(src2) != IROP_BTYPE_INT64)
      continue;

    /* Narrowing is only safe when the comparison condition treats both
     * widths identically:
     *   - EQ/NE: bitwise equality, always safe
     *   - unsigned <, <=, >, >=: since both operands have hi=0, the
     *     unsigned order is preserved at any width
     *   - SIGNED <, <=, >, >=: NOT safe — a u64 value like 0x00000000FFFF8000
     *     is positive at 64-bit but negative when interpreted as i32.
     * Look up the consuming SETIF/JUMPIF condition to decide. */
    int cond_ok = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (qj->op != TCCIR_OP_SETIF && qj->op != TCCIR_OP_JUMPIF)
        break;
      IROperand cond_op = tcc_ir_op_get_src1(ir, qj);
      if (!irop_is_immediate(cond_op))
        break;
      int tok = (int)irop_get_imm64_ex(ir, cond_op);
      if (tok == TOK_EQ || tok == TOK_NE ||
          tok == TOK_ULT || tok == TOK_ULE || tok == TOK_UGT || tok == TOK_UGE)
        cond_ok = 1;
      break;
    }
    if (!cond_ok)
      continue;

    /* src1 must be a TEMP whose def proves hi=0. */
    int32_t s1_vr = irop_get_vreg(src1);
    if (TCCIR_DECODE_VREG_TYPE(s1_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
    if (s1_pos > max_tmp_pos || def_idx[s1_pos] < 0)
      continue;
    IRQuadCompact *q_def = &ir->compact_instructions[def_idx[s1_pos]];
    int s1_hi_zero = 0;
    if (q_def->op == TCCIR_OP_ZEXT)
    {
      /* ZEXT from u32 → u64 always zeros the high half. */
      s1_hi_zero = 1;
    }
    else if (q_def->op == TCCIR_OP_SHR)
    {
      IROperand shr_amt = tcc_ir_op_get_src2(ir, q_def);
      if (irop_is_immediate(shr_amt) && irop_get_imm64_ex(ir, shr_amt) >= 32)
      {
        IROperand shr_src = tcc_ir_op_get_src1(ir, q_def);
        if (irop_get_btype(shr_src) == IROP_BTYPE_INT64)
          s1_hi_zero = 1;
      }
    }
    if (!s1_hi_zero)
      continue;

    /* src2 must be a u64 constant value with high 32 bits == 0.
     * Two forms:
     *   (a) inline immediate (IMM32 or I64 tag)
     *   (b) VAR with a STORE def that wrote a u64 constant (the IR
     *       keeps the printf arg locals as VAR-stored even after
     *       const-prop, so we have to walk the def chain) */
    uint64_t imm;
    int got_imm = 0;
    if (irop_is_immediate(src2))
    {
      imm = (uint64_t)irop_get_imm64_ex(ir, src2);
      got_imm = 1;
    }
    else if (var_def_idx)
    {
      int32_t s2_vr = irop_get_vreg(src2);
      if (TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
        if (s2_pos <= max_var_pos && var_def_idx[s2_pos] >= 0)
        {
          IRQuadCompact *q_vdef = &ir->compact_instructions[var_def_idx[s2_pos]];
          if (q_vdef->op == TCCIR_OP_STORE)
          {
            IROperand store_src = tcc_ir_op_get_src1(ir, q_vdef);
            if (irop_is_immediate(store_src))
            {
              imm = (uint64_t)irop_get_imm64_ex(ir, store_src);
              got_imm = 1;
            }
          }
        }
      }
    }
    if (!got_imm)
      continue;
    if ((imm >> 32) != 0)
      continue;

    /* Narrow both operands to INT32 by patching the CMP's operand-pool
     * entries.  T's defining op stays u64; other consumers see u64. */
    LOG_IR_GEN("OPTIMIZE: cmp_narrow_64 at i=%d (T%d hi=0, imm=%llu)", i, s1_pos, (unsigned long long)imm);
    IROperand new_src1 = src1;
    new_src1.btype = IROP_BTYPE_INT32;
    tcc_ir_set_src1(ir, i, new_src1);
    IROperand new_src2 = irop_make_imm32(-1, (int32_t)(uint32_t)imm, IROP_BTYPE_INT32);
    new_src2.is_unsigned = src2.is_unsigned;
    tcc_ir_set_src2(ir, i, new_src2);
    changes++;
  }
  if (var_def_idx)
    tcc_free(var_def_idx);

  tcc_free(def_idx);
  return changes;
}

/* tcc_ir_opt_shl32_or_chain: collapse `((X SHL 32) OR Y) SHL 32` and
 * `((X SHL 32) OR Y) AND 0xFFFFFFFF` chains.
 *
 * Both forms appear in the 32-bit-to-64-bit widening idiom used by TCC when
 * the C code does `((long long)val << 32)` or `((long long)val & 0xFFFFFFFFLL)`
 * via the manual sign-extension sequence:
 *
 *   T_sar  = X SAR #31              ; i32 sign-extension
 *   T_shl1 = T_sar SHL #32          ; place sign-ext into high half (i64)
 *   T_or   = T_shl1 OR X            ; (long long)X (sign-extended)
 *   T_use  = T_or SHL #32           ; → final = (long long)X << 32
 *      -- or --
 *   T_use  = T_or AND #0xFFFFFFFF   ; → final = (uint32_t)X zero-extended
 *
 * Because the high half of `T_shl1 OR X` is shifted out by the final SHL 32
 * (or masked out by AND 0xFFFFFFFF), `T_shl1` (and hence the SAR feeding it)
 * is dead.  Rewrite the final SHL/AND to read X directly so the SAR/SHL1/OR
 * chain becomes dead and gets DCE'd.
 *
 * IR shape before (pattern A — SHL 32 consumer):
 *   i_shl1:  T_shl1 = anything SHL #32      ; i64
 *   i_or:    T_or   = T_shl1 OR Y           ; i64, single-use
 *   i_use:   T_use  = T_or SHL #32          ; i64
 *
 * IR shape after:
 *   i_shl1:  NOP                            ; (was T_shl1's def, now dead)
 *   i_or:    NOP                            ; (was T_or's def, now dead)
 *   i_use:   T_use  = Y SHL #32             ; reads Y directly
 *
 * Pattern B (AND consumer) is the same but with `AND #0xFFFFFFFF` in place
 * of `SHL #32`.  Both T_shl1 and T_or must be single-use TEMPs so we can
 * safely NOP them. */
int tcc_ir_opt_shl32_or_chain(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 3)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Looking for the consumer: SHL #32 or AND #0xFFFFFFFF on a TEMP src1. */
    int is_shl32 = 0, is_and_low = 0;
    if (q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_AND)
    {
      IROperand q_src2 = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(q_src2))
        continue;
      int64_t imm = irop_get_imm64_ex(ir, q_src2);
      if (q->op == TCCIR_OP_SHL && imm == 32)
        is_shl32 = 1;
      else if (q->op == TCCIR_OP_AND && (uint32_t)imm == 0xFFFFFFFFu)
        /* Compare the low 32 bits only: irop_get_imm64_ex sign-extends a
           32-bit immediate, so the natural 0xFFFFFFFF low-word mask arrives
           here as int64_t -1 (0xFFFF...FFFF), which would never equal a
           0x00000000FFFFFFFF test. A full 64-bit IROP_TAG_I64 constant of
           0xFFFFFFFF (not sign-extended) also matches, as intended. */
        is_and_low = 1;
      else
        continue;
    }
    else
    {
      continue;
    }
    IROperand q_dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_btype(q_dest) != IROP_BTYPE_INT64)
      continue;

    IROperand q_src1 = tcc_ir_op_get_src1(ir, q);
    int32_t or_vr = irop_get_vreg(q_src1);
    if (TCCIR_DECODE_VREG_TYPE(or_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (q_src1.is_lval || q_src1.is_sym)
      continue;
    if (ir_opt_du_uses(&du, or_vr) != 1 || !ir_opt_du_is_single_def(&du, or_vr))
      continue;
    int or_def = ir_opt_du_def(&du, or_vr, n);
    if (or_def < 0)
      continue;

    IRQuadCompact *or_q = &ir->compact_instructions[or_def];
    if (or_q->op != TCCIR_OP_OR)
      continue;
    IROperand or_dest = tcc_ir_op_get_dest(ir, or_q);
    if (irop_get_btype(or_dest) != IROP_BTYPE_INT64)
      continue;

    /* One of OR's operands must be `something SHL #32` (the dead-bits half). */
    IROperand or_a = tcc_ir_op_get_src1(ir, or_q);
    IROperand or_b = tcc_ir_op_get_src2(ir, or_q);

    int chosen = -1; /* 0 → a is shl, b is keep; 1 → b is shl, a is keep */
    int shl1_def = -1;
    IROperand keep_op = IROP_NONE;

    for (int s = 0; s < 2; s++)
    {
      IROperand shl_cand = (s == 0) ? or_a : or_b;
      IROperand keep_cand = (s == 0) ? or_b : or_a;
      int32_t shl_vr = irop_get_vreg(shl_cand);
      if (TCCIR_DECODE_VREG_TYPE(shl_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (shl_cand.is_lval || shl_cand.is_sym)
        continue;
      if (ir_opt_du_uses(&du, shl_vr) != 1 || !ir_opt_du_is_single_def(&du, shl_vr))
        continue;
      int def = ir_opt_du_def(&du, shl_vr, n);
      if (def < 0)
        continue;
      IRQuadCompact *shl_q = &ir->compact_instructions[def];
      if (shl_q->op != TCCIR_OP_SHL)
        continue;
      IROperand shl_amt = tcc_ir_op_get_src2(ir, shl_q);
      if (!irop_is_immediate(shl_amt) || irop_get_imm64_ex(ir, shl_amt) != 32)
        continue;
      IROperand shl_dest_chk = tcc_ir_op_get_dest(ir, shl_q);
      if (irop_get_btype(shl_dest_chk) != IROP_BTYPE_INT64)
        continue;
      chosen = s;
      shl1_def = def;
      keep_op = keep_cand;
      break;
    }
    if (chosen < 0)
      continue;

    LOG_IR_GEN("OPTIMIZE: SHL32_OR_CHAIN %s at i=%d (or_def=%d, shl1_def=%d)",
               is_shl32 ? "SHL32" : "AND_low", i, or_def, shl1_def);
    (void)is_and_low;

    /* Rewrite consumer's src1 from T_or to the kept OR operand. */
    tcc_ir_set_src1(ir, i, keep_op);
    /* The OR is now dead; the SHL feeding it is dead (single-use both). */
    ir->compact_instructions[or_def].op = TCCIR_OP_NOP;
    ir->compact_instructions[shl1_def].op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(du.def);
  return changes;
}

/* tcc_ir_opt_shift64_dead_half: flag a 64-bit SHL whose result's low word is
 * dead so codegen can skip materializing it.  Targets the 64-bit
 * bitfield-extract idiom for a sub-32-bit field spanning a storage-unit word
 * boundary:
 *
 *   T1 = V  SHL #a      ; i64, T1 has a single use: the SHR below
 *   T2 = T1 SHR #b      ; i64, b >= 32  -> reads ONLY T1's high word
 *
 * A 64-bit SHR/SAR by >= 32 reads only its source's HIGH word, so a SHL whose
 * sole consumer is such a shift has a provably dead LOW word.  Skipping its
 * `lsl dst_lo, src_lo, #a` removes one instruction per spanning-field extract.
 *
 * Writes ir->shift64_dead_half[orig_index] = bit0:skip_lo (bit1:skip_hi is
 * honoured by codegen but not currently emitted — left for a future, equally
 * safe extension).  Pure annotation: no IR mutation.  Anchored from the SHR
 * consumer and gated on single-use of T1, so it stays valid across RA spills
 * (which store/reload the dead low word as never-read garbage). */
int tcc_ir_opt_shift64_dead_half(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  if (ir->shift64_dead_half)
  {
    tcc_free(ir->shift64_dead_half);
    ir->shift64_dead_half = NULL;
    ir->shift64_dead_half_len = 0;
  }

  /* Build last-def index for TEMPs (single-def in practice; the SHL we match
   * is single-use so its def is unambiguous). */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp_pos)
        max_tmp_pos = p;
    }
  }
  if (max_tmp_pos == 0)
    return 0;

  int stride = max_tmp_pos + 1;
  int *def_idx = tcc_malloc(stride * sizeof(int));
  for (int i = 0; i < stride; i++)
    def_idx[i] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p <= max_tmp_pos)
        def_idx[p] = i;
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Consumer: a 64-bit SHR/SAR by >= 32, reading a 64-bit TEMP src1. */
    if (q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
      continue;
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(s2) || irop_get_imm64_ex(ir, s2) < 32)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_btype(s1) != IROP_BTYPE_INT64)
      continue;
    int32_t s1_vr = irop_get_vreg(s1);
    if (TCCIR_DECODE_VREG_TYPE(s1_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
    if (s1_pos > max_tmp_pos || def_idx[s1_pos] < 0)
      continue;

    /* Producer must be a 64-bit SHL feeding only this shift. */
    int dpos = def_idx[s1_pos];
    IRQuadCompact *def = &ir->compact_instructions[dpos];
    if (def->op != TCCIR_OP_SHL)
      continue;
    if (irop_get_btype(tcc_ir_op_get_dest(ir, def)) != IROP_BTYPE_INT64)
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, s1_vr, dpos))
      continue;

    if (!ir->shift64_dead_half) {
      ir->shift64_dead_half = tcc_mallocz(ir->max_orig_index + 1);
      ir->shift64_dead_half_len = ir->max_orig_index + 1;
    }
    ir->shift64_dead_half[def->orig_index] |= 1; /* skip_lo */
    changes++;
  }

  tcc_free(def_idx);
  return changes;
}

int tcc_ir_opt_pack64_ex(IROptCtx *ctx) { return tcc_ir_opt_pack64(ctx->ir); }
int tcc_ir_opt_pack64_tautology_ex(IROptCtx *ctx) { return tcc_ir_opt_pack64_tautology(ctx->ir); }
int tcc_ir_opt_cmp_narrow_64_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_narrow_64(ctx->ir); }
int tcc_ir_opt_shl32_or_chain_ex(IROptCtx *ctx) { return tcc_ir_opt_shl32_or_chain(ctx->ir); }

