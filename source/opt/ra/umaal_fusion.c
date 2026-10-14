/*
 *  TCC IR - UMAAL fusion
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt_range.h"
#include "regalloc.h"

/* See docs/bugs/kernel-wyhash-pair-repacking-spills.md for accumulator widths and stability constraints. */

typedef struct UmaalCtx
{
  TCCIRState *ir;
  int n;
  int ntemp;
  int *mentions; /* per TEMP position: operand slots naming it (defs and uses) */
  int *def_idx;  /* per TEMP position: the (last) instruction defining it */
  uint8_t *ndefs; /* per TEMP position: definitions, saturating at 2 */
} UmaalCtx;

static int umaal_nops(const IRQuadCompact *q)
{
  int n = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
  if (tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_SELECT)
    n++;
  return n;
}

static int umaal_temp_pos(IROperand op)
{
  if (op.is_lval || !irop_has_vreg(op))
    return -1;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  return TCCIR_DECODE_VREG_POSITION(vr);
}

/* The instruction defining `op` when `op` is a TEMP with one def and one use,
 * both before `use`; else -1. */
static int umaal_single_def(UmaalCtx *c, IROperand op, int use)
{
  int p = umaal_temp_pos(op);
  if (p < 0 || p >= c->ntemp || c->mentions[p] != 2)
    return -1;
  int d = c->def_idx[p];
  if (d < 0 || d >= use)
    return -1;
  return d;
}

/* No label, branch or redefinition of `v` in (from, to].  A register value
 * (not an lvalue) only changes when an instruction names it as dest. */
static int umaal_value_stable(TCCIRState *ir, IROperand v, int from, int to)
{
  if (v.is_lval)
    return 0;
  int32_t vr = irop_has_vreg(v) ? irop_get_vreg(v) : -1;
  for (int k = from + 1; k <= to; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->is_jump_target && k > from)
      return 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (vr >= 0 && k < to && irop_config[q->op].has_dest)
    {
      if (!tcc_ir_op_dest_is_lval(ir, q) && tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == vr)
        return 0;
    }
  }
  return 1;
}

/* No control flow in [from, to): a jump ends the straight-line run. */
static int umaal_straight(TCCIRState *ir, int from, int to)
{
  /* Control flow only, `from` itself included: a join after it, at `to` too. */
  const uint32_t not_control = IR_HZ_CALL | IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_MEM_READ | IR_HZ_MEM_WRITE |
                               IR_HZ_UPDATES_SRC | IR_HZ_VLA | IR_HZ_CHAIN | IR_HZ_HINT | IR_HZ_FLAGS_SET |
                               IR_HZ_FLAGS_READ | IR_HZ_VOLATILE | IR_HZ_DEST_LVAL | IR_HZ_DEST_STACKOFF |
                               IR_HZ_SRC_LVAL;
  const uint32_t mask = IR_HZ_ALL & ~(not_control | IR_LEGACY_GAP_HZ(IR_HZ_TRAP | IR_HZ_ASM));
  const IROpSet gaps =
      IR_LEGACY_GAP_OPS(TCCIR_OP_BUILTIN_APPLY_ARGS, TCCIR_OP_BUILTIN_APPLY, TCCIR_OP_BUILTIN_RETURN);
  if (to == from)
    return 1;
  if (to < from)
    return !ir->compact_instructions[to].is_jump_target; /* IR_LEGACY_GAP: an inverted range */
  if (ir_q_hazards_except(ir, &ir->compact_instructions[from], mask & IR_HZ_FROM_OP, gaps))
    return 0;
  return ir_range_safe_except(ir, from, to, mask, gaps);
}

static int umaal_is_add64(TCCIRState *ir, int i)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, q))
    return 0;
  return !tcc_ir_op_dest_is_lval(ir, q) && tcc_ir_op_dest_btype(ir, q) == IROP_BTYPE_INT64 && tcc_ir_op_src1_btype(ir, q) == IROP_BTYPE_INT64 &&
         tcc_ir_op_src2_btype(ir, q) == IROP_BTYPE_INT64;
}

static int umaal_is_word_addend(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_ZEXT && q->op != TCCIR_OP_SHR)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  if (tcc_ir_op_dest_is_lval(ir, q) || tcc_ir_op_dest_btype(ir, q) != IROP_BTYPE_INT64 ||
      s.is_lval || tcc_ir_barrel_shift_at(ir, q))
    return 0;
  if (q->op == TCCIR_OP_ZEXT)
    return irop_get_btype(s) == IROP_BTYPE_INT32;
  return q->op == TCCIR_OP_SHR && irop_get_btype(s) == IROP_BTYPE_INT64 &&
         tcc_ir_op_src2_is_imm(ir, q) && tcc_ir_op_src2_imm(ir, q) == 32;
}

static int umaal_is_umull(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_UMULL || tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  return !tcc_ir_op_src1_is_lval(ir, q) && !s2.is_lval;
}

/* Split ADD into a product and a zero-extended word, including SHR #32. */
static void umaal_split(UmaalCtx *c, int i, int *mul, int *zext, int *other)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int d1 = umaal_single_def(c, tcc_ir_op_get_src1(ir, q), i);
  int d2 = umaal_single_def(c, tcc_ir_op_get_src2(ir, q), i);
  *mul = *zext = *other = -1;
  if (d1 < 0 || d2 < 0)
    return;
  int ds[2] = {d1, d2};
  for (int k = 0; k < 2; k++)
  {
    if (*mul < 0 && umaal_is_umull(ir, ds[k]))
      *mul = ds[k];
    else if (*zext < 0 && umaal_is_word_addend(ir, ds[k]))
      *zext = ds[k];
    else
      *other = ds[k];
  }
}

/* A single-use truncation can read the pair's low word directly if its value remains stable. */
static IROperand umaal_through_trunc(UmaalCtx *c, IROperand op, int use, int read_at, int *assign_idx)
{
  TCCIRState *ir = c->ir;
  *assign_idx = -1;
  int d = umaal_single_def(c, op, use);
  if (d < 0)
    return op;
  IRQuadCompact *q = &ir->compact_instructions[d];
  if (q->op != TCCIR_OP_ASSIGN || tcc_ir_barrel_shift_at(ir, q))
    return op;
  IROperand src = tcc_ir_op_get_src1(ir, q);
  if (tcc_ir_op_dest_btype(ir, q) != IROP_BTYPE_INT32 || irop_get_btype(src) != IROP_BTYPE_INT64 || src.is_lval ||
      !irop_has_vreg(src) || irop_get_vreg(src) < 0 || src.is_complex)
    return op;
  if (!umaal_value_stable(ir, src, d, read_at))
    return op;
  *assign_idx = d;
  src.btype = IROP_BTYPE_INT32;
  return src;
}

static void umaal_set_ops(TCCIRState *ir, IRQuadCompact *q, const IROperand *ops, int n)
{
  int base = ir->iroperand_pool_count;
  tcc_ir_pool_ensure(ir, n);
  for (int k = 0; k < n; k++)
    tcc_ir_pool_add(ir, ops[k]);
  q->operand_base = base;
}

static void umaal_narrow_shift(TCCIRState *ir, int i)
{
  /* The sole consumer reads a word; preserve the pair's original lifetime. */
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_SHR)
    return;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  d.btype = IROP_BTYPE_INT32;
  tcc_ir_set_dest(ir, i, d);
  tcc_ir_vreg_live_interval(ir, irop_get_vreg(d))->is_llong = 0;
}

static int umaal_try(UmaalCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  if (!umaal_is_add64(ir, i))
    return 0;

  int mul, z1, other, z2 = -1, inner = -1;
  umaal_split(c, i, &mul, &z1, &other);
  if (mul < 0 && z1 >= 0 && other >= 0 && umaal_is_add64(ir, other))
  {
    /* R = (T ADD Z1) ADD Z2: the inner ADD must itself be product + addend. */
    int im, iz, io;
    umaal_split(c, other, &im, &iz, &io);
    if (im < 0 || iz < 0 || io >= 0)
      return 0;
    inner = other;
    z2 = z1;
    z1 = iz;
    mul = im;
  }
  else if (mul < 0 || z1 < 0 || other >= 0)
    return 0;

  IRQuadCompact *mq = &ir->compact_instructions[mul];
  IROperand a = tcc_ir_op_get_src1(ir, mq);
  IROperand b = tcc_ir_op_get_src2(ir, mq);
  IRQuadCompact *zq1 = &ir->compact_instructions[z1];
  IROperand cz = zq1->op == TCCIR_OP_ZEXT ? tcc_ir_op_get_src1(ir, zq1) : tcc_ir_op_get_dest(ir, zq1);
  int first = mul < z1 ? mul : z1;
  if (inner >= 0 && inner < first)
    first = inner;
  if (z2 >= 0 && z2 < first)
    first = z2;
  if (!umaal_straight(ir, first, i))
    return 0;
  if (!umaal_value_stable(ir, a, mul, i) || !umaal_value_stable(ir, b, mul, i))
    return 0;

  IROperand accum;
  if (z2 < 0)
  {
    if (!umaal_value_stable(ir, cz, z1, i))
      return 0;
    int ai;
    accum = umaal_through_trunc(c, cz, z1, i, &ai);
    accum.btype = IROP_BTYPE_INT32;
    if (ai >= 0)
      ir->compact_instructions[ai].op = TCCIR_OP_NOP;
    umaal_narrow_shift(ir, z1);
    if (zq1->op == TCCIR_OP_ZEXT)
      zq1->op = TCCIR_OP_NOP;
  }
  else
  {
    IRQuadCompact *zq2 = &ir->compact_instructions[z2];
    IROperand dz = zq2->op == TCCIR_OP_ZEXT ? tcc_ir_op_get_src1(ir, zq2) : tcc_ir_op_get_dest(ir, zq2);
    int slot = z1 > z2 ? z1 : z2;
    if (inner > slot)
      slot = inner;
    if (!umaal_value_stable(ir, cz, z1, slot) || !umaal_value_stable(ir, dz, z2, slot))
      return 0;
    int moved = -1;
    if (slot == z2 && zq2->op == TCCIR_OP_SHR)
    {
      /* Free the latest slot for PACK64 by moving its extraction into the eliminated ADD. */
      IROperand src = tcc_ir_op_get_src1(ir, zq2);
      if (inner < 0 || inner >= z2 || !umaal_value_stable(ir, src, inner - 1, z2))
        return 0;
      moved = inner;
    }
    IRQuadCompact *sq = &ir->compact_instructions[slot];
    accum = tcc_ir_op_get_dest(ir, &ir->compact_instructions[inner]);
    umaal_narrow_shift(ir, z1);
    umaal_narrow_shift(ir, z2);
    if (moved >= 0)
      ir->compact_instructions[moved] = *zq2;
    int a1, a2;
    IROperand lo = umaal_through_trunc(c, cz, z1, slot, &a1);
    IROperand hi = umaal_through_trunc(c, dz, z2, slot, &a2);
    if (a1 >= 0)
      ir->compact_instructions[a1].op = TCCIR_OP_NOP;
    if (a2 >= 0)
      ir->compact_instructions[a2].op = TCCIR_OP_NOP;
    lo.btype = IROP_BTYPE_INT32;
    hi.btype = IROP_BTYPE_INT32;
    IROperand pk[3] = {accum, lo, hi};
    sq->op = TCCIR_OP_PACK64;
    umaal_set_ops(ir, sq, pk, 3);
    if (z1 != slot && zq1->op == TCCIR_OP_ZEXT)
      zq1->op = TCCIR_OP_NOP;
    if (z2 != slot && zq2->op == TCCIR_OP_ZEXT)
      zq2->op = TCCIR_OP_NOP;
    if (inner != slot && inner != moved)
      ir->compact_instructions[inner].op = TCCIR_OP_NOP;
  }

  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand ops[4] = {tcc_ir_op_get_dest(ir, q), a, b, accum};
  ops[1].btype = IROP_BTYPE_INT32;
  ops[2].btype = IROP_BTYPE_INT32;
  ops[0].is_unsigned = 1;
  q->op = TCCIR_OP_UMAAL;
  umaal_set_ops(ir, q, ops, 4);
  mq->op = TCCIR_OP_NOP;
  return 1;
}

/* ---- a constant partial product in the carry chain --------------------------
 * The schoolbook chain (try_inline_multi3, or the same thing written in C)
 *
 *   t1 = K ADD z              K = a1*b0, folded once a1 is a known word
 *   t2 = P ADD ZEXT(lo32(t1)) P = a0*b1
 *   S  = ... ADD ZEXT(hi32(t1)) ... ADD (t2 SHR #32)
 *
 * lost its UMAALs when ssa:fold made K a constant: t1 became `adds/adc` of two
 * literals and t2 a UMAAL with a zero high accumulator.  P + z + K.lo fits in
 * 64 bits, so with c = carry(K.lo + z) -- and K.hi + c not wrapping:
 *
 *   t2' = P + z + K.lo = t2 + c * 2^32     one UMAAL, accumulators z and K.lo
 *   lo32(t2') = lo32(t2),  hi32(t2') = hi32(t2) + c,  hi32(t1) = K.hi + c
 *
 * so S keeps its value with hi32(t1) replaced by K.hi and t2 by t2' -- as
 * long as both reach S only as addends. */

/* `op` is a 64-bit literal; its value in *v. */
static int umaal_imm64(TCCIRState *ir, IROperand op, uint64_t *v)
{
  if (!irop_is_immediate(op) || op.is_lval || op.is_sym || irop_get_btype(op) != IROP_BTYPE_INT64)
    return 0;
  *v = (uint64_t)irop_get_imm64_ex(ir, op);
  return 1;
}

/* The defining instruction of a TEMP with one def named `uses` times besides,
 * else -1. */
static int umaal_def_with_uses(UmaalCtx *c, IROperand op, int uses)
{
  int p = umaal_temp_pos(op);
  if (p < 0 || p >= c->ntemp || c->ndefs[p] != 1 || c->mentions[p] != uses + 1)
    return -1;
  return c->def_idx[p];
}

/* The one instruction after `def` reading TEMP `vr`, other than `skip`. */
static int umaal_other_use(TCCIRState *ir, int32_t vr, int def, int skip)
{
  for (int k = def + 1; k < ir->next_instruction_index; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (k == skip || q->op == TCCIR_OP_NOP)
      continue;
    if ((irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == vr) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == vr))
      return k;
  }
  return -1;
}

/* `d <- s [ASSIGN]` truncating a 64-bit TEMP to a word, or `d <- ZEXT s` of a
 * word into a 64-bit TEMP (`want_zext`). */
static int umaal_is_narrowing(TCCIRState *ir, int idx, int want_zext)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != (want_zext ? TCCIR_OP_ZEXT : TCCIR_OP_ASSIGN) || tcc_ir_barrel_shift_at(ir, q) ||
      tcc_ir_op_dest_is_lval(ir, q))
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  if (s.is_lval || !irop_has_vreg(s) || irop_get_vreg(s) < 0)
    return 0;
  int from = want_zext ? IROP_BTYPE_INT32 : IROP_BTYPE_INT64, to = want_zext ? IROP_BTYPE_INT64 : IROP_BTYPE_INT32;
  return irop_get_btype(s) == from && tcc_ir_op_dest_btype(ir, q) == to;
}

/* `d <- s SHR #32` of a 64-bit TEMP. */
static int umaal_is_hi_shift(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_SHR || tcc_ir_barrel_shift_at(ir, q) || tcc_ir_op_dest_is_lval(ir, q) ||
      tcc_ir_op_dest_btype(ir, q) != IROP_BTYPE_INT64 || !tcc_ir_op_src2_is_imm(ir, q) ||
      tcc_ir_op_src2_imm(ir, q) != 32)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  return !s.is_lval && irop_get_btype(s) == IROP_BTYPE_INT64;
}

/* The operand of ADD `q` other than TEMP `vr`, or IROP_NONE when `vr` is not
 * exactly one of them. */
static IROperand umaal_add_other(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, q), s2 = tcc_ir_op_get_src2(ir, q);
  int m1 = !s1.is_lval && irop_get_vreg(s1) == vr, m2 = !s2.is_lval && irop_get_vreg(s2) == vr;
  if (m1 == m2)
    return IROP_NONE;
  return m1 ? s2 : s1;
}

static int umaal_try_const_carry(UmaalCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  if (!umaal_is_add64(ir, i))
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand t2 = tcc_ir_op_get_dest(ir, q);

  /* t2 = P ADD zz, zz = ZEXT(w1), w1 = lo32(t1) */
  int d1 = umaal_single_def(c, tcc_ir_op_get_src1(ir, q), i);
  int d2 = umaal_single_def(c, tcc_ir_op_get_src2(ir, q), i);
  if (d1 < 0 || d2 < 0)
    return 0;
  int mul = umaal_is_umull(ir, d1) ? d1 : d2, zz = mul == d1 ? d2 : d1;
  if (!umaal_is_umull(ir, mul) || !umaal_is_narrowing(ir, zz, 1))
    return 0;
  int dw1 = umaal_single_def(c, tcc_ir_op_get_src1(ir, &ir->compact_instructions[zz]), zz);
  if (dw1 < 0 || !umaal_is_narrowing(ir, dw1, 0))
    return 0;
  IROperand t1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[dw1]);
  int dt1 = umaal_def_with_uses(c, t1, 2);
  if (dt1 < 0 || dt1 >= dw1 || !umaal_is_add64(ir, dt1))
    return 0;

  /* t1 = K ADD z, z a zero-extended word */
  IRQuadCompact *t1q = &ir->compact_instructions[dt1];
  uint64_t k;
  IROperand z = tcc_ir_op_get_src2(ir, t1q);
  if (!umaal_imm64(ir, tcc_ir_op_get_src1(ir, t1q), &k))
  {
    z = tcc_ir_op_get_src1(ir, t1q);
    if (!umaal_imm64(ir, tcc_ir_op_get_src2(ir, t1q), &k))
      return 0;
  }
  if ((uint32_t)(k >> 32) == 0xFFFFFFFFu)
    return 0; /* K.hi + carry could wrap */
  int zp = umaal_temp_pos(z);
  if (zp < 0 || zp >= c->ntemp || c->ndefs[zp] != 1)
    return 0;
  int dz = c->def_idx[zp];
  if (dz < 0 || dz >= dt1 || !umaal_is_word_addend(ir, dz))
    return 0;
  IRQuadCompact *zq = &ir->compact_instructions[dz];
  IROperand zword = zq->op == TCCIR_OP_ZEXT ? tcc_ir_op_get_src1(ir, zq) : z;

  /* hi32(t1): s = t1 SHR #32, read directly or as ZEXT(lo32(s)): zw */
  int ds = umaal_other_use(ir, irop_get_vreg(t1), dt1, dw1);
  if (ds < 0 || !umaal_is_hi_shift(ir, ds))
    return 0;
  IROperand zw = tcc_ir_op_get_dest(ir, &ir->compact_instructions[ds]);
  int dzw = ds, dw2 = -1;
  int u = umaal_other_use(ir, irop_get_vreg(zw), ds, -1);
  if (umaal_def_with_uses(c, zw, 1) != ds || u < 0)
    return 0;
  if (umaal_is_narrowing(ir, u, 0))
  {
    dw2 = u;
    IROperand w2 = tcc_ir_op_get_dest(ir, &ir->compact_instructions[dw2]);
    u = umaal_other_use(ir, irop_get_vreg(w2), dw2, -1);
    if (umaal_def_with_uses(c, w2, 1) != dw2 || u < 0 || !umaal_is_narrowing(ir, u, 1))
      return 0;
    dzw = u;
    zw = tcc_ir_op_get_dest(ir, &ir->compact_instructions[dzw]);
    u = umaal_other_use(ir, irop_get_vreg(zw), dzw, -1);
    if (umaal_def_with_uses(c, zw, 1) != dzw || u < 0)
      return 0;
  }

  /* t2's two readers: lo32(t2), and h = t2 SHR #32 */
  if (umaal_def_with_uses(c, t2, 2) != i)
    return 0;
  int r1 = umaal_other_use(ir, irop_get_vreg(t2), i, -1), r2 = r1 < 0 ? -1 : umaal_other_use(ir, irop_get_vreg(t2), i, r1);
  if (r1 < 0 || r2 < 0)
    return 0;
  int dh = umaal_is_hi_shift(ir, r1) ? r1 : r2, lo = dh == r1 ? r2 : r1;
  if (!umaal_is_hi_shift(ir, dh) || !umaal_is_narrowing(ir, lo, 0))
    return 0;
  IROperand h = tcc_ir_op_get_dest(ir, &ir->compact_instructions[dh]);
  int sidx = umaal_other_use(ir, irop_get_vreg(h), dh, -1);
  if (umaal_def_with_uses(c, h, 1) != dh || sidx < 0 || !umaal_is_add64(ir, sidx))
    return 0;

  /* S = h ADD zw, or h ADD y with y = zw ADD x read only there */
  IRQuadCompact *sq = &ir->compact_instructions[sidx];
  IROperand y = umaal_add_other(ir, sq, irop_get_vreg(h));
  if (irop_get_tag(y) != IROP_TAG_VREG)
    return 0;
  int yidx = -1;
  if (irop_get_vreg(y) != irop_get_vreg(zw))
  {
    yidx = u;
    if (umaal_def_with_uses(c, y, 1) != yidx || !umaal_is_add64(ir, yidx) ||
        irop_get_tag(umaal_add_other(ir, &ir->compact_instructions[yidx], irop_get_vreg(zw))) == IROP_TAG_NONE)
      return 0;
  }
  else if (u != sidx)
    return 0;

  int last = sidx > dzw ? sidx : dzw, first = dz < mul ? dz : mul;
  if (lo > last)
    last = lo;
  if (!umaal_straight(ir, first, last))
    return 0;
  IRQuadCompact *mq = &ir->compact_instructions[mul];
  IROperand a = tcc_ir_op_get_src1(ir, mq), b = tcc_ir_op_get_src2(ir, mq);
  if (!umaal_value_stable(ir, a, mul, i) || !umaal_value_stable(ir, b, mul, i) ||
      !umaal_value_stable(ir, zword, dz, zz))
    return 0;

  /* zz <- PACK64 z.word #K.lo;  t2 <- UMAAL a b zz */
  zword.btype = IROP_BTYPE_INT32;
  IROperand klo = irop_make_imm32(-1, (int32_t)(uint32_t)k, IROP_BTYPE_INT32);
  klo.is_unsigned = 1;
  IRQuadCompact *zzq = &ir->compact_instructions[zz];
  IROperand pk[3] = {tcc_ir_op_get_dest(ir, zzq), zword, klo};
  zzq->op = TCCIR_OP_PACK64;
  umaal_set_ops(ir, zzq, pk, 3);
  IROperand ops[4] = {t2, a, b, pk[0]};
  ops[1].btype = IROP_BTYPE_INT32;
  ops[2].btype = IROP_BTYPE_INT32;
  ops[0].is_unsigned = 1;
  q->op = TCCIR_OP_UMAAL;
  umaal_set_ops(ir, q, ops, 4);
  mq->op = TCCIR_OP_NOP;

  /* hi32(t1) -> K.hi, folded into y's literal when it has one */
  uint64_t khi = k >> 32, yk;
  IRQuadCompact *yq = yidx >= 0 ? &ir->compact_instructions[yidx] : NULL;
  int fold_y = yq && umaal_imm64(ir, umaal_add_other(ir, yq, irop_get_vreg(zw)), &yk);
  IRQuadCompact *aq = fold_y ? yq : &ir->compact_instructions[dzw];
  uint64_t v = fold_y ? yk + khi : khi;
  IROperand ad = tcc_ir_op_get_dest(ir, aq);
  IROperand imm = (int64_t)v == (int64_t)(int32_t)v
                      ? irop_make_imm32(-1, (int32_t)v, IROP_BTYPE_INT64)
                      : irop_make_i64(-1, tcc_ir_pool_add_i64(ir, (int64_t)v), IROP_BTYPE_INT64);
  imm.is_unsigned = ad.is_unsigned;
  IROperand as[2] = {ad, imm};
  aq->op = TCCIR_OP_ASSIGN;
  umaal_set_ops(ir, aq, as, 2);
  if (fold_y)
    ir->compact_instructions[dzw].op = TCCIR_OP_NOP;
  ir->compact_instructions[dt1].op = TCCIR_OP_NOP;
  ir->compact_instructions[dw1].op = TCCIR_OP_NOP;
  if (dzw != ds)
    ir->compact_instructions[ds].op = TCCIR_OP_NOP;
  if (dw2 >= 0)
    ir->compact_instructions[dw2].op = TCCIR_OP_NOP;

  /* The census no longer describes these: no later match may rely on it. */
  int touched[] = {umaal_temp_pos(t1), umaal_temp_pos(zw), umaal_temp_pos(t2), umaal_temp_pos(h), zp};
  for (unsigned n = 0; n < sizeof touched / sizeof touched[0]; n++)
    if (touched[n] >= 0 && touched[n] < c->ntemp)
      c->ndefs[touched[n]] = 2;
  return 1;
}

static void umaal_census(UmaalCtx *c)
{
  TCCIRState *ir = c->ir;
  memset(c->mentions, 0, sizeof(int) * (size_t)c->ntemp);
  memset(c->ndefs, 0, (size_t)c->ntemp);
  for (int p = 0; p < c->ntemp; p++)
    c->def_idx[p] = -1;
  for (int i = 0; i < c->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = umaal_nops(q);
    for (int k = 0; k < nops; k++)
    {
      int32_t vr = irop_get_vreg(ir->iroperand_pool[q->operand_base + k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(vr) < c->ntemp)
        c->mentions[TCCIR_DECODE_VREG_POSITION(vr)]++;
    }
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int p = umaal_temp_pos(d);
      if (p >= 0 && p < c->ntemp)
      {
        c->def_idx[p] = i;
        if (c->ndefs[p] < 2)
          c->ndefs[p]++;
      }
    }
  }
}

int ra_fuse_umaal(TCCIRState *ir)
{
  if (!tcc_machine_has_umaal() || ir->next_instruction_index < 3)
    return 0;
  UmaalCtx c = {.ir = ir, .n = ir->next_instruction_index};
  c.ntemp = ir->next_temporary_variable > 0 ? ir->next_temporary_variable : 1;

  /* Cheap prefilter: a UMULL feeding anything at all. */
  int any = 0;
  for (int i = 0; i < c.n && !any; i++)
    any = ir->compact_instructions[i].op == TCCIR_OP_UMULL;
  if (!any)
    return 0;

  c.mentions = tcc_mallocz(sizeof(int) * (size_t)c.ntemp);
  c.def_idx = tcc_malloc(sizeof(int) * (size_t)c.ntemp);
  c.ndefs = tcc_mallocz((size_t)c.ntemp);
  umaal_census(&c);

  int changes = 0;
  /* Match the outer ADD before its inner product-plus-addend. */
  for (int i = c.n - 1; i >= 0; i--)
    changes += umaal_try_const_carry(&c, i) || umaal_try(&c, i);

  if (changes)
    umaal_census(&c);
  for (int i = 0; i < c.n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || tcc_ir_barrel_shift_at(ir, q))
      continue;
    int nops = umaal_nops(q);
    for (int k = irop_config[q->op].has_dest; k < nops; k++)
    {
      IROperand s = ir->iroperand_pool[q->operand_base + k];
      if (!irop_is_vreg_value(s) ||
          (irop_get_btype(s) != IROP_BTYPE_INT32 &&
           !(q->op == TCCIR_OP_ASSIGN && tcc_ir_op_dest_btype(ir, q) == IROP_BTYPE_INT32)))
        continue;
      int d = umaal_single_def(&c, s, i);
      if (d < 0 || !umaal_is_word_addend(ir, d) || ir->compact_instructions[d].op != TCCIR_OP_SHR)
        continue;
      umaal_narrow_shift(ir, d);
      s.btype = IROP_BTYPE_INT32;
      ir->iroperand_pool[q->operand_base + k] = s;
      changes++;
    }
  }

  tcc_free(c.mentions);
  tcc_free(c.def_idx);
  tcc_free(c.ndefs);
  return changes;
}
