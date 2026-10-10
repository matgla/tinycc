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

/* A 32x32->64 product plus one or two zero-extended words,
 *
 *     T = a UMULL b;  Z1 = ZEXT c;  S = T ADD Z1            (one addend)
 *     T = a UMULL b;  Z1 = ZEXT c;  S = T ADD Z1;  Z2 = ZEXT d;  R = S ADD Z2
 *
 * is ARM's UMAAL, {hi:lo} = a * b + c + d, which cannot overflow 64 bits.  It is
 * exactly the step of a multi-word multiply (zig.h's u128 product, soft-float
 * mantissa products), and as separate instructions each step keeps two 64-bit
 * temporaries alive -- the product and the widened addend -- which is where a
 * register-hungry caller starts spilling.
 *
 * Runs after phi resolution and the late address hoists, right before live
 * intervals are built, so only the allocator, the post-RA passes and codegen
 * ever see a TCCIR_OP_UMAAL (they read its accumulator through
 * tcc_ir_op_is_mac).  The accumulator operand is a 64-bit value whose two
 * words are the addends: the ZEXT itself for one addend, a PACK64 of both
 * for two (built in the slot of one of the instructions the fusion frees).
 *
 * The UMAAL sits where the final ADD was, so it reads a, b (and the PACK64
 * reads c, d) later than the original instructions did: none of them may be
 * redefined in between, and the whole chain must be one straight-line run. */

typedef struct UmaalCtx
{
  TCCIRState *ir;
  int n;
  int ntemp;
  int *mentions; /* per TEMP position: operand slots naming it (defs and uses) */
  int *def_idx;  /* per TEMP position: the (last) instruction defining it */
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

/* `idx` is `ZEXT c` from a 32-bit c into a 64-bit TEMP. */
static int umaal_is_zext32(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_ZEXT)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  /* Exactly a word: a narrower source's register is not known to be clean. */
  return tcc_ir_op_dest_btype(ir, q) == IROP_BTYPE_INT64 && irop_get_btype(s) == IROP_BTYPE_INT32 && !s.is_lval;
}

static int umaal_is_umull(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_UMULL || tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  return !tcc_ir_op_src1_is_lval(ir, q) && !s2.is_lval;
}

/* Split ADD i into (umull, zext): the index of each, or -1. */
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
    else if (*zext < 0 && umaal_is_zext32(ir, ds[k]))
      *zext = ds[k];
    else
      *other = ds[k];
  }
}

/* `op` is `(uint32)Y`, a truncating ASSIGN of a 64-bit Y used only here:
 * return a word-typed view of Y (which reads its low register) and the
 * ASSIGN's index, so the copy and its temporary go away.  Y must still hold
 * the same value at `read_at`. */
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
  IROperand cz = tcc_ir_op_get_src1(ir, &ir->compact_instructions[z1]);
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
    /* One addend: ZEXT c already is {c, 0}. */
    IRQuadCompact *zq = &ir->compact_instructions[z1];
    accum = tcc_ir_op_get_dest(ir, zq);
    int ai;
    IROperand w = umaal_through_trunc(c, cz, z1, z1, &ai);
    if (ai >= 0)
    {
      IROperand zo[2] = {accum, w};
      umaal_set_ops(ir, zq, zo, 2);
      ir->compact_instructions[ai].op = TCCIR_OP_NOP;
    }
  }
  else
  {
    /* Two: PACK64 {c, d} in the latest freed slot (after both ZEXTs, so both
     * words are defined there). */
    IROperand dz = tcc_ir_op_get_src1(ir, &ir->compact_instructions[z2]);
    int slot = z1 > z2 ? z1 : z2;
    if (inner > slot)
      slot = inner;
    if (!umaal_value_stable(ir, cz, z1, slot) || !umaal_value_stable(ir, dz, z2, slot))
      return 0;
    IRQuadCompact *sq = &ir->compact_instructions[slot];
    accum = tcc_ir_op_get_dest(ir, sq);
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
    if (z1 != slot)
      ir->compact_instructions[z1].op = TCCIR_OP_NOP;
    if (z2 != slot)
      ir->compact_instructions[z2].op = TCCIR_OP_NOP;
    if (inner != slot)
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
  for (int p = 0; p < c.ntemp; p++)
    c.def_idx[p] = -1;
  for (int i = 0; i < c.n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = umaal_nops(q);
    for (int k = 0; k < nops; k++)
    {
      int32_t vr = irop_get_vreg(ir->iroperand_pool[q->operand_base + k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(vr) < c.ntemp)
        c.mentions[TCCIR_DECODE_VREG_POSITION(vr)]++;
    }
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int p = umaal_temp_pos(d);
      if (p >= 0 && p < c.ntemp)
        c.def_idx[p] = i;
    }
  }

  /* Backwards, so a two-addend chain is met at its outer ADD: forwards, its
   * inner `T ADD Z1` would fuse alone first and hide the second addend. */
  int changes = 0;
  for (int i = c.n - 1; i >= 0; i--)
    changes += umaal_try(&c, i);

  tcc_free(c.mentions);
  tcc_free(c.def_idx);
  return changes;
}
