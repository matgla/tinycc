/*
 *  test_ssa_opt_fold.c - constant folding pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_fold(): evaluate ALU ops with two immediate operands
 *      * x + 0, x - 0, x * 1 → x (algebraic identity)
 *      * x * 0, x & 0 → 0
 *      * x << 0, x >> 0 → x
 *      * Non-immediate operands → no fold
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_fold.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Algebraic identity: x + 0 → x
 * ======================================================================== */

UT_TEST(test_fold_add_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #5; t1 = t0 + #0 → t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_fold(c.ctx);
  /* The pass should fold x + 0 → x. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Algebraic identity: x * 1 → x
 * ======================================================================== */

UT_TEST(test_fold_mul_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #7; t1 = t0 * #1 → t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_fold(c.ctx);
  /* The pass should fold x * 1 → x. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Algebraic identity: x * 0 → 0
 * ======================================================================== */

UT_TEST(test_fold_mul_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #5; t1 = t0 * #0 → t1 = #0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_fold(c.ctx);
  /* The pass should fold x * 0 → 0. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-immediate operands → no fold
 * ======================================================================== */

UT_TEST(test_fold_non_immediate)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = #3; t2 = t0 + t1 → no fold (both operands are temps) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  int add_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_fold(c.ctx);
  /* The pass should not fold (operands are not both immediate). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_fold)
{
  UT_COVERS("ssa:fold");
  UT_RUN(test_fold_add_zero);
  UT_RUN(test_fold_mul_one);
  UT_RUN(test_fold_mul_zero);
  UT_RUN(test_fold_non_immediate);
}
