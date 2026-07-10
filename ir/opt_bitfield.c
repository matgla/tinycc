/*
 *  TCC IR - Redundant Bitfield Insert/Extract Elimination
 *
 *  Recognises reading back a bitfield value that was just inserted into its
 *  host word, when the host word is otherwise dead (the common "copy a struct
 *  to a local, poke one bitfield, return it" idiom).  After the struct copy is
 *  expanded to register LOAD/STOREs, the in-register insert + immediate
 *  re-extract is pure churn:
 *
 *      L = V  SHL n              ; field value shifted into its position
 *      R = X  AND m   (m < 2^n)  ; host word with the field region cleared
 *      S = L  OR  R              ; insert
 *      D = S  SHR n              ; extract  ==  V   (because R has no bits at
 *                                ;                   or above n, and V < 2^(32-n))
 *
 *  is rewritten to `D = ASSIGN V`.  Copy-prop + DCE then delete the now-dead
 *  OR / SHL / AND.  This is the dominant waste in gcc.c-torture/execute/
 *  20040709-1.c (the fn1 and fn2 families), where GCC keeps the field in a
 *  plain register throughout while TCC round-trips it through the packed
 *  word twice.
 *
 *  Single forward pass over the compact IR.  Restricted to TEMP vregs
 *  (single-assignment), so the reaching definition found by
 *  tcc_ir_find_defining_instruction is unambiguous regardless of control flow.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"

#ifndef LOG_BITFIELD
#ifdef TCC_LOG_BITFIELD
#define LOG_BITFIELD(...) fprintf(stderr, "[BITFIELD] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_BITFIELD(...) ((void)0)
#endif
#endif

/* Compute a conservative superset of the bits that `op` may have set, as a
 * 32-bit mask.  Returns 1 and writes *out on success; 0 if undeterminable
 * (caller must then assume all bits / bail).  Reasons about literal immediates
 * and 32-bit TEMP results defined by AND/SHL/SHR-with-immediate (each a
 * path-independent bound), following ASSIGN copy chains.  before_idx bounds
 * the def search; recursion strictly decreases it, so it terminates. */
static int bf_possible_bits(TCCIRState *ir, IROperand op, int before_idx, uint32_t *out)
{
  if (irop_is_immediate(op) && !op.is_sym)
  {
    *out = (uint32_t)irop_get_imm64_ex(ir, op);
    return 1;
  }
  if (op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  /* This pass runs pre-SSA, so a TEMP can be written on several paths (e.g. an
   * un-lowered `?:` whose arms assign different values).  tcc_ir_find_defining_
   * instruction returns only the nearest preceding def, silently ignoring the
   * other reaching definitions; reasoning from one arm under-reports the bits.
   * longlong seed 111125 / volatile seed 112075: a TEMP was `T<-0` on one arm
   * and a non-zero value on another, and bounding it from the zero arm alone let
   * `(T | C) & 1` fold to `T`.  Only trust a def-derived bound for single-def
   * TEMP values. */
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;
  int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
  if (d < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  IROperand a1 = tcc_ir_op_get_src1(ir, dq);
  IROperand a2 = tcc_ir_op_get_src2(ir, dq);

  if (dq->op == TCCIR_OP_ASSIGN)
    return bf_possible_bits(ir, a1, d, out); /* follow the copy */

  if (irop_get_btype(tcc_ir_op_get_dest(ir, dq)) != IROP_BTYPE_INT32)
    return 0;

  switch (dq->op)
  {
  case TCCIR_OP_AND:
    /* X & m  has bits ⊆ m. */
    if (irop_is_immediate(a2) && !a2.is_sym)
    {
      *out = (uint32_t)irop_get_imm64_ex(ir, a2);
      return 1;
    }
    if (irop_is_immediate(a1) && !a1.is_sym)
    {
      *out = (uint32_t)irop_get_imm64_ex(ir, a1);
      return 1;
    }
    return 0;
  case TCCIR_OP_SHL:
    if (irop_is_immediate(a2) && !a2.is_sym)
    {
      int s = (int)irop_get_imm64_ex(ir, a2);
      if (s >= 0 && s < 32)
      {
        *out = 0xffffffffu << s;
        return 1;
      }
    }
    return 0;
  case TCCIR_OP_SHR:
    if (irop_is_immediate(a2) && !a2.is_sym)
    {
      int s = (int)irop_get_imm64_ex(ir, a2);
      if (s >= 0 && s < 32)
      {
        *out = 0xffffffffu >> s;
        return 1;
      }
    }
    return 0;
  case TCCIR_OP_UBFX:
    /* UBFX Rd, Rn, #lsb, #width  ->  result in [0, 2^width).  src2 packs
     * lsb (bits 0-4) and width (bits 5-9); width 0 encodes 8 (see emitter). */
    if (irop_is_immediate(a2) && !a2.is_sym)
    {
      int param = (int)irop_get_imm64_ex(ir, a2);
      int width = (param >> 5) & 0x1F;
      if (width == 0)
        width = 8;
      *out = (width >= 32) ? 0xffffffffu : (((uint32_t)1u << width) - 1u);
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

/* Is `op` provably a 32-bit unsigned value in [0, 2^bits)? */
static int bf_value_fits_unsigned(TCCIRState *ir, IROperand op, int bits, int before_idx)
{
  if (bits <= 0)
    return 0;
  if (bits >= 32)
    return 1;
  uint32_t pb;
  if (!bf_possible_bits(ir, op, before_idx, &pb))
    return 0;
  return (pb >> bits) == 0;
}

/* Set *src to a copy operand for vreg/immediate `v` (used as the ASSIGN source
 * replacing an extract).  Returns 0 if v is not a usable copy source. */
static int bf_make_copy_src(TCCIRState *ir, IROperand v, int dest_btype, IROperand *src)
{
  if (irop_is_immediate(v) && !v.is_sym)
  {
    *src = v;
    return 1;
  }
  if (v.is_lval)
    return 0;
  int32_t v_vr = irop_get_vreg(v);
  if (v_vr < 0)
    return 0;
  *src = irop_make_vreg(v_vr, dest_btype);
  return 1;
}

/* Return the defining instruction index of TEMP `vr`, following ASSIGN copy
 * chains (`T = ASSIGN T2`) to the real producer.  sl_forward leaves such copies
 * behind when it forwards a stored value through a reloaded local, so the OR
 * insert is reached only after skipping them.  Returns -1 if unresolvable. */
static int bf_real_def(TCCIRState *ir, int32_t vr, int before_idx)
{
  for (int guard = 0; guard < 64; guard++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return -1;
    int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
    if (d < 0)
      return -1;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op != TCCIR_OP_ASSIGN)
      return d;
    IROperand a1 = tcc_ir_op_get_src1(ir, dq);
    if (a1.is_lval)
      return -1;
    vr = irop_get_vreg(a1);
    before_idx = d;
  }
  return -1;
}

int tcc_ir_opt_bitfield_insert_extract(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_AND)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval || irop_get_btype(dest) != IROP_BTYPE_INT32)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* S = the value being extracted, must be a TEMP defined by an OR (insert) */
    IROperand sop = tcc_ir_op_get_src1(ir, q);
    if (sop.is_lval)
      continue;
    int32_t s_vr = irop_get_vreg(sop);
    if (s_vr < 0 || TCCIR_DECODE_VREG_TYPE(s_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int def_or = bf_real_def(ir, s_vr, i);
    if (def_or < 0)
      continue;
    IRQuadCompact *orq = &ir->compact_instructions[def_or];

    /* For an SHR extract, peel an optional outer SHL so the two-shift form
     * `(S SHL a) SHR b` (b >= a) is handled as well as the bare `S SHR b`.
     * This is the canonical unsigned bitfield extract for a field that is not
     * at the top of the word:  `(word << (32-(off+width))) >> (32-width)`.
     * After peeling, `outer_shl` is the `a` and the OR insert is `S`. */
    int outer_shl = 0;
    if (q->op == TCCIR_OP_SHR && orq->op == TCCIR_OP_SHL)
    {
      IROperand sa = tcc_ir_op_get_src2(ir, orq);
      if (irop_is_immediate(sa) && !sa.is_sym)
      {
        int av = (int)irop_get_imm64_ex(ir, sa);
        IROperand inner = tcc_ir_op_get_src1(ir, orq);
        int32_t iv = inner.is_lval ? -1 : irop_get_vreg(inner);
        if (av >= 1 && av <= 31 && iv >= 0 && TCCIR_DECODE_VREG_TYPE(iv) == TCCIR_VREG_TYPE_TEMP)
        {
          int di = bf_real_def(ir, iv, def_or);
          if (di >= 0 && ir->compact_instructions[di].op == TCCIR_OP_OR)
          {
            outer_shl = av;
            def_or = di;
            orq = &ir->compact_instructions[di];
          }
        }
      }
    }
    if (orq->op != TCCIR_OP_OR)
      continue;

    IROperand or_a = tcc_ir_op_get_src1(ir, orq);
    IROperand or_b = tcc_ir_op_get_src2(ir, orq);

    if (q->op == TCCIR_OP_SHR)
    {
      /* Shape A — field extract: the value V lives at bit offset `s_eff` and
       * width `w` inside the OR-inserted word S.  The extract reads exactly the
       * bits `field_window = ((1<<w)-1) << s_eff` (bits below s_eff and at/above
       * s_eff+w are shifted out by `(S SHL outer_shl) SHR b`).  So the result is
       * V provided: the high operand supplies `V SHL s_eff` (V fitting in w
       * bits), and the low operand contributes nothing inside field_window.
       * The bare `S SHR b` case is outer_shl == 0, s_eff == b. */
      IROperand shn = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(shn) || shn.is_sym)
        continue;
      int b = (int)irop_get_imm64_ex(ir, shn);
      if (b < 1 || b > 31)
        continue;
      if (outer_shl > b)
        continue;
      int s_eff = b - outer_shl;
      int w = 32 - b;
      if (w < 1 || w > 31)
        continue;
      uint32_t field_window = ((1u << w) - 1) << s_eff;

      for (int trial = 0; trial < 2; trial++)
      {
        IROperand high = trial ? or_b : or_a;
        IROperand low = trial ? or_a : or_b;

        uint32_t low_bits;
        if (!bf_possible_bits(ir, low, def_or, &low_bits))
          continue;
        if ((low_bits & field_window) != 0)
          continue;

        IROperand v;
        if (s_eff == 0)
        {
          /* field already at bit 0 — high operand is V itself */
          v = high;
        }
        else
        {
          if (high.is_lval)
            continue;
          int32_t h_vr = irop_get_vreg(high);
          if (h_vr < 0 || TCCIR_DECODE_VREG_TYPE(h_vr) != TCCIR_VREG_TYPE_TEMP)
            continue;
          int def_shl = bf_real_def(ir, h_vr, def_or);
          if (def_shl < 0)
            continue;
          IRQuadCompact *shlq = &ir->compact_instructions[def_shl];
          if (shlq->op != TCCIR_OP_SHL)
            continue;
          IROperand shl_n = tcc_ir_op_get_src2(ir, shlq);
          if (!irop_is_immediate(shl_n) || shl_n.is_sym)
            continue;
          if ((int)irop_get_imm64_ex(ir, shl_n) != s_eff)
            continue;
          v = tcc_ir_op_get_src1(ir, shlq);
        }

        if (!bf_value_fits_unsigned(ir, v, w, def_or))
          continue;

        IROperand new_src;
        if (!bf_make_copy_src(ir, v, irop_get_btype(dest), &new_src))
          continue;

        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        LOG_BITFIELD("@%d: ((V SHL %d) | low) extract folded to field value (a=%d b=%d w=%d)", i, s_eff, outer_shl,
                     b, w);
        changes++;
        break;
      }
    }
    else /* TCCIR_OP_AND */
    {
      /* Shape B — masked extract: (lowpart | other) & m == lowpart, when
       * `other` has no bits in m and `lowpart` has no bits outside m.  Covers
       * bottom/low bitfields whose read-back is `word & field_mask`. */
      IROperand mop = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(mop) || mop.is_sym)
        continue;
      uint32_t m = (uint32_t)irop_get_imm64_ex(ir, mop);
      if (m == 0 || m == 0xffffffffu)
        continue; /* zero / identity handled elsewhere */

      for (int trial = 0; trial < 2; trial++)
      {
        IROperand lowpart = trial ? or_b : or_a;
        IROperand other = trial ? or_a : or_b;

        uint32_t low_bits, other_bits;
        if (!bf_possible_bits(ir, lowpart, def_or, &low_bits))
          continue;
        if ((low_bits & ~m) != 0) /* lowpart must stay within the mask */
          continue;
        if (!bf_possible_bits(ir, other, def_or, &other_bits))
          continue;
        if ((other_bits & m) != 0) /* other must contribute nothing in the mask */
          continue;

        IROperand new_src;
        if (!bf_make_copy_src(ir, lowpart, irop_get_btype(dest), &new_src))
          continue;

        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        LOG_BITFIELD("@%d: (OR@%d) & %#x folded to low field value", i, def_or, m);
        changes++;
        break;
      }
    }
  }

  return changes;
}

int tcc_ir_opt_bitfield_insert_extract_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_bitfield_insert_extract(ctx->ir);
}

/* ============================================================================
 * Bitfield INSERT -> ARM BFI  (tcc_ir_opt_bitfield_insert_to_bfi)
 * ============================================================================
 *
 * The complement of the extract fold above.  A bitfield poke whose result is
 * *observed* (stored back to a global, returned, ...) leaves a register insert
 * that the extract fold cannot collapse:
 *
 *     Vsh = V SHL #lsb              ; field value shifted into position (lsb>0)
 *     R   = W AND #clearmask        ; host word, field region cleared
 *     S   = Vsh OR R                ; insert  ->  stored / returned
 *
 * where clearmask = ~(((1<<width)-1) << lsb) and V < 2^width.  TCC emits this as
 * `ldr <clearmask>; and; orr` (3 instructions + a literal-pool word); ARM does
 * the whole insert in one: `BFI Rd, V, #lsb, #width` with Rd preset to W.  This
 * is the `bfi` GCC uses for `s.k += x` style global-bitfield RMW (fn3* in
 * gcc.c-torture/execute/20040709-2.c).  When lsb==0 the SHL is absent and the
 * value operand is V directly.
 *
 * The rewrite is a pure algebraic identity — (W & ~field) | (V<<lsb) == BFI for
 * a contiguous `field` at `lsb` with V < 2^width — so it is correct whenever the
 * gates hold, independent of whether the source was "really" a bitfield.  It is
 * provably non-increasing: it replaces 3 instructions (+ pool word) with at most
 * 2 (an optional reg move + the BFI).
 *
 * Rewrites the OR in place into BFI(dest=S, src1=W, src2=V); lsb/width go into
 * ir->bfi_params[orig_index].  NOPs the AND (and the SHL when present), each
 * required single-use so the removal drops exactly those instructions.  Must run
 * BEFORE tcc_ir_barrel_shift_fusion (which would fold the SHL into the OR).
 */
/* True if `imm` is encodable as an ARMv7-M (Thumb-2) modified immediate — the
 * AND/BIC/MOV can then take it directly, no literal-pool load.  Boolean mirror
 * of th_pack_const() (arch/arm/thumb/thumb.c); replicated here to keep the IR
 * layer free of backend headers. */
static int bf_thumb_modified_imm_ok(uint32_t imm)
{
  if ((imm & 0xffffff00u) == 0)
    return 1; /* 0x000000XY */
  if (!(imm & 0xff00ff00u) && (imm >> 16) == (imm & 0xffu))
    return 1; /* 0x00XY00XY */
  if (!(imm & 0x00ff00ffu) && ((imm >> 16) & 0xff00u) == (imm & 0xff00u))
    return 1; /* 0xXY00XY00 */
  if ((imm & 0xffffu) == ((imm >> 16) & 0xffffu) && ((imm >> 8) & 0xffu) == (imm & 0xffu))
    return 1; /* 0xXYXYXYXY */
  for (uint32_t j = 0; j <= 23; j++) /* 8-bit value (MSB set) rotated right */
  {
    uint32_t mask = 0xFF000000u >> j;
    uint32_t one = 0x80000000u >> j;
    if ((imm & one) == one && (imm & ~mask) == 0)
      return 1;
  }
  return 0;
}

int tcc_ir_opt_bitfield_insert_to_bfi(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *orq = &ir->compact_instructions[i];
    if (orq->op != TCCIR_OP_OR || orq->is_jump_target)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, orq);
    if (dest.is_lval || irop_get_btype(dest) != IROP_BTYPE_INT32)
      continue;

    IROperand o1 = tcc_ir_op_get_src1(ir, orq);
    IROperand o2 = tcc_ir_op_get_src2(ir, orq);

    /* One operand is the cleared host word (AND #clearmask); the other is the
     * field value (SHL #lsb, or for lsb==0 the value itself).  Try both. */
    for (int swap = 0; swap < 2 && orq->op == TCCIR_OP_OR; swap++)
    {
      IROperand and_side = swap ? o2 : o1;
      IROperand val_side = swap ? o1 : o2;

      /* --- AND side: W & clearmask --- */
      if (and_side.is_lval || !irop_has_vreg(and_side))
        continue;
      int32_t and_vr = irop_get_vreg(and_side);
      if (and_vr < 0 || TCCIR_DECODE_VREG_TYPE(and_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int and_idx = tcc_ir_find_defining_instruction(ir, and_vr, i);
      if (and_idx < 0 || ir->compact_instructions[and_idx].op != TCCIR_OP_AND)
        continue;
      IRQuadCompact *andq = &ir->compact_instructions[and_idx];
      IROperand mop = tcc_ir_op_get_src2(ir, andq);
      if (!irop_is_immediate(mop) || mop.is_sym)
        continue;
      uint32_t clearmask = (uint32_t)irop_get_imm64_ex(ir, mop);
      uint32_t fieldmask = ~clearmask;
      if (fieldmask == 0)
        continue;
      int lsb_field = __builtin_ctz(fieldmask);
      int width = __builtin_popcount(fieldmask);
      if (lsb_field + width > 32 ||
          ((((uint64_t)1u << width) - 1) << lsb_field) != fieldmask)
        continue; /* not a single contiguous run */

      /* Non-increasing gate 1 (mirrors the AND->UBFX lever): the clearmask must
       * NOT be a Thumb-2 modified immediate.  TCC has no BIC lowering, so an
       * unencodable clearmask costs 2 insns to apply (`mvn;and` or `ldr;and`);
       * the original insert is then `<clear x2>; orr` >= 3, and BFI is at most
       * `mov; bfi` (2, or 3 in the rare value/dest-coalesced case) — never more.
       * An encodable clearmask clears in 1 insn (original 2), where BFI's
       * two-address mov could break even or regress, so skip it. */
      if (bf_thumb_modified_imm_ok(clearmask))
        continue;

      IROperand word_op = tcc_ir_op_get_src1(ir, andq);
      if (word_op.is_lval || !irop_has_vreg(word_op))
        continue;
      int32_t word_vr = irop_get_vreg(word_op);

      /* Non-increasing gate 2: if the host word provably has NO bits in the
       * field region, the clear is a no-op that gets DCE'd, so the original is
       * just a single barrel-folded `orr` (1 insn) — BFI (2) would regress
       * (e.g. pr110166-1, where the word is `i & 1` and the field is bit 1).
       * Only skip when we can PROVE the word is field-clear; an unknown word
       * (e.g. a plain memory load) is assumed to need the insert. */
      {
        uint32_t word_pb;
        if (bf_possible_bits(ir, word_op, and_idx, &word_pb) && (word_pb & fieldmask) == 0)
          continue;
      }

      /* --- value side: SHL #lsb (lsb>0) or the value itself (lsb==0) --- */
      if (val_side.is_lval || !irop_has_vreg(val_side))
        continue;
      int32_t val_vr = irop_get_vreg(val_side);
      int lsb;
      IROperand value_op;
      int shl_idx = -1;
      int32_t shl_res_vr = -1;
      int vdef = (val_vr >= 0 && TCCIR_DECODE_VREG_TYPE(val_vr) == TCCIR_VREG_TYPE_TEMP)
                     ? tcc_ir_find_defining_instruction(ir, val_vr, i)
                     : -1;
      if (vdef >= 0 && ir->compact_instructions[vdef].op == TCCIR_OP_SHL)
      {
        IRQuadCompact *shlq = &ir->compact_instructions[vdef];
        IROperand sn = tcc_ir_op_get_src2(ir, shlq);
        if (!irop_is_immediate(sn) || sn.is_sym)
          continue;
        lsb = (int)irop_get_imm64_ex(ir, sn);
        IROperand vsrc = tcc_ir_op_get_src1(ir, shlq);
        if (vsrc.is_lval || !irop_has_vreg(vsrc))
          continue;
        value_op = vsrc;
        shl_idx = vdef;
        shl_res_vr = val_vr;
      }
      else
      {
        lsb = 0;
        value_op = val_side;
      }
      if (lsb != lsb_field || lsb < 0 || lsb > 31 || width < 1)
        continue;

      /* The host word and the field value must be distinct vregs (BFI Rd,Rn
       * needs Rn != the word base; a self-insert is degenerate). */
      int32_t value_vr = irop_get_vreg(value_op);
      if (value_op.is_lval || value_vr < 0 || value_vr == word_vr)
        continue;

      /* BFI inserts only the low `width` bits of the value; the ORR also OR'd
       * the shifted value into non-field bits, so equivalence needs V<2^width. */
      if (!bf_value_fits_unsigned(ir, value_op, width, i))
        continue;

      /* Single-use gates: NOPing the AND / SHL must drop exactly those insns. */
      if (!tcc_ir_vreg_has_single_use(ir, and_vr, and_idx))
        continue;
      if (shl_idx >= 0 && !tcc_ir_vreg_has_single_use(ir, shl_res_vr, shl_idx))
        continue;

      /* BFI re-reads W (read by the AND at and_idx) and V (read by the SHL at
       * shl_idx) at the OR site instead.  Each operand must be unmodified from
       * its original read point to the OR, and no control-flow edge may split
       * the window.  The windows are per-operand: W's own defining load can sit
       * before the AND (and after the SHL), and must NOT count as a redefinition
       * of W — so check W only in (and_idx, i) and V only in (shl_idx, i). */
      int lo = and_idx;
      if (shl_idx >= 0 && shl_idx < lo)
        lo = shl_idx;
      int safe = 1;
      for (int j = lo + 1; j < i && safe; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
            jq->op == TCCIR_OP_SWITCH_TABLE || jq->is_jump_target)
        {
          safe = 0;
          break;
        }
        if (irop_config[jq->op].has_dest)
        {
          IROperand jd = tcc_ir_op_get_dest(ir, jq);
          if (irop_has_vreg(jd))
          {
            int32_t jdv = irop_get_vreg(jd);
            if ((j > and_idx && jdv == word_vr) || (shl_idx >= 0 && j > shl_idx && jdv == value_vr))
            {
              safe = 0;
              break;
            }
          }
        }
      }
      if (!safe)
        continue;

      /* Side-array keyed by orig_index, allocated lazily (like barrel_shifts). */
      if (!ir->bfi_params) {
        ir->bfi_params = tcc_mallocz((size_t)(ir->max_orig_index + 1) * sizeof(uint16_t));
        ir->bfi_params_len = ir->max_orig_index + 1;
      }

      orq->op = TCCIR_OP_BFI;
      tcc_ir_set_src1(ir, i, word_op);
      tcc_ir_set_src2(ir, i, value_op);
      ir->bfi_params[orq->orig_index] = (uint16_t)((lsb & 0xFF) | ((width & 0xFF) << 8));
      andq->op = TCCIR_OP_NOP;
      if (shl_idx >= 0)
        ir->compact_instructions[shl_idx].op = TCCIR_OP_NOP;
      changes++;
      LOG_BITFIELD("INSERT->BFI @%d: lsb=%d width=%d (AND@%d SHL@%d)", i, lsb, width, and_idx, shl_idx);
    }
  }

  return changes;
}
