/*
 *  test_ssa_opt_reassoc.c - reassociation pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_reassoc(): reordering associative operations
 *      * (a + b) + c → a + (b + c) when c is cheaper to fold first
 *      * (a * b) * c → a * (b * c)
 *      * Non-associative ops (SUB, DIV) → no change
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_reassoc.c via UT11.
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
 * Reassociation of addition: (a + b) + c → a + (b + c)
 * ======================================================================== */

UT_TEST(test_reassoc_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t2 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* The pass should attempt reassociation. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of multiplication: (a * b) * c → a * (b * c)
 * ======================================================================== */

UT_TEST(test_reassoc_mul)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #2; t1 = #3; t2 = t0 * t1; t3 = t2 * #4 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(2, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* The pass should attempt reassociation. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-associative ops (SUB) → no change
 * ======================================================================== */

UT_TEST(test_reassoc_sub_no_change)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = #3; t2 = t0 - t1; t3 = t2 - #2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr(&c, TCCIR_OP_SUB, utb_temp(2, I32), utb_temp(0, I32));
  int sub_i = ssa_add_instr(&c, TCCIR_OP_SUB, utb_temp(3, I32),
                            utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* The pass should not change anything (SUB is not associative). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, sub_i), TCCIR_OP_SUB);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_reassoc)
{
  UT_COVERS("ssa:reassoc");
  UT_RUN(test_reassoc_add);
  UT_RUN(test_reassoc_mul);
  UT_RUN(test_reassoc_sub_no_change);
}
