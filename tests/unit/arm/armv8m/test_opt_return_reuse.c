/*
 *  test_opt_return_reuse.c - suite for ir/opt_dce.c return-constant register reuse
 *  (legacy pass "return_reuse" / tcc_ir_opt_return_const_reuse).
 *
 *  A RETURNVALUE that returns an integer immediate C, and that is reached only
 *  via the equality edge of a TEST_ZERO V (C == 0) or CMP V, #C, is rewritten
 *  to return V.  The register holding V can then be reused by the backend
 *  instead of rematerializing C.
 *
 *  These isolated tests drive the bare TCCIRState* entry point on hand-built IR.
 */

#include "ir_build.h"

#include "opt_engine.h"
#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_return_const_reuse(TCCIRState *ir);
int tcc_ir_opt_return_const_reuse_ex(IROptCtx *ctx);

#define I32 IROP_BTYPE_INT32

/* JUMPIF condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_EQ 0x94
#define TOK_NE 0x95

/* ------------------------------------------------------------------ helpers */

static void setup_optimize_for_return_reuse(void)
{
  /* The pass is gated on tcc_state->optimize >= 2. */
  tcc_state->optimize = 2;
}

static void reset_optimize(void)
{
  tcc_state->optimize = 0;
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: TEST_ZERO proves P0 == 0 on the EQ edge, so the equality-target
 * RETURNVALUE #0 is rewritten to RETURNVALUE P0.
 *   i0: TEST_ZERO P0
 *   i1: JUMPIF EQ -> i3
 *   i2: RETURNVALUE #1          (other path, diversion)
 *   i3: RETURNVALUE #0          (target, rewritten to P0) */
UT_TEST(test_return_reuse_test_zero_eq_rewrites_return)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int other_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, other_ret)));
  UT_ASSERT(!irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ret)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* POSITIVE: CMP P0, #7 proves P0 == 7 on the EQ edge, so the equality-target
 * RETURNVALUE #7 is rewritten to RETURNVALUE P0.
 *   i0: CMP P0, #7
 *   i1: JUMPIF EQ -> i3
 *   i2: RETURNVALUE #1
 *   i3: RETURNVALUE #7          (target, rewritten to P0) */
UT_TEST(test_return_reuse_cmp_nonzero_const_rewrites_return)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(!irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ret)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* POSITIVE with TEMP: the proven register can be a TEMP as well as a PARAM.
 *   i0: ADD T0 <- P0 + #0
 *   i1: CMP T0, #5
 *   i2: JUMPIF EQ -> i4
 *   i3: RETURNVALUE #9
 *   i4: RETURNVALUE #5          (rewritten to T0) */
UT_TEST(test_return_reuse_temp_proven_eq_rewrites_return)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(9, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(!irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ret)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* NEGATIVE: the only predecessor is a NE edge, not an EQ edge, so the
 * constant return is not provably reached with V == C.
 *   i0: TEST_ZERO P0
 *   i1: JUMPIF NE -> i3
 *   i2: RETURNVALUE #1
 *   i3: RETURNVALUE #0          (must stay #0) */
UT_TEST(test_return_reuse_ne_edge_no_rewrite)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ret)), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* NEGATIVE: the constant returned does not match the constant proved by the
 * comparison, so no rewrite is sound.
 *   i0: CMP P0, #7
 *   i1: JUMPIF EQ -> i3
 *   i2: RETURNVALUE #1
 *   i3: RETURNVALUE #5          (must stay #5: P0 may not equal 5 here) */
UT_TEST(test_return_reuse_const_mismatch_no_rewrite)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ret)), 5);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* NEGATIVE: the RETURNVALUE has a fall-through predecessor (the instruction
 * immediately before it is not an unconditional diversion), so it is reached
 * on a path where V may not equal C.
 *   i0: CMP P0, #0
 *   i1: JUMPIF EQ -> i3
 *   i2: ASSIGN T0 <- #1         (non-diversion; execution can fall through)
 *   i3: RETURNVALUE #0          (must stay #0) */
UT_TEST(test_return_reuse_fallthrough_no_rewrite)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ret)), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* NEGATIVE: the proven value is a memory dereference (is_lval), not a plain
 * register value, so the equality does not guarantee the register contents.
 *   i0: CMP *(P0), #0
 *   i1: JUMPIF EQ -> i3
 *   i2: RETURNVALUE #1
 *   i3: RETURNVALUE #0          (must stay #0) */
UT_TEST(test_return_reuse_lval_proven_no_rewrite)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  IROperand p0_deref = utb_lval(utb_param(0, I32));
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, p0_deref, utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, ret)));

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* NEGATIVE: the pass is gated on optimize >= 2; at optimize 0 it must not fire. */
UT_TEST(test_return_reuse_optimize_gate)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 0;

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, ret)));

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* FIXPOINT / IDEMPOTENCE: a second run after a successful rewrite must make no
 * further changes and the IR must stay well-formed. */
UT_TEST(test_return_reuse_idempotent)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(42, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(42, I32), UTB_NONE);

  int c1 = tcc_ir_opt_return_const_reuse(ir);
  int c2 = tcc_ir_opt_return_const_reuse(ir);

  UT_ASSERT_EQ(c1, 1);
  UT_ASSERT_EQ(c2, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* DEGENERATE: tiny/empty IR returns 0 without crashing. */
UT_TEST(test_return_reuse_empty_and_tiny)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();
  UT_ASSERT_EQ(tcc_ir_opt_return_const_reuse(ir), 0);
  utb_free(ir);

  ir = utb_new();
  setup_optimize_for_return_reuse();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_return_const_reuse(ir), 0);
  utb_free(ir);

  reset_optimize();
  return 0;
}

/* WRAPPER: the IROptCtx entry point forwards to the bare TCCIRState* pass. */
UT_TEST(test_return_reuse_ex_forwards)
{
  TCCIRState *ir = utb_new();
  setup_optimize_for_return_reuse();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(7, I32), UTB_NONE);

  IROptCtx ctx = {0};
  ctx.ir = ir;
  int changes = tcc_ir_opt_return_const_reuse_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(!irop_is_immediate(utb_src1(ir, ret)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ret)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_return_reuse)
{
  UT_COVERS("return_reuse");

  UT_RUN(test_return_reuse_test_zero_eq_rewrites_return);
  UT_RUN(test_return_reuse_cmp_nonzero_const_rewrites_return);
  UT_RUN(test_return_reuse_temp_proven_eq_rewrites_return);
  UT_RUN(test_return_reuse_ne_edge_no_rewrite);
  UT_RUN(test_return_reuse_const_mismatch_no_rewrite);
  UT_RUN(test_return_reuse_fallthrough_no_rewrite);
  UT_RUN(test_return_reuse_lval_proven_no_rewrite);
  UT_RUN(test_return_reuse_optimize_gate);
  UT_RUN(test_return_reuse_idempotent);
  UT_RUN(test_return_reuse_empty_and_tiny);
  UT_RUN(test_return_reuse_ex_forwards);
}
