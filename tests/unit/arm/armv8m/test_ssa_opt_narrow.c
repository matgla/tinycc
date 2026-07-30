/*
 *  test_ssa_opt_narrow.c - narrowing (shift-pair folding) pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_narrow(): gen_shr_fold + gen_and_fold
 *      * (x SHL #n) SHR #n → x AND #mask          [zero-extend idiom]
 *      * (x AND #m1) AND #m2 → simplify            [redundant-mask]
 *      * (x SHR #n) AND #mask → x SHR #n           [mask-covers-bits]
 *      * Non-matching shifts / 64-bit / multi-def → no fold
 *    - soft-FP double→float demotion fold:
 *      * tcc_ir_ssa_narrow_f64_imm_exact_f32() exact/inexact/NaN/IMM32 forms
 *      * full f2d→floor→d2f shape declines without mutation when the callee
 *        rename cannot complete (harness external_global_sym stub → NULL);
 *        the positive fold is covered by test_codegen_asm.py's
 *        test_float_narrow_fires
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/scalar/narrow.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - SHL/SHR/SAR require a shift-amount src2; use ssa_add_instr4().
 *    - AND with immediate src2 also needs ssa_add_instr4().
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/narrow.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include <string.h>

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * gen_shr_fold: (x SHL #28) SHR #28 → x AND #0xF
 *
 * Shift-left by 28 pushes 4 bits to the top; shift-right by 28 zero-extends.
 * Mask = (1 << 4) - 1 = 0xF.
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(28, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The SHR should have been rewritten to AND with mask 0xF. */
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_AND);
  IROperand src2 = utb_src2(c.ir, shr_i);
  UT_ASSERT_EQ(src2.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(src2.u.imm32, 0xF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: SHL #24, SHR #24 → AND #0xFF (unsigned char truncation)
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_char_trunc)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                utb_imm(0x12345678, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(24, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(24, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_AND);
  IROperand src2 = utb_src2(c.ir, shr_i);
  UT_ASSERT_EQ(src2.u.imm32, 0xFF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: SHL #16, SHR #16 → AND #0xFFFF (unsigned short truncation)
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_short_trunc)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                utb_imm(0xDEADBEEF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(16, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(16, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_AND);
  IROperand src2 = utb_src2(c.ir, shr_i);
  UT_ASSERT_EQ(src2.u.imm32, 0xFFFF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: Non-matching shifts → no fold
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_non_matching)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_SHR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: SAR (arithmetic shift right) → no fold
 *
 * The pass only folds (x SHL #n) SHR #n; SAR would sign-extend and the
 * AND mask is wrong for negative values.
 * ======================================================================== */

UT_TEST(test_narrow_shl_sar_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  int sar_i = ssa_add_instr4(&c, TCCIR_OP_SAR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(28, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, sar_i), TCCIR_OP_SAR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: 64-bit types → no fold
 *
 * The mask formula (1 << (32-n))-1 assumes 32-bit width. 64-bit shifts
 * require a 64-bit mask, which this pass does not handle.
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_int64_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I64), utb_imm(1, I64));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64),
                 utb_imm(28, I64), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I64), utb_temp(1, I64),
                             utb_imm(28, I64), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_SHR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: inner has multiple defs → no fold
 *
 * If the SHL result has multiple defs (non-SSA), we cannot safely redirect.
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_multi_def_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* Define t1 twice to force def_count > 1. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(28, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_SHR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_shr_fold: shift amount 0 → no fold
 * ======================================================================== */

UT_TEST(test_narrow_shl_shr_zero_shift_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, shr_i), TCCIR_OP_SHR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: (x AND #0xF) AND #0xFF → simplify to x AND #0xF
 *
 * Inner mask is tighter than outer → use inner result directly.
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_subset_mask)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 & #0xF; t2 = t1 & #0xFF → t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0xF, I32), UTB_NONE);
  int and2_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                              utb_imm(0xFF, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The outer AND should have been simplified: dest assigned from inner result. */
  UT_ASSERT_EQ(utb_op(c.ir, and2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, and2_i)),
               IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: (x AND #0xFF) AND #0xFF → simplify (idempotent)
 *
 * Outer mask equals inner mask → just use inner result.
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_idempotent)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 & #0xFF; t2 = t1 & #0xFF → t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0xFF, I32), UTB_NONE);
  int and2_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                              utb_imm(0xFF, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The outer AND should have been simplified: dest assigned from inner result. */
  UT_ASSERT_EQ(utb_op(c.ir, and2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, and2_i)),
               IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: (x SHR #28) AND #0xF → simplify (mask covers all bits)
 *
 * x SHR #28 produces values in [0, 0xF]; AND #0xF is redundant.
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_shr_mask_covers)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 >> 28; t2 = t1 & 0xF → t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(28, I32), UTB_NONE);
  int and_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0xF, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The AND should have been simplified to ASSIGN of the SHR result. */
  UT_ASSERT_EQ(utb_op(c.ir, and_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, and_i)), IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: (x SHR #4) AND #0x3 → no fold (mask does not cover all bits)
 *
 * x SHR #4 produces [0, 0xF]; AND #0x3 only keeps 2 bits → not redundant.
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_shr_mask_not_covering)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 >> 4; t2 = t1 & 0x3 → UBFX(t0, 4, 2) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  int and_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0x3, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, and_i), TCCIR_OP_UBFX);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: non-immediate src2 → no fold
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_non_imm_src2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 & #0xF; t2 = t1 & t3 (non-immediate) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0xF, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0xFF, I32));
  int and2_i = ssa_add_instr(&c, TCCIR_OP_AND, utb_temp(2, I32),
                             utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, and2_i), TCCIR_OP_AND);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: inner is not AND → no fold
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_inner_not_and)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 + #1; t2 = t1 & #0xFF → no fold (inner is ADD) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));
  int and_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0xFF, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, and_i), TCCIR_OP_AND);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: inner is SHR but outer src2 is not immediate → no fold
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_shr_non_imm_outer)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 >> 4; t2 = t1 & t3 (non-immediate outer) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0xF, I32));
  int and_i = ssa_add_instr(&c, TCCIR_OP_AND, utb_temp(2, I32),
                            utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, and_i), TCCIR_OP_AND);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * gen_and_fold: multi-def inner → no fold
 * ======================================================================== */

UT_TEST(test_narrow_and_fold_multi_def_inner)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #0xFF; t1 = t0 & 0xF (def'd twice); t2 = t1 & 0xFF */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0xF, I32), UTB_NONE);
  ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(0xF, I32), UTB_NONE);
  int and2_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                              utb_imm(0xFF, I32), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, and2_i), TCCIR_OP_AND);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * float demotion fold: tcc_ir_ssa_narrow_f64_imm_exact_f32 helper
 *
 * Double-immediate -> F32 narrowing used by the soft-FP demotion fold.
 * IMM32+FLOAT64 (integer-valued shorthand) always narrows; F64 pool
 * constants must be exactly float-representable.
 * ======================================================================== */

static double ut_bits_to_d(uint64_t b)
{
  double d;
  memcpy(&d, &b, sizeof(d));
  return d;
}

static uint64_t ut_d_to_bits(double d)
{
  uint64_t b;
  memcpy(&b, &d, sizeof(b));
  return b;
}

static uint32_t ut_f_to_bits(float f)
{
  uint32_t b;
  memcpy(&b, &f, sizeof(b));
  return b;
}

UT_TEST(test_narrow_f64_imm_exact_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);

  /* 0.0, 1.5, -2.0, 2^24 and +Inf are all exact float values. */
  const double vals[] = { 0.0, 1.5, -2.0, 16777216.0, -0.0,
                          3.4028234663852886e38 };
  for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++)
  {
    uint32_t idx = tcc_ir_pool_add_f64(c.ir, ut_d_to_bits(vals[k]));
    IROperand out;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_f64(0, idx), &out), 1);
    UT_ASSERT_EQ(out.tag, IROP_TAG_F32);
    UT_ASSERT_EQ(out.u.f32_bits, ut_f_to_bits((float)vals[k]));
  }

  /* +Inf is an exact float value too. */
  {
    uint32_t idx = tcc_ir_pool_add_f64(c.ir, ut_d_to_bits(1.0 / 0.0));
    IROperand out;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_f64(0, idx), &out), 1);
    UT_ASSERT_EQ(out.u.f32_bits, ut_f_to_bits((float)(1.0 / 0.0)));
  }

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_narrow_f64_imm_inexact_rejects)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);

  /* None of these is exactly representable as a float; narrowing ahead of
   * floorf would change rounding, so the helper must refuse. */
  const double vals[] = { 0.1, 0.9999999999999999, 1.0 / 3.0, 1e-50,
                          16777217.0 /* 2^24+1 */, -1.0000000000000002 };
  for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++)
  {
    uint32_t idx = tcc_ir_pool_add_f64(c.ir, ut_d_to_bits(vals[k]));
    IROperand out;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_f64(0, idx), &out), 0);
  }

  /* The canonical quiet NaN round-trips bit-exactly through the cast and is
   * accepted; that is sound here because every table entry (floor/ceil/...)
   * propagates NaN unchanged on both the double and float paths. */
  {
    uint32_t idx = tcc_ir_pool_add_f64(c.ir, 0x7FF8000000000000ULL);
    IROperand out;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_f64(0, idx), &out), 1);
  }

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_narrow_f64_imm_imm32_shorthand)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);

  /* IMM32 + FLOAT64: integer-valued double shorthand; always sound. */
  const int32_t vals[] = { 0, 5, -3, 1000000, -2147483647 };
  for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++)
  {
    IROperand op = irop_make_imm32(0, vals[k], IROP_BTYPE_FLOAT64);
    IROperand out;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, op, &out), 1);
    UT_ASSERT_EQ(out.tag, IROP_TAG_F32);
    UT_ASSERT_EQ(out.u.f32_bits, ut_f_to_bits((float)(double)(int64_t)vals[k]));
  }

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_narrow_f64_imm_wrong_form_rejects)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand out;

  /* F32 input is not a double immediate. */
  UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_f32(0, 0), &out), 0);
  /* Plain integer immediate. */
  UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_imm32(0, 5, I32), &out), 0);
  /* I64 pool entry without FLOAT64 btype. */
  {
    uint32_t idx = tcc_ir_pool_add_i64(c.ir, 0);
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, irop_make_i64(0, idx, I64), &out), 0);
  }
  /* VREG operand. */
  UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, utb_temp(0, I32), &out), 0);
  /* lval F64. */
  {
    uint32_t idx = tcc_ir_pool_add_f64(c.ir, ut_d_to_bits(0.0));
    IROperand op = irop_make_f64(0, idx);
    op.is_lval = 1;
    UT_ASSERT_EQ(tcc_ir_ssa_narrow_f64_imm_exact_f32(c.ir, op, &out), 0);
  }

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * float demotion fold: full f2d -> floor -> d2f shape, unresolved rename
 *
 * The harness's external_global_sym() stub returns NULL, so
 * change_callee_sym() fails and the fold must decline WITHOUT having
 * mutated anything (no param redirection, no NOPs).  The positive path is
 * covered end-to-end by tests/ir_tests/test_codegen_asm.py
 * (test_float_narrow_fires).
 * ======================================================================== */

UT_TEST(test_narrow_float_demote_rename_fails_no_mutation)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);

  static Sym f2d_sym, floor_sym, d2f_sym;
  utb_set_tok_str(600, "__aeabi_f2d");
  utb_set_tok_str(601, "floor");
  utb_set_tok_str(602, "__aeabi_d2f");
  f2d_sym.v = 600;
  floor_sym.v = 601;
  d2f_sym.v = 602;

  const int id_f2d = 1, id_floor = 2, id_d2f = 3;
  /* PARAM T0 ; T1 = f2d ; PARAM T1 ; T2 = floor ; PARAM T2 ; T3 = d2f ; RET T3 */
  ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                 utb_imm((int32_t)TCCIR_ENCODE_PARAM(id_f2d, 0), I32));
  int i_f2d = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I64),
                             irop_make_symref(0, tcc_ir_pool_add_symref(c.ir, &f2d_sym, 0, 0), 0, 0, 0, I32),
                             utb_imm((int32_t)TCCIR_ENCODE_CALL(id_f2d, 1), I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I64),
                 utb_imm((int32_t)TCCIR_ENCODE_PARAM(id_floor, 0), I32));
  int i_floor = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I64),
                               irop_make_symref(0, tcc_ir_pool_add_symref(c.ir, &floor_sym, 0, 0), 0, 0, 0, I32),
                               utb_imm((int32_t)TCCIR_ENCODE_CALL(id_floor, 1), I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(2, I64),
                 utb_imm((int32_t)TCCIR_ENCODE_PARAM(id_d2f, 0), I32));
  int i_d2f = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVAL, utb_temp(3, I32),
                             irop_make_symref(0, tcc_ir_pool_add_symref(c.ir, &d2f_sym, 0, 0), 0, 0, 0, I32),
                             utb_imm((int32_t)TCCIR_ENCODE_CALL(id_d2f, 1), I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_narrow(c.ctx);

  /* Rename cannot complete in the harness: zero changes, and the whole call
   * sequence (calls + params) must be byte-identical to the input. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, i_f2d), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(c.ir, i_floor), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(c.ir, i_d2f), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(IROP_VR(utb_dest(c.ir, i_floor)), IROP_VR(utb_temp(2, I64)));

  utb_set_tok_str(600, NULL);
  utb_set_tok_str(601, NULL);
  utb_set_tok_str(602, NULL);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:narrow");
