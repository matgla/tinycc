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
 * The information needed to delete all of it is one bit per half (derived from
 * per-word may-be-set masks, so shifts that move only zeros across the word
 * boundary keep a half known):
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

/* What the pass tracks per value: the bits each word may have set.  A word
 * whose mask is 0 is the constant zero.  One bit per word was not enough:
 * `(n & 0xff) << 16` and `(n >> 8) << 8` of a zero-extended n both keep a zero
 * high word, but only the masks show that the bits shifted across the word
 * boundary are zero (IntegerContext(u16).hash's key expansion). */
typedef struct
{
  uint32_t lo, hi;
} ZHMask;

#define ZH_ALL ((ZHMask){0xFFFFFFFFu, 0xFFFFFFFFu})

static int zh_mask_bits(ZHMask m)
{
  return (m.lo == 0 ? ZK_LO : 0) | (m.hi == 0 ? ZK_HI : 0);
}

typedef struct
{
  ZHMask *zk_t;   /* per TEMP position: may-be-set bits of each word */
  int *def_t;     /* per TEMP position: index of its single def, -1 / -2 */
  int max_t;
  ZHMask *zk_v;   /* the same for named locals (VAR) */
  int *def_v;
  uint8_t *ok_t;   /* per TEMP position: exactly one definition */
  uint8_t *ok_v;   /* per VAR position: one definition and no LEA */
  int max_v;
} ZHState;

/* A named local needs one guard a temp does not: its address must never be
 * handed out, or a store through a pointer changes it behind this scan's back.
 * (The single-definition requirement is common to both and is settled by the
 * def census in tcc_ir_opt_zero_half64.)  Same rule tcc_ir_opt_cmp_narrow_64
 * applies to a local.  Clears ok_v for every VAR position that is LEA'd, in one
 * walk: asking per local walked the body once per def (101_cleanup's 65536
 * address-taken locals: 7.5 s). */
static void zh_drop_addr_taken_vars(TCCIRState *ir, ZHState *st)
{
  for (int k = 0; k < ir->next_instruction_index; k++)
  {
    IRQuadCompact *qk = &ir->compact_instructions[k];
    if (qk->op != TCCIR_OP_LEA)
      continue;
    int32_t vr = tcc_ir_op_src1_vreg(ir, qk);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (p >= 0 && p <= st->max_v)
      st->ok_v[p] = 0;
  }
}

/* A volatile local owes every byte of every store (C11 6.7.3p7: each access
 * is a side effect), and a local the allocator keeps in memory (lvalue use) is
 * read through its slot, not as a register pair.  Neither has halves this pass
 * may call known or dead: `volatile uint64_t n = k; return n << 9;` lost the
 * slot's high-word store while the shift still loaded it. */
static void zh_drop_memory_vars(TCCIRState *ir, ZHState *st)
{
  for (int p = 0; p <= st->max_v; p++)
  {
    if (!st->ok_v[p])
      continue;
    IRLiveInterval *li = tcc_ir_try_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p));
    if (!li || li->is_volatile || li->is_lvalue || li->addrtaken)
      st->ok_v[p] = 0;
  }
}

/* The masks recorded for `vr`, or "anything" when it has no entry. */
static ZHMask zh_lookup(const ZHState *st, int32_t vr)
{
  int t = TCCIR_DECODE_VREG_TYPE(vr);
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_TEMP)
    return (p >= 0 && p <= st->max_t && st->ok_t[p]) ? st->zk_t[p] : ZH_ALL;
  if (t == TCCIR_VREG_TYPE_VAR)
    return (p >= 0 && p <= st->max_v && st->ok_v[p]) ? st->zk_v[p] : ZH_ALL;
  return ZH_ALL;
}

/* The halves of `op` that are provably zero.
 *
 * A non-pair operand is the important free case: every path that widens one
 * into a 64-bit slot zero-extends it (thumb_emit_data_processing_mop64 does so
 * with an explicit `mov rn_hi, #0`), so its high word is zero by construction.
 * Signed narrow values reach a 64-bit op through an explicit sign-extending
 * ASSIGN, which is a pair by then, so this never mistakes a sign fill for a
 * zero. */
static ZHMask zh_mask(TCCIRState *ir, const ZHState *st, IROperand op)
{
  if (irop_is_immediate(op))
  {
    uint64_t v = (uint64_t)irop_get_imm64_ex(ir, op);
    return (ZHMask){(uint32_t)v, (uint32_t)(v >> 32)};
  }
  if (!irop_needs_pair(op))
    return (ZHMask){0xFFFFFFFFu, 0};
  /* A pair read through memory is whatever the memory holds; the vreg is
   * only its address. */
  if (op.is_lval)
    return ZH_ALL;
  return zh_lookup(st, irop_get_vreg(op));
}

static int zh_known(TCCIRState *ir, const ZHState *st, IROperand op)
{
  return zh_mask_bits(zh_mask(ir, st, op));
}

/* The bits each word of a 64-bit result may have set, from its operands'.
 * Only the ops whose per-word behaviour is exact are listed; everything else
 * returns "anything", which is always safe. */
static ZHMask zh_result(TCCIRState *ir, const ZHState *st, IRQuadCompact *q)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  ZHMask a = zh_mask(ir, st, s1);

  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_ZEXT:
    /* Widening or copying assign; a truncating one has a non-pair dest and is
     * never asked about here.  A narrow source reports a zero high word on its
     * own (it is zero-extended), which is exactly what ZEXT guarantees. */
    return a;

  case TCCIR_OP_AND:
  {
    ZHMask b = zh_mask(ir, st, s2);
    return (ZHMask){a.lo & b.lo, a.hi & b.hi};
  }

  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  {
    ZHMask b = zh_mask(ir, st, s2);
    return (ZHMask){a.lo | b.lo, a.hi | b.hi};
  }

  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  {
    if (!irop_is_immediate(s2))
      return ZH_ALL;
    int64_t k = irop_get_imm64_ex(ir, s2);
    if (k < 0)
      return ZH_ALL;
    uint64_t m = ((uint64_t)a.hi << 32) | a.lo;
    /* An arithmetic right shift fills from the sign bit: ones wherever the
     * sign may be set. */
    uint64_t fill = (q->op == TCCIR_OP_SAR && (a.hi & 0x80000000u)) ? ~0ull : 0;
    if (k >= 64)
      m = (q->op == TCCIR_OP_SAR) ? fill : 0; /* UB in C; the emitter produces 0 / a sign fill */
    else if (q->op == TCCIR_OP_SHL)
      m <<= k;
    else
      m = (m >> k) | (k ? fill << (64 - k) : 0);
    return (ZHMask){(uint32_t)m, (uint32_t)(m >> 32)};
  }

  default:
    return ZH_ALL;
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
  /* An lvalue operand naming the vreg reads memory: the VAR's own slot (a
   * volatile or memory-resident local) or what the vreg points at.  zh_mask
   * reports nothing known for it, so pass 2 tells the emitter nothing, and the
   * emitter loads every word -- no half of it is ignored. */
  if ((irop_get_vreg(s1) == vreg && s1.is_lval) || (irop_get_vreg(s2) == vreg && s2.is_lval))
    return 0;
  int m1 = (irop_get_vreg(s1) == vreg) && irop_needs_pair(s1);
  int m2 = (irop_get_vreg(s2) == vreg) && irop_needs_pair(s2);
  /* A word-typed operand naming the pair reads its LOW register (machine
   * operands take their width from the operand, not the vreg). */
  int n1 = (irop_get_vreg(s1) == vreg) && !irop_needs_pair(s1);
  int n2 = (irop_get_vreg(s2) == vreg) && !irop_needs_pair(s2);
  if ((n1 || n2) && half == ZK_LO)
    return 0;
  if (!m1 && !m2)
    return 1; /* named only in a narrow slot (low word, handled above), or not at all */

  /* Every shortcut below lives in the 64-bit pair lowering.  A narrow
   * destination sends the same opcode to thumb_emit_data_processing_mop32,
   * which reads the low words outright, so the width that decides is the
   * DEST's -- not the operand's. */
  if (!tcc_ir_op_dest_needs_pair(ir, u))
    return 0;

  switch (u->op)
  {
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    /* The emitter's per-half plan turns the whole half into a copy of the
     * other operand or a constant zero, naming neither source register.
     * Against a literal it is src1's zero half that turns the result half into
     * the literal's half (OR/XOR) or zero (AND) -- HALF_CONST / HALF_ZERO in
     * the immediate arm of thumb_emit_data_processing_mop64.  A literal src1
     * is not covered: the emitter keys that plan on src1's bits only, and
     * claiming more made codegen read a register nothing wrote
     * (bug_ll_shift_ptr_clobber, 394_fuzz_barrel_shift_imm_remat_drop). */
    if (irop_is_immediate(s2))
      return m1 && !m2;
    return 1;

  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
    /* The carry lowering turns a zero high word on SRC2 into `adc/sbc rd,rn,#0`
     * and names no register for it.  Only src2 and only the high word: SUB is
     * not commutative, so the emitter never reaches for src1 instead, and a
     * value named on BOTH sides is still read through src1. */
    if (half != ZK_HI || m1)
      return 0;
    return !irop_is_immediate(s2) && tcc_ir_op_dest_needs_pair(ir, u);

  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    /* A shift by a literal of a source whose high word is zero is built from
     * the low word alone, whatever the count (thumb_emit_shift64_mop's
     * `s1_hi_zero` arm; a SAR of it is a logical shift).  src2 is the count and
     * is never the pair. */
    if (m2 || !irop_is_immediate(s2))
      return 0;
    if (half == ZK_HI)
      return 1;
    /* A logical right shift by >= 32 derives both result words from the
     * source's high word alone.  SHL by >= 32 still reads the low word, and
     * SAR's tail fills from the sign of the high word. */
    return u->op == TCCIR_OP_SHR && irop_get_imm64_ex(ir, s2) >= 32;

  default:
    return 0;
  }
}

/* Does the use at `u` leave the high word of `vreg` unread whatever it holds?
 * Only a read of the low word alone qualifies: a word-typed operand naming the
 * pair (its low register), or a truncating ASSIGN, which
 * tcc_gen_machine_assign_mop_ex lowers from the source's low half.  This is
 * what lets a literal-derived high word nobody reads die (`(uint32_t)(k ^ C)`
 * kept `ldr hi, =C.hi`); every other rule needs the half known zero. */
static int zh_use_never_reads_hi(TCCIRState *ir, IRQuadCompact *u, int32_t vreg)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, u);
  IROperand s2 = tcc_ir_op_get_src2(ir, u);
  if ((irop_get_vreg(s1) == vreg && s1.is_lval) || (irop_get_vreg(s2) == vreg && s2.is_lval))
    return 0;
  int m1 = (irop_get_vreg(s1) == vreg) && irop_needs_pair(s1);
  int m2 = (irop_get_vreg(s2) == vreg) && irop_needs_pair(s2);
  if (!m1 && !m2)
    return 1;
  return u->op == TCCIR_OP_ASSIGN && m1 && !tcc_ir_op_dest_needs_pair(ir, u) && !tcc_ir_op_dest_is_lval(ir, u);
}

/* Key of a TEMP or VAR in zh_build_uses' lists, or -1. */
static int zh_use_key(const ZHState *st, int32_t vr)
{
  if (vr < 0)
    return -1;
  int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p <= st->max_t)
    return p;
  if (t == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= st->max_v)
    return st->max_t + 1 + p;
  return -1;
}

/* For each TEMP and VAR, the non-NOP instructions naming it in src1, src2 or
 * a multiply-accumulate's accumulator, in program order: uses[start[k] ..
 * start[k + 1]).  Pass 3 asks this of every candidate def; scanning the whole
 * function each time was quadratic in a long 64-bit basic block. */
static void zh_build_uses(TCCIRState *ir, const ZHState *st, int **start_out, int **uses_out)
{
  int n = ir->next_instruction_index, nk = st->max_t + 1 + st->max_v + 1;
  int *start = tcc_mallocz(((size_t)nk + 1) * sizeof(int));
  for (int pass = 0; pass < 2; pass++)
  {
    int *uses = pass ? tcc_malloc(((size_t)start[nk] + 1) * sizeof(int)) : NULL;
    int *fill = pass ? tcc_malloc(((size_t)nk + 1) * sizeof(int)) : NULL;
    if (pass)
      memcpy(fill, start, ((size_t)nk + 1) * sizeof(int));
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *u = &ir->compact_instructions[j];
      if (u->op == TCCIR_OP_NOP)
        continue;
      int k[3] = {zh_use_key(st, tcc_ir_op_src1_vreg(ir, u)), zh_use_key(st, tcc_ir_op_src2_vreg(ir, u)),
                  tcc_ir_op_is_mac(u->op) ? zh_use_key(st, tcc_ir_op_accum_vreg(ir, u)) : -1};
      for (int a = 0; a < 3; a++)
      {
        if (k[a] < 0 || (a >= 1 && k[a] == k[0]) || (a == 2 && k[2] == k[1]))
          continue;
        if (pass)
          uses[fill[k[a]]++] = j;
        else
          start[k[a] + 1]++;
      }
    }
    if (!pass)
      for (int x = 0; x < nk; x++)
        start[x + 1] += start[x];
    else
    {
      tcc_free(fill);
      *uses_out = uses;
    }
  }
  *start_out = start;
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
    int32_t vr = tcc_ir_op_dest_vreg(ir, q);
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
  st.zk_t = tcc_malloc(((size_t)max_t + 2) * sizeof(ZHMask));
  st.def_t = tcc_malloc(((size_t)max_t + 2) * sizeof(int));
  st.zk_v = tcc_malloc(((size_t)max_v + 2) * sizeof(ZHMask));
  st.def_v = tcc_malloc(((size_t)max_v + 2) * sizeof(int));
  st.ok_t = tcc_mallocz((size_t)max_t + 2);
  st.ok_v = tcc_mallocz((size_t)max_v + 2);
  for (int p = 0; p <= max_t; p++)
  {
    st.def_t[p] = -1;
    st.zk_t[p] = ZH_ALL;
  }
  for (int p = 0; p <= max_v; p++)
  {
    st.def_v[p] = -1;
    st.zk_v[p] = ZH_ALL;
  }

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
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
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
    /* A value in a graph-coalesced class shares its register with the other
     * members, and the copies between them are gone: their defs write it and
     * their uses read it under other names.  Its halves are neither known from
     * its own def nor dead for want of readers of its own name -- expr_preprocess
     * in tccpp.c lost `c = 0` (an int64 whose only reader, the phi copy into
     * `c`, had been coalesced away), so `#if defined X` was true for every X
     * in the native tcc. */
    for (int k = 0; k < ir->ls.next_interval_index; k++)
    {
      const LSLiveInterval *li = &ir->ls.intervals[k];
      if (!li->co_member)
        continue;
      int32_t vr = (int32_t)li->vreg;
      int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p <= max_t)
        st.ok_t[p] = 0;
      else if (t == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= max_v)
        st.ok_v[p] = 0;
    }
    if (max_v >= 0)
    {
      zh_drop_addr_taken_vars(ir, &st);
      zh_drop_memory_vars(ir, &st);
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
    int32_t dv = tcc_ir_op_dest_vreg(ir, q);
    if (dv < 0)
      continue;
    int dt = TCCIR_DECODE_VREG_TYPE(dv);
    int p = TCCIR_DECODE_VREG_POSITION(dv);
    ZHMask *zk;
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
    if (!tcc_ir_op_dest_needs_pair(ir, q) || tcc_ir_op_dest_is_lval(ir, q))
      continue;
    zk[p] = zh_result(ir, &st, q);
  }

  if (!ir->zero_half64)
  {
    ir->zero_half64 = tcc_mallocz((size_t)ir->max_orig_index + 1);
    ir->zero_half64_len = ir->max_orig_index + 1;
  }
  else
    memset(ir->zero_half64, 0, (size_t)ir->zero_half64_len);

  int changes = 0;
  int *use_start = NULL, *uses = NULL; /* zh_build_uses, when pass 3 first asks */

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
   * instructions; without it the consumer rule only rewrites an ORR as a MOV.
   * A high word that no use reads at all (zh_use_never_reads_hi) need not be
   * written either, whatever it holds. */
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
      zk = zh_mask_bits(st.zk_t[p]);
    else if (dt == TCCIR_VREG_TYPE_VAR && p >= 0 && p <= max_v && st.ok_v[p] && st.def_v[p] == i)
      zk = zh_mask_bits(st.zk_v[p]);
    else
      continue;
    if (!use_start)
      zh_build_uses(ir, &st, &use_start, &uses);
    int key = zh_use_key(&st, dv);
    int dead = 0;
    for (int half = ZK_LO; half <= ZK_HI; half <<= 1)
    {
      /* A half not known zero dies only when no use reads it at all. */
      const int unread_only = !(zk & half);
      if (unread_only && half != ZK_HI)
        continue;
      int all_ignore = 1;
      for (int x = use_start[key]; x < use_start[key + 1] && all_ignore; x++)
      {
        int j = uses[x];
        if (j == i)
          continue;
        IRQuadCompact *u = &ir->compact_instructions[j];
        if (tcc_ir_op_is_mac(u->op) && tcc_ir_op_accum_vreg(ir, u) == dv &&
            (half != ZK_HI || irop_needs_pair(tcc_ir_op_get_accum(ir, u))))
        {
          all_ignore = 0;
          break;
        }
        if (tcc_ir_op_src1_vreg(ir, u) != dv &&
            tcc_ir_op_src2_vreg(ir, u) != dv)
          continue;
        all_ignore = unread_only ? zh_use_never_reads_hi(ir, u, dv) : zh_use_ignores_half(ir, u, dv, half);
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

  tcc_free(use_start);
  tcc_free(uses);
  tcc_free(st.zk_t);
  tcc_free(st.def_t);
  tcc_free(st.zk_v);
  tcc_free(st.def_v);
  tcc_free(st.ok_t);
  tcc_free(st.ok_v);
  return changes;
}
