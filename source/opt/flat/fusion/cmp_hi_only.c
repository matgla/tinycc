/*
 *  TCC IR - 64-bit compare against a constant with a zero low half
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * A 64-bit compare costs two instructions and a scratch register: `cmp rlo,
 * #imm_lo` for the low words, then `sbcs rt, rhi, #imm_hi` to fold in the
 * borrow.  Against a constant whose LOW 32 bits are zero, and for a STRICT
 * order test, the low words cannot decide the answer at all:
 *
 *     x <u C   <=>   x_hi <u C_hi  ||  (x_hi == C_hi && x_lo <u 0)
 *                                                       ~~~~~~~~~ never true
 *     x >=u C  <=>   the negation of the above
 *
 * and the same for the signed pair, where a 64-bit compare orders the high
 * words signed and the low words unsigned.  So the whole compare is one
 * `cmp rhi, #imm_hi` -- one instruction, no borrow, no scratch.
 *
 * This is the same proof tcc_ir_opt_cmp_narrow_64 applies, moved from the
 * VALUE to the CONSTANT.  That pass narrows to 32 bits when both operands are
 * known to fit in their low words; this one keeps the value 64-bit and throws
 * away the half the constant makes irrelevant.  The two are disjoint: a
 * constant with a zero low half and a non-zero high half never satisfies
 * cmp_narrow_64's "high word is provably zero" test.
 *
 * NON-strict tests are excluded and the asymmetry is the point: `x >u C` is
 * true when x_hi == C_hi and x_lo is merely non-zero, which a high-word
 * compare cannot see.  Only <, >= (constant on the right) survive.  EQ/NE are
 * excluded for the same reason and are already handled by codegen's cmp_eq64
 * peephole.
 *
 * Where sfp_round_pack_double's two normalisation guards --
 * `while (mant >= (DOUBLE_NORM_BIT << 1))` and `while (mant < DOUBLE_NORM_BIT)`
 * -- are each a compare against 1<<56 / 1<<55.  They run 2,310 times over 996
 * __aeabi_dadd calls, so this is ~2.3 instructions and a register per call of
 * the hottest routine in the soft-float library.
 *
 * PURE ANNOTATION, no IR mutation, and it must run LAST for the same reason
 * zero_half64 does: the verdict is about the FINAL instruction stream.  A pass
 * that rewrote the compare's operands afterwards -- swapping them, or
 * substituting a different immediate -- would leave the annotation describing
 * a compare that no longer exists, and codegen would silently drop a half that
 * had become load-bearing.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

/* Bit 6 of the byte tcc_ir_zero_half64_at returns -- kept in sync with the
 * decoder in thumb_emit_data_processing_mop64, which reads it from bit 24 of
 * its `barrel_shift` word.  It shares the array with zero_half64.c's bits
 * because both answer the same question ("which halves does this instruction
 * really need?") for the same consumer; it does NOT share the pass, because
 * zero_half64 clears the array when it runs and this must be applied after. */
#define ZH64_CMP_HI 0x40u

/* Conditions under which a strict high-word compare gives the same answer as
 * the full 64-bit one, with the constant as src2.  Both orders are covered:
 * the 64-bit lowering compares high words signed for LT/GE and unsigned for
 * ULT/UGE, and a single high-word CMP feeds exactly the same flags to exactly
 * the same branch. */
static int chi_cond_ok(int tok)
{
  return tok == TOK_ULT || tok == TOK_UGE || tok == TOK_LT || tok == TOK_GE;
}

int tcc_ir_opt_cmp_hi_only(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || ir->max_orig_index < 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    if (q->orig_index < 0)
      continue;

    if (tcc_ir_op_src1_btype(ir, q) != IROP_BTYPE_INT64)
      continue;
    if (!tcc_ir_op_src2_is_imm(ir, q) || tcc_ir_op_src2_btype(ir, q) != IROP_BTYPE_INT64)
      continue;
    /* The low half is what makes the transform exact.  Anything else and the
     * low words still decide ties. */
    if (((uint64_t)tcc_ir_op_src2_imm(ir, q) & 0xFFFFFFFFull) != 0)
      continue;

    /* Every condition applied to these flags must be one of the four.  The
     * scan mirrors codegen's own notion of a flag consumer: NOPs and ASSIGNs
     * are transparent (an ASSIGN lowers to `mov`, which preserves flags, and
     * phi resolution schedules them between the compare and its branch), and
     * anything else ends the run. */
    int readers = 0;
    int all_ok = 1;
    for (int j = i + 1; j < n && all_ok; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP || qj->op == TCCIR_OP_ASSIGN)
        continue;
      IROperand c;
      if (qj->op == TCCIR_OP_SETIF || qj->op == TCCIR_OP_JUMPIF)
        c = tcc_ir_op_get_src1(ir, qj);
      else if (qj->op == TCCIR_OP_SELECT)
        c = tcc_ir_op_get_cond(ir, qj);
      else
        break;
      if (!irop_is_immediate(c) || !chi_cond_ok((int)irop_get_imm64_ex(ir, c)))
      {
        all_ok = 0;
        break;
      }
      readers++;
    }
    if (!all_ok || readers == 0)
      continue;

    /* Share zero_half64's side table.  It has already run and either allocated
     * and filled the array or left it NULL (its pass can be disabled on its
     * own); allocate but never clear, or its verdicts go with it. */
    if (!ir->zero_half64)
    {
      ir->zero_half64 = tcc_mallocz((size_t)ir->max_orig_index + 1);
      ir->zero_half64_len = ir->max_orig_index + 1;
    }
    if (q->orig_index >= ir->zero_half64_len)
      continue;

    LOG_IR_GEN("OPTIMIZE: cmp_hi_only at i=%d (constant low half is zero)", i);
    ir->zero_half64[q->orig_index] |= ZH64_CMP_HI;
    changes++;
  }

  return changes;
}
