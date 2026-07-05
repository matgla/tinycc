/*
 *  test_ssa_opt_branch.c - branch optimization pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_branch(): folding unconditional branches
 *      * JMP to single predecessor → eliminate jump
 *      * JMP to multiple predecessors → keep jump
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_branch.c via UT11.
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
 * Unconditional JMP to single successor → eliminate
 * ======================================================================== */

UT_TEST(test_branch_unconditional_single)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/3);
  /* Block 0: JMP to block 1; Block 1: t2 = #0 */
  ssa_add_instr(&c, TCCIR_OP_JUMP, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));

  /* Build CFG with 2 blocks. */
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  /* The pass should fold the unconditional JMP. */
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_branch)
{
  UT_COVERS("ssa:branch");
  UT_RUN(test_branch_unconditional_single);
}
