/*
 *  test_ssa_opt_strength.c - strength reduction pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_strength(): MUL/UDIV/UMOD by powers of 2 -> shifts/masks
 *      * MUL x, 2^n  -> SHL x, n        (src2 immediate)
 *      * MUL 2^n, x  -> SHL x, n        (src1 immediate, swapped)
 *      * UDIV x, 2^n -> SHR x, n
 *      * UMOD x, 2^n -> AND x, (2^n-1)
 *      * Non-power-of-2 / zero immediate -> no change
 *      * No immediate operand -> no change
 *      * Lval immediate operand -> no change
 *      * NOP instructions are skipped
 *      * Multiple rewrites in one pass
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/strength.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - MUL/UDIV/UMOD need a src2 operand; use ssa_add_instr3().
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/strength.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * MUL by power of 2 -> SHL (src2 is the immediate)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_pow2_src2_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 * #4 -> t1 = t0 << 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_SHL);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mul_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, mul_i).u.imm32, 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL by power of 2 -> SHL (src1 is the immediate, operands swapped)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_pow2_src1_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = #4 * t0 -> t1 = t0 << 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_imm(4, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_SHL);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mul_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, mul_i).u.imm32, 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL by 1 -> SHL by 0 (edge of power-of-2 detection)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_by_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 * #1 -> t1 = t0 << 0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_SHL);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mul_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, mul_i).u.imm32, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL with no immediate operand -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_no_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #7; t2 = #3; t1 = t0 * t2 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(3, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL by non-power-of-2 immediate -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_non_pow2_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #7; t1 = t0 * #3 -> no optimization (3 is not power of 2) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL by immediate 0 -> no change (0 is not a power of 2)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_zero_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #7; t1 = t0 * #0 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL by lval immediate -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_lval_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #7; t1 = t0 * &4 (lval) -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_lval(utb_imm(4, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV by power of 2 -> SHR
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / #4 -> t1 = t0 >> 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_SHR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, div_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, div_i).u.imm32, 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV by 1 -> SHR by 0
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_by_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / #1 -> t1 = t0 >> 0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_SHR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, div_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, div_i).u.imm32, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV with non-immediate divisor -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_non_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t2 = #4; t1 = t0 / t2 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(4, I32));
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_UDIV);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV by non-power-of-2 -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_non_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / #3 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_UDIV);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV by lval immediate -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_lval_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / &4 (lval) -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), utb_lval(utb_imm(4, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_UDIV);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD by power of 2 -> AND mask
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % #4 -> t1 = t0 & 3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_AND);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mod_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, mod_i).u.imm32, 3);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD by 1 -> AND 0
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_by_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % #1 -> t1 = t0 & 0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_AND);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mod_i)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, mod_i).u.imm32, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD with non-immediate divisor -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_non_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t2 = #4; t1 = t0 % t2 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(4, I32));
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_UMOD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD by non-power-of-2 -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_non_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % #3 -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_UMOD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD by lval immediate -> no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_lval_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % &4 (lval) -> no optimization */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_lval(utb_imm(4, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_UMOD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Multiple rewrites in a single pass
 * ======================================================================== */

UT_TEST(test_strength_reduce_multiple)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  /* t0 = #256;
   * t1 = t0 * #2  -> SHL
   * t2 = t0 / #8  -> SHR
   * t3 = t0 % #16 -> AND
   */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(256, I32));
  ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                 utb_temp(0, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(2, I32),
                 utb_temp(0, I32), utb_imm(8, I32));
  ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(3, I32),
                 utb_temp(0, I32), utb_imm(16, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 3);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * NOP instructions are skipped by the generator driver
 * ======================================================================== */

UT_TEST(test_strength_reduce_skips_nop)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_NOP, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_SHL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Other operations are ignored by the strength table
 * ======================================================================== */

UT_TEST(test_strength_reduce_ignores_other_ops)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #8; t1 = t0 + #4 -> ADD is not in the strength table */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL with F32 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_f32_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #8; t1 = t0 * #4.0f -> no optimization (F32 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  IROperand f32_imm = irop_make_f32(0, 0); /* F32 with bits=0 */
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), f32_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL with I64 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_i64_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #8; t1 = t0 * #4 (I64) -> no optimization (I64 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  int idx = tcc_ir_pool_add_i64(c.ir, 4);
  IROperand i64_imm = irop_make_i64(0, idx, IROP_BTYPE_INT64);
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             utb_temp(0, I32), i64_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV with F32 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_f32_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / #4.0f -> no optimization (F32 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  IROperand f32_imm = irop_make_f32(0, 0);
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), f32_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_UDIV);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV with I64 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_i64_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / #4 (I64) -> no optimization (I64 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  int idx = tcc_ir_pool_add_i64(c.ir, 4);
  IROperand i64_imm = irop_make_i64(0, idx, IROP_BTYPE_INT64);
  int div_i = ssa_add_instr3(&c, TCCIR_OP_UDIV, utb_temp(1, I32),
                             utb_temp(0, I32), i64_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, div_i), TCCIR_OP_UDIV);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD with F32 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_f32_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % #4.0f -> no optimization (F32 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  IROperand f32_imm = irop_make_f32(0, 0);
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), f32_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_UMOD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD with I64 immediate (PATTERN constraint matches, but is_imm32 rejects)
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_i64_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % #4 (I64) -> no optimization (I64 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  int idx = tcc_ir_pool_add_i64(c.ir, 4);
  IROperand i64_imm = irop_make_i64(0, idx, IROP_BTYPE_INT64);
  int mod_i = ssa_add_instr3(&c, TCCIR_OP_UMOD, utb_temp(1, I32),
                             utb_temp(0, I32), i64_imm);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mod_i), TCCIR_OP_UMOD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MUL with src1 F32 imm, src2 TEMP (swapped operands, non-IMM32)
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_f32_imm_src1)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #8; t1 = #4.0f * t0 -> no optimization (F32 is not IMM32) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  IROperand f32_imm = irop_make_f32(0, 0);
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                             f32_imm, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:strength_reduce");
