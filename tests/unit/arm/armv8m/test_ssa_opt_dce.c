/*
 *  test_ssa_opt_dce.c - dead code elimination pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_dce(): removing instructions with no live uses
 *      * t0 = #1; t1 = t0; (t1 unused) → NOP t1
 *      * t0 = #1; t1 = t0; use(t1) → keep t1
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_dce.c via UT11.
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

static int run_dce_with_opt(IRSSAOptCtx *ctx)
{
  static TCCState tcc_state_storage;

  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
  int changed = ssa_opt_dce(ctx);
  tcc_exit_state(&tcc_state_storage);
  return changed;
}

/* ========================================================================
 * Dead assignment: t1 = t0; (t1 unused) → NOP t1
 * ======================================================================== */

UT_TEST(test_dce_dead_assign)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  /* t0 = #1; t1 = t0; (t1 has no uses) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int t1_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                           utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  /* The pass should NOP the dead assignment. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, t1_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Live assignment: t1 = t0; use(t1) → keep t1
 * ======================================================================== */

UT_TEST(test_dce_live_assign)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_ctx_init_manual(&c);
  /* t0 = #1; t1 = t0; return t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int t1_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                           utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  /* The pass should keep t1 (it has a use). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, t1_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_dce)
{
  UT_COVERS("ssa:dce");
  UT_RUN(test_dce_dead_assign);
  UT_RUN(test_dce_live_assign);
}
