/*
 *  test_opt_float_branch.c - suite for ir/opt_branch.c float_branch_fold
 *
 *  tcc_ir_opt_float_branch_fold() folds redundant branches after soft-float
 *  flag-comparison helper calls (__aeabi_cfcmple / __aeabi_cdcmple) and after
 *  TEST_ZERO patterns.  If a first comparison/test proves the result of a
 *  second identical comparison, the second compare + JUMPIF are either turned
 *  into an unconditional JUMP or NOP-ed away.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_float_branch_fold(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Bound large enough for encoded vreg values (type<<28 | position). */
#define UTB_VREG_BOUND 0x30000010

/* Condition tokens used in JUMPIF src1 (see evaluate_compare_condition). */
#define TOK_EQ 0x94
#define TOK_NE 0x95

/* Token used to name the flag-comparison helper via the harness get_tok_str
 * table.  The pass recognizes __aeabi_cfcmple / __aeabi_cdcmple. */
#define TOK_FCMP 101

/* Distinct call ids so ir_opt_get_call_param_operand does not confuse the
 * parameters of the first and second helper calls. */
#define CALL_ID_1 1
#define CALL_ID_2 2

/* ----------------------------------------------------------------- helpers */

/* Read a jump's current target index. */
static int jump_target(TCCIRState *ir, int i)
{
  return (int)utb_dest(ir, i).u.imm32;
}

/* Build a SYMREF callee operand whose token is `tok`.  Caller must have called
 * utb_pools_init(ir) first. */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Emit a JUMPIF (conditional branch) with condition token `tok`. */
static int emit_jumpif(TCCIRState *ir, int tgt, int tok)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_imm(tok, I32), UTB_NONE);
}

/* Emit a single-precision flag-comparison helper call:
 *     __aeabi_cfcmple(a, b)
 * The call consumes two FUNCPARAMVAL instructions immediately preceding it. */
static int emit_fcmp_call(TCCIRState *ir, int call_id, IROperand callee,
                          IROperand a, IROperand b)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, a,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, b,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: two identical __aeabi_cfcmple helper calls with the same operands.
 * First branch is JUMPIF NE; after it falls through the comparison is known to
 * be EQ.  The second JUMPIF NE can therefore never be taken, so the second
 * helper call and its jump are NOP-ed. */
UT_TEST(test_float_branch_helper_same_ops_ne_then_ne_nop)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  emit_jumpif(ir, 7, TOK_NE);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, b);
  int ijmp2 = emit_jumpif(ir, 9, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: same helper/operands, first branch is JUMPIF EQ.  After the first
 * branch falls through the comparison is known to be NE, so the second JUMPIF
 * NE is always true and becomes an unconditional JUMP. */
UT_TEST(test_float_branch_helper_same_ops_eq_then_ne_jump)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  emit_jumpif(ir, 7, TOK_EQ);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, b);
  int ijmp2 = emit_jumpif(ir, 9, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, ijmp2), 9);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: second helper call uses the same two operands in swapped order, with
 * differing jump targets.  For a non-strict known fact (here EQ -> known NE) the
 * fold pass conservatively declines the swapped case: float comparisons are
 * unordered w.r.t. NaN, so cfcmple(a,b) vs cfcmple(b,a) only reason soundly for
 * strict orderings (LT/GT/ULT/UGT).  No fold occurs. */
UT_TEST(test_float_branch_helper_swapped_ops_eq_then_eq_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  emit_jumpif(ir, 7, TOK_EQ);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, b, a);
  int ijmp2 = emit_jumpif(ir, 9, TOK_EQ);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: TEST_ZERO followed by JUMPIF NE.  After the first branch falls
 * through the value is known to be zero, so a second TEST_ZERO + JUMPIF NE
 * (jump-if-nonzero) can never be taken: both instructions are NOP-ed. */
UT_TEST(test_float_branch_test_zero_ne_then_ne_nop)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  emit_jumpif(ir, 4, TOK_NE);
  int itest2 = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  int ijmp2 = emit_jumpif(ir, 5, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, itest2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: TEST_ZERO followed by JUMPIF EQ.  After the first branch falls
 * through the value is known to be non-zero, so a second TEST_ZERO + JUMPIF NE
 * (jump-if-nonzero) is always taken and becomes an unconditional JUMP. */
UT_TEST(test_float_branch_test_zero_eq_then_ne_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  emit_jumpif(ir, 4, TOK_EQ);
  int itest2 = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  int ijmp2 = emit_jumpif(ir, 5, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, itest2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, ijmp2), 5);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a non-flag-helper call is ignored entirely.  Even though the shape is
 * identical, the callee name "foo" is not in the recognized flag-cmp set. */
UT_TEST(test_float_branch_non_flag_helper_ignored)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "foo");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  int icall1 = emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  int ijmp1 = emit_jumpif(ir, 7, TOK_EQ);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, b);
  int ijmp2 = emit_jumpif(ir, 9, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall1), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: mismatched helper operands block the fold.  The second call compares
 * a against c instead of b, so the result is not implied by the first. */
UT_TEST(test_float_branch_mismatched_operands_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);
  IROperand c = utb_temp(2, I32);

  emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  emit_jumpif(ir, 7, TOK_EQ);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, c);
  int ijmp2 = emit_jumpif(ir, 9, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a merge point between the first and second helper calls stops the
 * scan.  The second helper is reachable from the fall-through of the first
 * branch and from an unconditional JUMP, so its incoming state is unknown. */
UT_TEST(test_float_branch_merge_point_blocks_scan)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  emit_jumpif(ir, 8, TOK_EQ);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, b);
  int ijmp2 = emit_jumpif(ir, 10, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: the instruction immediately following the first helper call must be a
 * JUMPIF for the pass to engage.  A RETURNVOID there blocks folding. */
UT_TEST(test_float_branch_non_jumpif_after_helper_blocks_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  IROperand a = utb_temp(0, I32);
  IROperand b = utb_temp(1, I32);

  int icall1 = emit_fcmp_call(ir, CALL_ID_1, callee, a, b);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int icall2 = emit_fcmp_call(ir, CALL_ID_2, callee, a, b);
  int ijmp2 = emit_jumpif(ir, 9, TOK_NE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_float_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall1), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, icall2), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_float_branch)
{
  UT_COVERS("float_branch");

  UT_RUN(test_float_branch_helper_same_ops_ne_then_ne_nop);
  UT_RUN(test_float_branch_helper_same_ops_eq_then_ne_jump);
  UT_RUN(test_float_branch_helper_swapped_ops_eq_then_eq_no_fold);
  UT_RUN(test_float_branch_test_zero_ne_then_ne_nop);
  UT_RUN(test_float_branch_test_zero_eq_then_ne_jump);

  UT_RUN(test_float_branch_non_flag_helper_ignored);
  UT_RUN(test_float_branch_mismatched_operands_no_fold);
  UT_RUN(test_float_branch_merge_point_blocks_scan);
  UT_RUN(test_float_branch_non_jumpif_after_helper_blocks_fold);
}
