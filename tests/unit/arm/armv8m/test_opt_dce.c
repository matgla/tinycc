/*
 *  test_opt_dce.c - suite for ir/opt_dce.c (legacy Dead Code Elimination)
 *
 *  DCE follows control-flow edges from instruction 0, marks reachable
 *  instructions, and NOPs the rest.  It returns the number of instructions
 *  it converted to NOP.
 */

#include "ir_build.h"
#include "ut.h"

/* Pass entry point (defined in ir/opt_dce.c). */
int tcc_ir_opt_dce(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Emit an unconditional JUMP to target index `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* Emit a JUMPIF with target index `tgt` and condition temp `cond`. */
static int emit_jumpif(TCCIRState *ir, int tgt, int cond)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_temp(cond, I32), UTB_NONE);
}

/* --------------------------------------------------------- positive test */

/* Unreachable fall-through after an unconditional JUMP is NOPed:
 *   0: ADD T0 <- #1, #2
 *   1: JUMP -> 3
 *   2: ADD T1 <- #4, #5   (unreachable)
 *   3: RETURNVALUE #0
 * Only instruction 2 should be eliminated. */
UT_TEST(test_dce_unreachable_after_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A conditional JUMPIF keeps both its taken edge and its fall-through alive:
 *   0: JUMPIF -> 2, cond T0
 *   1: ADD T1 <- #1, #2
 *   2: RETURNVALUE #0
 * Nothing is dead. */
UT_TEST(test_dce_jumpif_keeps_both_targets)
{
  TCCIRState *ir = utb_new();

  emit_jumpif(ir, 2, 0);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The presence of an IJUMP makes static reachability unknowable, so the pass
 * bails out and returns 0 without mutating anything. */
UT_TEST(test_dce_ijump_skips_pass)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_IJUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- negative test */

/* Straight-line code with no branches is already fully reachable. */
UT_TEST(test_dce_straight_line_unchanged)
{
  TCCIRState *ir = utb_new();

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------ idempotence test */

/* A second DCE run should ideally report 0 changes because the first run
 * already NOPed everything unreachable.  The current implementation
 * recomputes reachability from scratch and counts every unreachable
 * instruction, including ones that are already NOP, so the second run
 * returns the same non-zero count as the first.
 *
 * SUSPECTED BUG: DCE does not skip already-NOP instructions when counting
 * changes, so it is not idempotent in its return value.  This may cause
 * the pass manager to schedule extra fixpoint iterations even though no
 * real transformation happens after the first run. */
UT_TEST(test_dce_second_run_reports_same_count)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int first = tcc_ir_opt_dce(ir);
  TccIrOp dead_op_after_first = utb_op(ir, dead);
  int second = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(first, 1);
  /* Current (possibly buggy) behavior: second pass re-counts the already-NOP
   * unreachable instruction.  Do not change production code; pin behavior. */
  UT_ASSERT_EQ(second, first);
  UT_ASSERT_EQ(dead_op_after_first, TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP); /* no new non-NOP -> NOP changes */
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_dce)
{
  UT_COVERS("dce");

  UT_RUN(test_dce_unreachable_after_jump);
  UT_RUN(test_dce_jumpif_keeps_both_targets);
  UT_RUN(test_dce_ijump_skips_pass);
  UT_RUN(test_dce_straight_line_unchanged);
  UT_RUN(test_dce_second_run_reports_same_count);
}
