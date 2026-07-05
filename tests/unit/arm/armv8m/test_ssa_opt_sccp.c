/*
 *  test_ssa_opt_sccp.c - sparse conditional constant propagation
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_sccp(): propagating constants through branches
 *      * t0 = #1; if (t0) → always-taken branch
 *      * t0 = #0; if (t0) → never-taken branch
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_sccp.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Always-taken branch: t0 = #1; if (t0) → fold to unconditional JMP
 * ======================================================================== */

UT_TEST(test_sccp_always_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #1; JUMPIF t0 → always taken */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  /* The pass should fold the conditional branch. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Never-taken branch: t0 = #0; if (t0) → fold to NOP
 * ======================================================================== */

UT_TEST(test_sccp_never_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #0; JUMPIF t0 → never taken */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  /* The pass should fold the conditional branch. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unknown branch: t0 = t1 (unknown); if (t0) → keep conditional
 * ======================================================================== */

UT_TEST(test_sccp_unknown)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = t1 (unknown); JUMPIF t0 → keep conditional */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  /* The pass should not fold (t0 is unknown). */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_sccp)
{
  UT_COVERS("ssa:sccp");
  UT_RUN(test_sccp_always_taken);
  UT_RUN(test_sccp_never_taken);
  UT_RUN(test_sccp_unknown);
}
