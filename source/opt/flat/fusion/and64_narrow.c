/*
 *  TCC IR - 64-bit AND narrowing (mask clears the high word)
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

/* tcc_ir_opt_and64_narrow: rewrite a 64-bit AND whose immediate mask has a zero
 * high word, and whose result is only ever read as 32 bits, into a 32-bit AND.
 *
 *   T2(i64) = T1 AND #0x7FF     ; high word of the mask is 0 -> result hi == 0
 *   T3(i32) = T2                ; truncating ASSIGN reads only T2's low word
 *
 * The AND's high half is `hi & 0` -- a constant zero that nothing reads, so
 * emitting it (`mov dst_hi, #0`) is pure waste.  Narrowing the op also makes it
 * a low-only consumer of T1, which is what unlocks the real win: the SHR
 * feeding it can then be marked skip_hi by tcc_ir_opt_shift64_dead_half, whose
 * single-use/low-only test previously failed here because the intervening AND
 * was pair-typed and therefore read both words.
 *
 * This is the dominant idiom in lib/fp/soft -- every accessor in soft_common.h
 * is `(bits >> c) & mask` cast to int (double_sign, double_exp, is_nan_bits,
 * is_inf_bits, is_zero_bits) -- so the pair of passes turns each extract from a
 * shift-pair + OR + constant materialization + two dead `mov #0`s into a plain
 * 32-bit shift and mask.
 *
 * Narrowing only: the def keeps its vreg, so this never changes what any other
 * consumer sees -- the single-use gate guarantees there is no other consumer.
 * Operands are patched through the pool setters (a raw .btype overwrite on a
 * split-encoded operand corrupts the second word). */

/* A use of `vreg` at `u` reads only the LOW word when the operand naming it is
 * not itself pair-typed -- codegen derives is_64bit from the operand's own
 * btype, so a non-pair operand never references the high register.  The
 * truncating ASSIGN keeps its SOURCE pair-typed while narrowing, so the width
 * that decides is the DEST's; that is the shape this idiom ends in. */
static int a64_use_reads_low_only(TCCIRState *ir, IRQuadCompact *u, int32_t vreg)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, u);
  IROperand s2 = tcc_ir_op_get_src2(ir, u);
  int m1 = (irop_get_vreg(s1) == vreg);
  int m2 = (irop_get_vreg(s2) == vreg);
  if (!m1 && !m2)
    return 0;

  if (u->op == TCCIR_OP_ASSIGN && m1 && !m2 && !irop_needs_pair(tcc_ir_op_get_dest(ir, u)))
    return 1;

  if (m1 && irop_needs_pair(s1))
    return 0;
  if (m2 && irop_needs_pair(s2))
    return 0;
  return 1;
}

int tcc_ir_opt_and64_narrow(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_AND)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_needs_pair(dest))
      continue; /* already 32-bit */
    /* A memory destination must still have its full 8 bytes written; this
     * analysis only proves no VREG use reads the high half. */
    if (dest.is_lval)
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(src2))
      continue;
    /* The mask must zero the entire high word, so the narrowed op computes the
     * same value the 64-bit one did. */
    uint64_t imm = (uint64_t)irop_get_imm64_ex(ir, src2);
    if ((imm >> 32) != 0)
      continue;
    /* src1 must be a real pair we can read the low word of. */
    if (!irop_needs_pair(src1))
      continue;

    if (!tcc_ir_vreg_has_single_use(ir, dv, i))
      continue;

    int low_only = 0;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *u = &ir->compact_instructions[j];
      if (u->op == TCCIR_OP_NOP)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_src1(ir, u)) != dv && irop_get_vreg(tcc_ir_op_get_src2(ir, u)) != dv)
        continue;
      low_only = a64_use_reads_low_only(ir, u, dv);
      break;
    }
    if (!low_only)
      continue;

    LOG_IR_GEN("OPTIMIZE: and64_narrow at i=%d (mask=0x%llx)", i, (unsigned long long)imm);

    IROperand nd = dest;
    nd.btype = IROP_BTYPE_INT32;
    tcc_ir_op_set_dest(ir, q, nd);

    IROperand ns1 = src1;
    ns1.btype = IROP_BTYPE_INT32;
    tcc_ir_set_src1(ir, i, ns1);

    IROperand ns2 = irop_make_imm32(-1, (int32_t)(uint32_t)imm, IROP_BTYPE_INT32);
    ns2.is_unsigned = src2.is_unsigned;
    tcc_ir_set_src2(ir, i, ns2);

    changes++;
  }

  return changes;
}

int tcc_ir_opt_and64_narrow_ex(IROptCtx *ctx) { return tcc_ir_opt_and64_narrow(ctx->ir); }
