/*
 *  TCC IR - 64-bit high-word field extract to UBFX
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

/* tcc_ir_opt_shift64_extract_ubfx: `(v64 >> k) & mask` with k >= 32 is one
 * UBFX on v64's HIGH word.
 *
 * Every accessor in lib/fp/soft/soft_common.h is this shape --
 * `double_sign(bits)` is `(bits >> 63) & 1`, `double_exp(bits)` is
 * `(bits >> 52) & 0x7FF` -- and each is executed once per operand of every
 * soft-float entry point.  A dynamic profile of the double benchmarks counts
 * 138,746 of these pairs, all in dadd/dmul/ddiv/dcmp_core's unpacking.
 *
 * The 32-bit version of the rewrite already exists twice over
 * (source/opt/ssa/scalar/narrow.c's SHR-into-AND and SHR-into-UBFX rules,
 * source/opt/flat/fusion/shift_pair_ubfx.c's SHL/SHR pair), and both bail the
 * moment a 64-bit value is involved -- with reason: a 32-bit mask over a
 * 64-bit value clears its high half, so the narrow rewrite would change the
 * value.  What makes it sound HERE is the shift count: at k >= 32 the field
 * lies entirely inside the high word, so extracting it from that word alone
 * loses nothing.
 *
 * By the time this runs the shift already emits a single instruction --
 * shift64_dead_half marks its high word dead and and64_narrow (or narrow.c)
 * has made the mask 32-bit -- so the pair costs `lsr` + `ubfx`/`and` and the
 * rewrite makes it one.
 *
 * UBFX names the high word through bit 12 of its parameter word (UBFX_HI_HALF)
 * rather than an lsb above 31: the lsb field stays 0..31, so vrp.c and fold.c
 * keep reading the same width and the same value range out of it.
 */

/* Contiguous low-bit mask -> its width, else 0. */
static int u64x_mask_width(uint32_t m)
{
  if (m == 0 || (m & (m + 1)) != 0)
    return 0;
  return __builtin_popcount(m);
}

/* The (lsb, width) a UBFX parameter word encodes, or 0 when it is one this
 * pass has already rewritten. */
static int u64x_ubfx_params(int32_t param, int *lsb, int *width)
{
  if (param & UBFX_HI_HALF)
    return 0;
  *lsb = param & 0x1F;
  *width = (param >> 5) & 0x1F;
  return *width > 0;
}

int tcc_ir_opt_shift64_extract_ubfx(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int lsb_in, width;

    /* Consumer: a 32-bit AND against a contiguous low mask, or the UBFX the
     * mask passes have already made of one. */
    if (q->op == TCCIR_OP_AND)
    {
      IROperand m = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(m) || m.is_sym)
        continue;
      uint64_t mv = (uint64_t)irop_get_imm64_ex(ir, m);
      if ((mv >> 32) != 0)
        continue;
      width = u64x_mask_width((uint32_t)mv);
      if (width < 1 || width > 31)
        continue;
      lsb_in = 0;
    }
    else if (q->op == TCCIR_OP_UBFX)
    {
      IROperand p = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(p) || p.is_sym)
        continue;
      if (!u64x_ubfx_params((int32_t)irop_get_imm64_ex(ir, p), &lsb_in, &width))
        continue;
    }
    else
      continue;

    /* The result is the 32-bit field, so a pair destination would still owe a
     * high word this rewrite does not produce. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_needs_pair(dest) || dest.is_lval)
      continue;

    /* The shift's result reaches the mask as a 32-bit operand as often as a
     * 64-bit one -- and64_narrow and narrow.c both retype it in place once
     * they have proved only its low word is read -- so the width that matters
     * is the SHIFT's, checked below, not this operand's. */
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (s1.is_lval || !irop_has_vreg(s1))
      continue;
    int32_t t1 = irop_get_vreg(s1);
    if (TCCIR_DECODE_VREG_TYPE(t1) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Producer: a LOGICAL 64-bit right shift by at least 32.  SAR is excluded
     * -- above the sign bit it fills, and its lowering reads the high word
     * differently. */
    int shr_idx = tcc_ir_find_defining_instruction(ir, t1, i);
    if (shr_idx < 0)
      continue;
    IRQuadCompact *shr_q = &ir->compact_instructions[shr_idx];
    if (shr_q->op != TCCIR_OP_SHR)
      continue;
    /* It is the shift that has to be the 64-bit one: only then does a count of
     * 32 or more mean "the high word", which is the whole argument here. */
    if (!irop_needs_pair(tcc_ir_op_get_dest(ir, shr_q)))
      continue;
    IROperand cnt = tcc_ir_op_get_src2(ir, shr_q);
    if (!irop_is_immediate(cnt) || cnt.is_sym)
      continue;
    int64_t k = irop_get_imm64_ex(ir, cnt);
    if (k < 32 || k > 63)
      continue;

    IROperand v = tcc_ir_op_get_src1(ir, shr_q);
    if (v.is_lval || !irop_has_vreg(v) || !irop_needs_pair(v))
      continue;

    /* The whole field must sit inside the high word. */
    int lsb = (int)(k - 32) + lsb_in;
    if (lsb < 0 || lsb > 31 || lsb + width > 32)
      continue;

    /* NOPing the shift is only free if this is its one reader. */
    if (!tcc_ir_vreg_has_single_use(ir, t1, shr_idx))
      continue;

    /* The UBFX reads v at the consumer's position, so v must be unchanged in
     * between and no control-flow edge may separate the two. */
    int32_t v_vr = irop_get_vreg(v);
    int safe = 1;
    for (int j = shr_idx + 1; j < i && safe; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
          jq->op == TCCIR_OP_SWITCH_TABLE || jq->is_jump_target)
        safe = 0;
      else if (irop_config[jq->op].has_dest)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, jq);
        if (irop_has_vreg(jd) && irop_get_vreg(jd) == v_vr)
          safe = 0;
      }
    }
    if (!safe || q->is_jump_target)
      continue;

    LOG_IR_GEN("OPTIMIZE: shift64_extract_ubfx @%d: (v>>%d)&mask -> UBFX hi lsb=%d width=%d (SHR@%d NOP)", i,
               (int)k, lsb, width, shr_idx);
    q->op = TCCIR_OP_UBFX;
    tcc_ir_set_src1(ir, i, v);
    tcc_ir_set_src2(ir, i,
                    irop_make_imm32(-1, lsb | (width << 5) | UBFX_HI_HALF, IROP_BTYPE_INT32));
    shr_q->op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_shift64_extract_ubfx_ex(IROptCtx *ctx) { return tcc_ir_opt_shift64_extract_ubfx(ctx->ir); }
