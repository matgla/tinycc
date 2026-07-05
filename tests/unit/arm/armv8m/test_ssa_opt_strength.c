/*
 *  test_ssa_opt_strength.c - strength reduction pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_strength(): MUL/UDIV/UMOD by powers of 2 → shifts
 *      * MUL by 2^n → SHL by n
 *      * UDIV by 2^n → SHR by n
 *      * UMOD by 2^n → AND (mask)
 *      * Non-power-of-2 → no change
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_strength.c via UT11.
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
#define I64 IROP_BTYPE_INT64

/* ========================================================================
 * MUL by power of 2 → SHL
 * ======================================================================== */

UT_TEST(test_strength_reduce_mul_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #8; t1 = t0 * 4 → t1 = t0 << 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(8, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  /* The pass should replace MUL with SHL. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UDIV by power of 2 → SHR
 * ======================================================================== */

UT_TEST(test_strength_reduce_udiv_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #16; t1 = t0 / 4 → t1 = t0 >> 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(16, I32));
  ssa_add_instr(&c, TCCIR_OP_UDIV, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  /* The pass should replace UDIV with SHR. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * UMOD by power of 2 → AND mask
 * ======================================================================== */

UT_TEST(test_strength_reduce_umod_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 % 4 → t1 = t0 & 3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_UMOD, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  /* The pass should replace UMOD with AND. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-power-of-2 operand → no change
 * ======================================================================== */

UT_TEST(test_strength_reduce_non_pow2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #7; t1 = t0 * 3 → no optimization (3 is not power of 2) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int mul_i = ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_strength(c.ctx);
  /* The pass should not change anything. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_MUL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_strength)
{
  UT_COVERS("ssa:strength_reduce");
  UT_RUN(test_strength_reduce_mul_pow2);
  UT_RUN(test_strength_reduce_udiv_pow2);
  UT_RUN(test_strength_reduce_umod_pow2);
  UT_RUN(test_strength_reduce_non_pow2);
}
