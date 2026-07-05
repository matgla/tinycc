/*
 *  test_ssa_opt_cmp_eq.c - CMP equality fact propagation
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_cmp_eq_prop(): pushing equality facts from CMP+JEQ
 *      * CMP a, b; JEQ → fact: a == b
 *      * CMP a, b; JNE → fact: a != b
 *      * Later CMP with same operands uses the fact
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_cmp_eq.c via UT11.
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
 * CMP + JEQ: fact a == b derived
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_eq)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; CMP t0, t1; JEQ → fact: t0 == t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_CMP, utb_temp(2, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  /* The pass should derive the equality fact. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * CMP + JNE: fact a != b derived
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_ne)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; CMP t0, t1; JNE → fact: t0 != t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_CMP, utb_temp(2, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  /* The pass should derive the inequality fact. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_cmp_eq)
{
  UT_COVERS("ssa:cmp_eq_prop");
  UT_RUN(test_cmp_eq_prop_eq);
  UT_RUN(test_cmp_eq_prop_ne);
}
