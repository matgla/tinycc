/*
 *  TCC IR - known-zero halves of 64-bit values
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

/* tcc_ir_opt_zero_half64: annotate, per instruction, which halves of its 64-bit
 * operands are provably the constant zero -- and which halves of its result no
 * consumer will read as a consequence.
 *
 * Soft float builds every packed double out of the same shape:
 *
 *   (uint64_t)(sign & 1) << 63 | (uint64_t)(exp & 0x7FF) << 52 | (mant & MASK)
 *
 * Each widening cast materialises a zero high word, each shift by >= 32
 * materialises a zero low word, and every OR then reads those zeros back to
 * compute `0 | 0` and `0 | x`.  make_double cost 17 instructions against GCC's
 * 8, and four of the four registers it had to push were holding zeros.
 *
 * The information needed to delete all of it is one bit per half:
 *
 *   consumer side - an OR/XOR against a zero half is a copy of the other
 *                   operand, an AND against one is the constant zero, so the
 *                   emitter reads neither register;
 *   producer side - a half every consumer has stopped reading need not be
 *                   written at all, which is what removes the `mov rN, #0`
 *                   itself (and the scratch register that held it).
 *
 * Both directions are needed: on its own the consumer rule only turns an ORR
 * into a MOV, and the producer rule has nothing to justify it.
 *
 * Pure annotation, no IR mutation.  Runs last, after every operand rewrite, for
 * the same reason tcc_ir_opt_shift64_dead_half does: the producer rule is only
 * valid against the FINAL instruction stream, and a later pass redirecting one
 * more reader onto a value whose half is marked dead would make codegen read a
 * register it never wrote.
 */

/* Bit layout of one annotation byte -- kept in sync with the decoder in
 * tcc_gen_machine_data_processing_mop (bits 18-23 of its `barrel_shift`). */
#define ZH64_S1_LO 0x01u  /* src1's low  word is provably 0 */
#define ZH64_S1_HI 0x02u  /* src1's high word is provably 0 */
#define ZH64_S2_LO 0x04u
#define ZH64_S2_HI 0x08u
#define ZH64_D_LO 0x10u /* dest's low  word is dead: no consumer reads it */
#define ZH64_D_HI 0x20u

/* Known-zero halves of a value, as a two-bit set: bit0 = low word is 0,
 * bit1 = high word is 0. */
#define ZK_LO 1
#define ZK_HI 2

typedef struct
{
  uint8_t *zk_t;  /* per TEMP position: ZK_LO | ZK_HI */
  int *def_t;     /* per TEMP position: index of its single def, -1 / -2 */
  int max_t;
  uint8_t *zk_v;  /* the same for named locals (VAR) */
  int *def_v;
  uint8_t *ok_t;   /* per TEMP position: exactly one definition */
  uint8_t *ok_v;   /* per VAR position: one definition and no LEA */
  int max_v;
} ZHState;

/* A named local needs one guard a temp does not: its address must never be
 * handed out, or a store through a pointer changes it behind this scan's back.
 * (The single-definition requirement is common to both and is settled by the
 * def census in tcc_ir_opt_zero_half64.)  Same rule tcc_ir_opt_cmp_narrow_64
 * applies to a local. */
static int zh_var_addr_taken(TCCIRState *ir, int32_t vr)
{
  for (int k = 0; k < ir->next_instruction_index; k++)
  {
    IRQuadCompact *qk = &ir->compact_instructions[k];
    if (qk->op == TCCIR_OP_LEA && irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == vr)
      return 1;
  }
  return 0;
}

/* The known-zero halves recorded for `vr`, or 0 when it has no entry. */
static int zh_lookup(const ZHState *st, int32_t vr)
{
  int t = TCCIR_DECODE_VREG_TYPE(vr);
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_TEMP)
    return (p >= 0 && p <= st->max_t && st->ok_t[p]) ? st->zk_t[p] : 0;
  if (t == TCCIR_VREG_TYPE_VAR)
    return (p >= 0 && p <= st->max_v && st->ok_v[p]) ? st->zk_v[p] : 0;
  return 0;
}

/* The halves of `op` that are provably zero.
 *
 * A non-pair operand is the important free case: every path that widens one
 * into a 64-bit slot zero-extends it (thumb_emit_data_processing_mop64 does so
 * with an explicit `mov rn_hi, #0`), so its high word is zero by construction.
 * Signed narrow values reach a 64-bit op through an explicit sign-extending
 * ASSIGN, which is a pair by then, so this never mistakes a sign fill for a
 * zero. */
static int zh_known(TCCIRState *ir, const ZHState *st, IROperand op)
{
  if (irop_is_immediate(op))
  {
    uint64_t v = (uint64_t)irop_get_imm64_ex(ir, op);
    int r = 0;
    if ((uint32_t)v == 0)
      r |= ZK_LO;
    if ((uint32_t)(v >> 32) == 0)
      r |= ZK_HI;
    return r;
  }
  if (!irop_needs_pair(op))
    return ZK_HI;
  return zh_lookup(st, irop_get_vreg(op));
}

/* The halves of a 64-bit result that are provably zero, from its operands'.
 * Only the ops whose per-half behaviour is exact are listed; everything else
 * returns 0 ("nothing known"), which is always safe. */
static int zh_result(TCCIRState *ir, const ZHState *st, IRQuadCompact *q)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  int a = zh_known(ir, st, s1);

  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_ZEXT:
    /* Widening or copying assign; a truncating one has a non-pair dest and is
     * never asked about here.  A narrow source reports ZK_HI on its own (it is
     * zero-extended), which is exactly what ZEXT guarantees. */
    return a;

  case TCCIR_OP_AND:
    return a | zh_known(ir, st, s2);

  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return a & zh_known(ir, st, s2);

  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  {
    if (!irop_is_immediate(s2))
      return 0;
    int64_t k = irop_get_imm64_ex(ir, s2);
    if (k < 0)
      return 0;
    /* An arithmetic right shift fills from the sign bit, which is only known
     * to be zero when the source's high word is. */
    if (q->op == TCCIR_OP_SAR && !(a & ZK_HI))
      return 0;
    if (k >= 64)
      return ZK_LO | ZK_HI; /* UB in C; the emitter produces zero */
    if (k == 0)
      return a;
    if (q->op == TCCIR_OP_SHL)
    {
      /* The low word is fed only by src's low word, the high word by both. */
      int r = (k >= 32) ? ZK_LO : ((a & ZK_LO) ? ZK_LO : 0);
      int hi_src = (k >= 32) ? (a & ZK_LO) : ((a & ZK_LO) && (a & ZK_HI));
      if (hi_src)
        r |= ZK_HI;
      return r;
    }
    /* SHR / SAR-with-zero-sign: mirror image. */
    {
      int r = (k >= 32) ? ZK_HI : ((a & ZK_HI) ? ZK_HI : 0);
      int lo_src = (k >= 32) ? (a & ZK_HI) : ((a & ZK_LO) && (a & ZK_HI));
      if (lo_src)
        r |= ZK_LO;
      return r;
    }
  }

  default:
    return 0;
  }
}

/* Does the use at `u` leave the half `half` (ZK_LO / ZK_HI) of `vreg` unread,
 * GIVEN that this pass has told the emitter the half is zero?
 *
 * The two answers must agree with what the emitter actually does, so this is
 * the exact mirror of the shortcuts in thumb_emit_data_processing_mop64 and
 * thumb_emit_shift64_mop -- adding a rule here without adding the matching
 * emitter arm makes codegen read a register nothing wrote. */
static int zh_use_ignores_half(TCCIRState *ir, IRQuadCompact *u, int32_t vreg, int half)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, u);
  IROperand s2 = tcc_ir_op_get_src2(ir, u);
  int m1 = (irop_get_vreg(s1) == vreg) && irop_needs_pair(s1);
  int m2 = (irop_get_vreg(s2) == vreg) && irop_needs_pair(s2);
  if (!m1 && !m2)
    return 1; /* named only in a narrow slot, or not at all: reads no half */

  /* Every shortcut below lives in the 64-bit pair lowering.  A narrow
   * destination sends the same opcode to thumb_emit_data_processing_mop32,
   * which reads the low words outright, so the width that decides is the
   * DEST's -- not the operand's. */
  if (!irop_needs_pair(tcc_ir_op_get_dest(ir, u)))
    return 0;

  switch (u->op)
  {
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    /* The emitter's per-half plan turns the whole half into a copy of the
     * other operand or a constant zero, naming neither source register --
     * but ONLY on the register-register path.  Against a literal the emitter
     * takes its own per-half peephole, and every arm of it but `AND #0` reads
     * rn.  Claiming otherwise made codegen read a register nothing wrote:
     * bug_ll_shift_ptr_clobber and 394_fuzz_barrel_shift_imm_remat_drop both
     * miscompiled on it. */
    return !irop_is_immediate(s2);

  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
    /* The carry lowering turns a zero high word on SRC2 into `adc/sbc rd,rn,#0`
     * and names no register for it.  Only src2 and only the high word: SUB is
     * not commutative, so the emitter never reaches for src1 instead, and a
     * value named on BOTH sides is still read through src1. */
    if (half != ZK_HI || m1)
      return 0;
    return !irop_is_immediate(s2) && irop_needs_pair(tcc_ir_op_get_dest(ir, u));

  case TCCIR_OP_SHL:
    /* A left shift by >= 32 builds its high word out of the source's LOW word
     * alone and forces its low word to zero, so `hi_needed` is 0 and the high
     * word is never materialised.  src2 is the count and is never the pair. */
    if (m2 || half != ZK_HI)
      return 0;
    return irop_is_immediate(s2) && irop_get_imm64_ex(ir, s2) >= 32;

  case TCCIR_OP_SHR:
    /* Mirror image at the value level: a logical right shift by >= 32 derives
     * both result words from the source's high word alone.  SAR is excluded --
     * its tail fills both halves from the sign of the high word, so which
     * words it reads is not the mirror of SHR. */
    if (m2 || half != ZK_LO)
      return 0;
    return irop_is_immediate(s2) && irop_get_imm64_ex(ir, s2) >= 32;

  default:
    return 0;
  }
}

int tcc_ir_opt_zero_half64(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 1 || ir->max_orig_index < 0)
    return 0;

  /* Highest TEMP and VAR position defined anywhere, so the side arrays stay
   * flat. */
  int max_t = -1, max_v = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0)
      continue;
    int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
    if (t == TCCIR_VREG_TYPE_TEMP && p > max_t)
      max_t = p;
    else if (t == TCCIR_VREG_TYPE_VAR && p > max_v)
      max_v = p;
  }
  if (max_t < 0 && max_v < 0)
    return 0;

  ZHState st;
  st.max_t = max_t;
  st.max_v = max_v;
  st.zk_t = tcc_mallocz((size_t)max_t + 2);
  st.def_t = tcc_malloc(((size_t)max_t + 2) * sizeof(int));
  st.zk_v = tcc_mallocz((size_t)max_v + 2);
  st.def_v = tcc_malloc(((size_t)max_v + 2) * sizeof(int));
  st.ok_t = tcc_mallocz((size_t)max_t + 2);
  st.ok_v = tcc_mallocz((size_t)max_v + 2);
  for (int p = 0; p <= max_t; p++)
    st.def_t[p] = -1;
  for (int p = 0; p <= max_v; p++)
    st.def_v[p] = -1;

  /* Census FIRST: which positions have exactly one definition.
   *
   * This cannot be folded into the value pass below, and getting that wrong is
   * how the first version of this pass miscompiled bug_ll_shift_ptr_clobber.
   * There, a value read at instruction 133 still looked single-def, so the AND
   * consuming it recorded "both halves zero"; the second definition came later
   * in the list and cleared the operand's own entry, but the AND's conclusion
   * had already been drawn from it and stayed.  Deciding multi-def up front
   * means an entry only ever goes from "nothing known" to its final value, so
   * an entry read before it is filled under-approximates -- never the reverse.
   *
   * Counted without consulting irop_config[].has_dest: an opcode that writes
   * its destination slot without declaring one would otherwise pass for
   * single-def.  Over-counting only costs optimisation. */
  {
    uint8_t *cnt_t = tcc_mallocz((size_t)max_t + 2);
    uint8_t *cnt_v = tcc_mallocz((size_t)max_v + 2);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p <= max_t && cnt_t[p] < 2)
        cnt_t[p]++;
      else if (t == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= max_v && cnt_v[p] < 2)
        cnt_v[p]++;
    }
    for (int p = 0; p <= max_t; p++)
      st.ok_t[p] = (uint8_t)(cnt_t[p] == 1);
    for (int p = 0; p <= max_v; p++)
      st.ok_v[p] = (uint8_t)(cnt_v[p] == 1);
    /* zh_var_addr_taken walks the whole body, so ask it once per local. */
    for (int i = 0; i < n && max_v >= 0; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p < 0 || p > max_v || !st.ok_v[p])
        continue;
      if (zh_var_addr_taken(ir, vr))
        st.ok_v[p] = 0;
    }
    tcc_free(cnt_t);
    tcc_free(cnt_v);
  }

  /* Pass 1: known-zero halves of every single-def 64-bit TEMP, in program
   * order.  Reading only entries already filled keeps a loop-carried value
   * (whose def follows its use) at "nothing known" rather than wrong. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dv = irop_get_vreg(d);
    if (dv < 0)
      continue;
    int dt = TCCIR_DECODE_VREG_TYPE(dv);
    int p = TCCIR_DECODE_VREG_POSITION(dv);
    uint8_t *zk;
    int *def;
    if (dt == TCCIR_VREG_TYPE_TEMP && p >= 0 && p <= max_t && st.ok_t[p])
    {
      zk = st.zk_t;
      def = st.def_t;
    }
    else if (dt == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= max_v && st.ok_v[p])
    {
      zk = st.zk_v;
      def = st.def_v;
    }
    else
      continue;
    def[p] = i;
    if (!irop_needs_pair(d) || d.is_lval)
      continue;
    zk[p] = (uint8_t)zh_result(ir, &st, q);
  }

  if (!ir->zero_half64)
  {
    ir->zero_half64 = tcc_mallocz((size_t)ir->max_orig_index + 1);
    ir->zero_half64_len = ir->max_orig_index + 1;
  }
  else
    memset(ir->zero_half64, 0, (size_t)ir->zero_half64_len);

  int changes = 0;

  /* Pass 2: per-instruction operand bits. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->orig_index < 0 || q->orig_index >= ir->zero_half64_len)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    uint8_t bits = 0;
    if (irop_needs_pair(s1))
    {
      int k = zh_known(ir, &st, s1);
      if (k & ZK_LO)
        bits |= ZH64_S1_LO;
      if (k & ZK_HI)
        bits |= ZH64_S1_HI;
    }
    if (irop_needs_pair(s2))
    {
      int k = zh_known(ir, &st, s2);
      if (k & ZK_LO)
        bits |= ZH64_S2_LO;
      if (k & ZK_HI)
        bits |= ZH64_S2_HI;
    }
    if (bits)
    {
      ir->zero_half64[q->orig_index] |= bits;
      changes++;
    }
  }

  /* Pass 3: a half that is provably zero AND that every use ignores need not be
   * written.  This is the half of the transform that actually removes
   * instructions; without it the consumer rule only rewrites an ORR as a MOV. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->orig_index < 0 || q->orig_index >= ir->zero_half64_len)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    /* A memory destination owes its full 8 bytes whatever the vreg uses do. */
    if (!irop_needs_pair(d) || d.is_lval)
      continue;
    int32_t dv = irop_get_vreg(d);
    if (dv < 0)
      continue;
    int dt = TCCIR_DECODE_VREG_TYPE(dv);
    int p = TCCIR_DECODE_VREG_POSITION(dv);
    int zk;
    if (dt == TCCIR_VREG_TYPE_TEMP && p >= 0 && p <= max_t && st.ok_t[p] && st.def_t[p] == i)
      zk = st.zk_t[p];
    else if (dt == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= max_v && st.ok_v[p] && st.def_v[p] == i)
      zk = st.zk_v[p];
    else
      continue;
    if (!zk)
      continue;

    int dead = 0;
    for (int half = ZK_LO; half <= ZK_HI; half <<= 1)
    {
      if (!(zk & half))
        continue;
      int all_ignore = 1;
      for (int j = 0; j < n && all_ignore; j++)
      {
        if (j == i)
          continue;
        IRQuadCompact *u = &ir->compact_instructions[j];
        if (u->op == TCCIR_OP_NOP)
          continue;
        if (irop_get_vreg(tcc_ir_op_get_src1(ir, u)) != dv &&
            irop_get_vreg(tcc_ir_op_get_src2(ir, u)) != dv)
          continue;
        all_ignore = zh_use_ignores_half(ir, u, dv, half);
      }
      if (all_ignore)
        dead |= (half == ZK_LO) ? ZH64_D_LO : ZH64_D_HI;
    }
    if (dead)
    {
      LOG_IR_GEN("OPTIMIZE: zero_half64 at i=%d dead=%s%s", i, (dead & ZH64_D_LO) ? "lo" : "",
                 (dead & ZH64_D_HI) ? "hi" : "");
      ir->zero_half64[q->orig_index] |= (uint8_t)dead;
      changes++;
    }
  }

  tcc_free(st.zk_t);
  tcc_free(st.def_t);
  tcc_free(st.zk_v);
  tcc_free(st.def_v);
  tcc_free(st.ok_t);
  tcc_free(st.ok_v);
  return changes;
}

int tcc_ir_opt_zero_half64_ex(IROptCtx *ctx) { return tcc_ir_opt_zero_half64(ctx->ir); }
