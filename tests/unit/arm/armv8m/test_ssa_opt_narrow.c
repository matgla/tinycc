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
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_narrow.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - SHL/SHR/SAR require a shift-amount src2; use ssa_add_instr4().
 *    - AND with immediate src2 also needs ssa_add_instr4().
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

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
  /* t0 = #0xFF; t1 = t0 >> 4; t2 = t1 & 0x3 → no fold */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  int and_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0x3, I32), UTB_NONE);

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
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_narrow)
{
  UT_COVERS("ssa:narrow");
  UT_RUN(test_narrow_shl_shr_basic);
  UT_RUN(test_narrow_shl_shr_char_trunc);
  UT_RUN(test_narrow_shl_shr_short_trunc);
  UT_RUN(test_narrow_shl_shr_non_matching);
  UT_RUN(test_narrow_shl_sar_no_fold);
  UT_RUN(test_narrow_shl_shr_int64_no_fold);
  UT_RUN(test_narrow_shl_shr_multi_def_no_fold);
  UT_RUN(test_narrow_shl_shr_zero_shift_no_fold);
  UT_RUN(test_narrow_and_fold_subset_mask);
  UT_RUN(test_narrow_and_fold_idempotent);
  UT_RUN(test_narrow_and_fold_shr_mask_covers);
  UT_RUN(test_narrow_and_fold_shr_mask_not_covering);
  UT_RUN(test_narrow_and_fold_non_imm_src2);
  UT_RUN(test_narrow_and_fold_inner_not_and);
  UT_RUN(test_narrow_and_fold_shr_non_imm_outer);
  UT_RUN(test_narrow_and_fold_multi_def_inner);
}
