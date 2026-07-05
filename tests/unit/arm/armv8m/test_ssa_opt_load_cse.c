/*
 *  test_ssa_opt_load_cse.c - load common subexpression elimination
 *
 *  Phase 4: Large, high-bug-density pass.
 *
 *  Covers:
 *    - Stack Store-Load Forwarding: forward tracked stack stores into loads
 *    - T_vreg-deref forwarding: forward stores through TEMP pointer derefs
 *    - LOAD_INDEXED CSE: dedup indexed loads with constant idx/scale
 *    - Invalidation: calls, stores to overlapping addresses
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_load_cse.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - Some tests need a real tcc_state for USING_GLOBALS; see setup_tcc_state.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define I8  IROP_BTYPE_INT8

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* Helper to set up tcc_state for tests that need USING_GLOBALS. */
static void setup_tcc_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
}

static void teardown_tcc_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_exit_state(&tcc_state_storage);
}

/* ========================================================================
 * Stack Store-Load Forwarding: tracked stack store forwarded into load
 * ======================================================================== */

UT_TEST(test_stack_fwd_basic)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] <- t0; t1 = LOAD(StackLoc[0]) → t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Store-Load Forwarding: no tracked store → no forwarding
 * ======================================================================== */

UT_TEST(test_stack_fwd_no_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 3);
  /* t0 = LOAD(StackLoc[0]) — no prior store → no forwarding */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Store-Load Forwarding: overlapping store invalidates earlier tracking
 * ======================================================================== */

UT_TEST(test_stack_fwd_overlap_invalidates)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 6);
  /* StackLoc[0] <- t0; StackLoc[0] <- t1; t2 = LOAD(StackLoc[0]) → t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(1, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Should forward from t1 (the later store), not t0 */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * T_vreg-deref forwarding: store through TEMP pointer forwarded into load
 * ======================================================================== */

UT_TEST(test_tvstore_fwd_basic)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = t2; t1 = *t0 → t1 = t2 */
  /* t0 = LEA(StackLoc[0]) */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 1, 0, 0, I32));
  /* *t0 = t2 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_temp(2, I32));
  /* t1 = *t0 */
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Load should be rewritten to ASSIGN t2 */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * LOAD_INDEXED CSE: same indexed load CSE'd
 * ======================================================================== */

UT_TEST(test_iload_cse_duplicate)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = *(t1 + #0<<2); t2 = *(t1 + #0<<2) → t2 = t0 */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));
  int t2_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                            utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Second load should be rewritten to ASSIGN t0 */
  UT_ASSERT_EQ(utb_op(c.ir, t2_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * LOAD_INDEXED CSE: different indices → no CSE
 * ======================================================================== */

UT_TEST(test_iload_cse_different_idx)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = *(t1 + #0<<2); t2 = *(t1 + #1<<2) → no CSE */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                 utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Invalidation: function call clears all forwarding state
 * ======================================================================== */

UT_TEST(test_invalidated_by_call)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* StackLoc[0] <- t0; call foo(); t1 = LOAD(StackLoc[0]) → no fwd (call invalidates) */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_load_cse)
{
  UT_COVERS("ssa:load_cse");
  UT_RUN(test_stack_fwd_basic);
  UT_RUN(test_stack_fwd_no_store);
  UT_RUN(test_stack_fwd_overlap_invalidates);
  UT_RUN(test_tvstore_fwd_basic);
  UT_RUN(test_iload_cse_duplicate);
  UT_RUN(test_iload_cse_different_idx);
  UT_RUN(test_invalidated_by_call);
}
