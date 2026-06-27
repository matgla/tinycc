/*
 *  test_opt_redundant_assign.c - suite for ir/opt_dce.c:redundant_var_assign
 *
 *  tcc_ir_opt_redundant_var_assign forward-scans basic blocks and NOPs a VAR
 *  assignment that is overwritten by a later assignment to the same VAR before
 *  any intervening read.  It flushes its pending-assign table at jump targets,
 *  terminators, calls, and returns.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"
#include "ut.h"

/* Pass entry point (declared in ir/opt.h). */
int tcc_ir_opt_redundant_var_assign(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Encoded vreg helpers for assertions. */
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))
#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))

/* Bound large enough for encoded vreg values (type<<28 | position). */
#define UTB_VREG_BOUND 0x30000010

/* ------------------------------------------------------------------ tests */

/* POSITIVE: two back-to-back assignments to the same VAR with no read in
 * between -> the first one is dead and gets NOP'd.
 *
 *   V1 <- #1   -> NOP
 *   V1 <- #2   (kept)
 */
UT_TEST(test_redundant_var_assign_positive)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_redundant_var_assign(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, i1)), VR_VAR(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a read of the VAR between the two assignments clears the pending
 * assign, so the first store is observable and must be kept.
 *
 *   V1 <- #1
 *   T0 = ADD V1, #1   (reads V1)
 *   V1 <- #2
 */
UT_TEST(test_redundant_var_assign_read_keeps)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_redundant_var_assign(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a jump target between the two assignments flushes pending state,
 * so the first assign is not eliminated even though it is overwritten on the
 * only incoming edge.
 *
 *   V1 <- #1
 *   JUMP -> 2
 * L1:
 *   V1 <- #2
 */
UT_TEST(test_redundant_var_assign_jump_target_flushes)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_redundant_var_assign(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE / CONVERGENCE: after the first run eliminates the dead assign,
 * a second run reports zero changes and leaves the IR unchanged.
 */
UT_TEST(test_redundant_var_assign_idempotent)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(2, I32), UTB_NONE);

  int c1 = tcc_ir_opt_redundant_var_assign(ir);
  int c2 = tcc_ir_opt_redundant_var_assign(ir);

  UT_ASSERT_EQ(c1, 1);
  UT_ASSERT_EQ(c2, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* SUSPECTED BUG: the pass returns 0 immediately when the highest VAR position
 * is 0 (max_var == 0), so redundant assigns to VAR 0 are not eliminated even
 * though they are provably dead.  This test asserts the current (buggy)
 * behavior rather than the ideal one.
 *
 *   V0 <- #1
 *   V0 <- #2   (should make the first dead, but doesn't today)
 */
UT_TEST(test_redundant_var_assign_var0_skipped)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_redundant_var_assign(ir);

  /* SUSPECTED BUG: pass bails because max_var == 0, so no change is reported
   * and the first assign stays intact. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* DEGENERATE: an empty IR returns 0 without crashing. */
UT_TEST(test_redundant_var_assign_empty)
{
  TCCIRState *ir = utb_new();

  int changes = tcc_ir_opt_redundant_var_assign(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_redundant_assign)
{
  UT_COVERS("redundant_assign");
  UT_RUN(test_redundant_var_assign_positive);
  UT_RUN(test_redundant_var_assign_read_keeps);
  UT_RUN(test_redundant_var_assign_jump_target_flushes);
  UT_RUN(test_redundant_var_assign_idempotent);
  UT_RUN(test_redundant_var_assign_var0_skipped);
  UT_RUN(test_redundant_var_assign_empty);
}
