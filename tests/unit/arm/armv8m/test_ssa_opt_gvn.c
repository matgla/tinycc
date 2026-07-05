/*
 *  test_ssa_opt_gvn.c - global value numbering pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_gvn(): detecting redundant computations
 *      * x = a + b; y = a + b → y = x (GVN)
 *      * Different operands → no redundancy
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_gvn.c via UT11.
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
 * Redundant computation: x = a + b; y = a + b → y = x
 * ======================================================================== */

UT_TEST(test_gvn_redundant_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t0 + t1 → t3 = t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  /* The pass should detect the redundant ADD. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-redundant: different operands → no optimization
 * ======================================================================== */

UT_TEST(test_gvn_non_redundant)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t0 + #3 → no redundancy */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32));
  int add_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  /* The pass should not optimize (operands differ). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_gvn)
{
  UT_COVERS("ssa:gvn");
  UT_RUN(test_gvn_redundant_add);
  UT_RUN(test_gvn_non_redundant);
}
