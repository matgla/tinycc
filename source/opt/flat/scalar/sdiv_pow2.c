/*
 *  TCC IR - Signed division by a power of two, without SDIV
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* `x / 2` and friends compiled to `movs rN,#2` + `sdiv` -- an instruction to
 * park a constant the divide cannot encode, then a divide that costs 2-12
 * cycles on a Cortex-M33 depending on the operand magnitudes.  The unsigned
 * form has been a plain shift for a long time (ssa:strength sr_udiv); the
 * signed form could not follow, because C truncates toward zero while an
 * arithmetic right shift floors: -7 / 2 is -3, but -7 >> 1 is -4.
 *
 * Biasing the dividend by 2^n - 1 when, and only when, it is negative fixes
 * exactly that off-by-one:
 *
 *     x / 2^n  ==  (x + ((x >> 31) >>u (32 - n))) >> n
 *
 * with an arithmetic outer shift.  `x >> 31` is all-ones for a negative x and
 * zero otherwise, so the logical shift yields 2^n - 1 or 0.  It is exact at
 * INT_MIN too, where nothing overflows: INT_MIN + (2^n - 1) stays negative.
 *
 * n == 1 needs one instruction fewer, since `(x >> 31) >>u 31` is just
 * `x >>u 31`.  The remaining `>>u` folds into the ADD as a barrel shift
 * post-RA, so the emitted code is
 *
 *     add.w t, x, x, lsr #31        asr  t, x, #31
 *     asr   d, t, #1          and   add.w t, x, t, lsr #(32-n)
 *                                   asr  d, t, #n
 *
 * -- two or three single-cycle ALU ops in place of `movs` + `sdiv`.  This is
 * the sequence gcc emits for the same source.
 *
 * Running here, in the flat pipeline, rather than in codegen is what buys the
 * barrel-shift fold and lets the later value passes const-fold or CSE the
 * pieces; a constant dividend is left alone for fold_const_eval, which turns
 * the whole divide into one immediate.
 */

/* insert_instr_at, not a hand-rolled splice: it is what stamps a FRESH
 * orig_index on the new instruction.  orig_index keys the side tables
 * (barrel_shifts, shift64_dead_half, zero_half64), all sized max_orig_index+1,
 * so a synthesized instruction left at index 0 silently inherits instruction
 * 0's annotations -- here that fused a neighbour's shift into the bias ADD and
 * dropped both `asr`s, turning x/4 into a mov. */

int tcc_ir_opt_sdiv_pow2(TCCIRState *ir)
{
  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_DIV)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* 32-bit only: the 64-bit divide is a runtime call, not an SDIV. */
    if (irop_is_64bit(dest) || irop_is_64bit(src1) || irop_is_64bit(src2))
      continue;
    /* An unsigned dividend reaching a signed DIV would make `x >> 31` a
     * sign test on a value that has no sign; leave it to the divide. */
    if (src1.is_unsigned || dest.is_unsigned)
      continue;
    /* The dividend is read three times, so it must be a plain register value:
     * a deref would turn one load into three, and a stack lvalue names memory
     * an intervening pass may still rewrite. */
    if (!irop_has_vreg(src1) || src1.is_lval || src1.is_local || src1.is_llocal)
      continue;
    if (tcc_ir_barrel_shift_at(ir, q))
      continue;
    if (!irop_is_immediate(src2) || src2.is_lval)
      continue;

    /* Positive powers of two only.  The immediate is signed, so 0x80000000 is
     * INT_MIN -- a divisor of -2^31, not +2^31 -- and must not be read as a
     * shift of 31. */
    int32_t divisor = (int32_t)irop_get_imm64_ex(ir, src2);
    if (divisor < 2)
      continue;
    int n = is_power_of_2((int64_t)divisor);
    if (n < 1 || n > 30)
      continue;

    int btype = irop_get_btype(dest);
    IROperand x = src1;
    x.is_lval = 0;

    int32_t t_bias = tcc_ir_vreg_alloc_temp(ir);
    int32_t t_sum = tcc_ir_vreg_alloc_temp(ir);
    int32_t t_sign = (n == 1) ? 0 : tcc_ir_vreg_alloc_temp(ir);
    if (t_bias < 0 || t_sum < 0 || t_sign < 0)
      continue;
    IROperand bias = irop_make_vreg(t_bias, btype);
    IROperand sum = irop_make_vreg(t_sum, btype);

    /* The DIVIDE's own slot becomes the FIRST instruction of the sequence and
     * the rest are appended after it -- never inserted before it.  Inserting
     * ahead of the divide would keep every branch that targeted it pointing at
     * the same instruction, which is now the LAST of the sequence: control
     * would jump straight past the bias computation and read an undefined
     * temporary.  981001-1's `else` arm starts with `n / 2 + 1` and is exactly
     * that shape.  Rewriting in place keeps the branch landing on the block's
     * real first instruction, and the appended ops shift only what follows. */
    int at = i;
    if (n == 1)
    {
      /* bias = x >>u 31 */
      tcc_ir_set_src2(ir, at, irop_make_imm32(0, 31, btype));
      tcc_ir_set_src1(ir, at, x);
      tcc_ir_set_dest(ir, at, bias);
      ir->compact_instructions[at].op = TCCIR_OP_SHR;
    }
    else
    {
      /* sign = x >> 31 (arithmetic), then bias = sign >>u (32 - n) */
      IROperand sign = irop_make_vreg(t_sign, btype);
      tcc_ir_set_src2(ir, at, irop_make_imm32(0, 31, btype));
      tcc_ir_set_src1(ir, at, x);
      tcc_ir_set_dest(ir, at, sign);
      ir->compact_instructions[at].op = TCCIR_OP_SAR;
      if (insert_instr_at(ir, ++at, TCCIR_OP_SHR, bias, sign,
                          irop_make_imm32(0, 32 - n, btype)) < 0)
        continue;
    }

    if (insert_instr_at(ir, ++at, TCCIR_OP_ADD, sum, x, bias) < 0)
      continue;
    if (insert_instr_at(ir, ++at, TCCIR_OP_SAR, dest, sum,
                        irop_make_imm32(0, n, btype)) < 0)
      continue;

    i = at;
    changes++;
  }

  LOG_IR_GEN("=== SDIV POW2: %d divides lowered ===", changes);
  return changes;
}
