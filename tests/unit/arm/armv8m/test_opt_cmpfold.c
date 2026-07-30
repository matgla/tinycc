/*
 *  test_opt_cmpfold.c - suite for the comparison fold/fuse passes
 *  (ir/opt_constprop.c: tcc_ir_opt_cmp_expr_fold,
 *   ir/opt_cmp_fuse.c: tcc_ir_opt_cmp_field_fuse).
 *
 *  These isolated tests drive the comparison-fold entry points on hand-built IR.
 *
 *  A hand-built IR sequence is run through the bare pass entry point and the
 *  resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

#include <limits.h>

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Comparison condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_EQ  0x94 /* ==        */
#define TOK_NE  0x95 /* !=        */
#define TOK_LT  0x9c /* signed <  */
#define TOK_LE  0x9e /* signed <= */
#define TOK_GT  0x9f /* signed >  */
#define TOK_GE  0x9d /* signed >= */
#define TOK_ULT 0x92 /* unsigned < */

/* ------------------------------------------------------------------ tests */

/* ----------------------------------------------------------- helpers */

/* Live-interval setup needed by passes that allocate fresh temporaries
 * (cmp_field_fuse) or read interval flags (cmp_expr_fold asymmetric path). */
static void utb_init_intervals_with_temp_base(TCCIRState *ir, int temp_base, int size)
{
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->temporary_variables_live_intervals_size = size;
  ir->next_temporary_variable = temp_base;

  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->variables_live_intervals_size = size;
  ir->next_local_variable = 0;

  ir->parameters_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->parameters_live_intervals_size = size;
  ir->next_parameter = 0;
}

static void utb_free_intervals(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_free(ir->temporary_variables_live_intervals);
  tcc_free(ir->variables_live_intervals);
  tcc_free(ir->parameters_live_intervals);
  ir->temporary_variables_live_intervals = NULL;
  ir->variables_live_intervals = NULL;
  ir->parameters_live_intervals = NULL;
}

/* AND-extract feeder for cmp_field_fuse tests.  src_is_lval lets us test the
 * lval base guard without mutating operand bitfields by hand. */
static int utb_emit_and_extract(TCCIRState *ir, int dest_pos, int src_param_pos,
                                int32_t mask, int src_is_lval)
{
  IROperand src = utb_param(src_param_pos, I32);
  if (src_is_lval)
    src = utb_lval(src);
  return utb_emit(ir, TCCIR_OP_AND, utb_temp(dest_pos, I32), src, utb_imm(mask, I32));
}

/* -------------------------------- cmp_expr_fold corner cases */

/* FIXED: comparing a register value to itself.  EQ is always true, so the
 * fold is NOP(CMP) + JUMP(target).  The pass now folds identical (non-lval)
 * vregs via evaluate_compare_condition(0,0,tok). */
UT_TEST(test_cmpfold_expr_same_vreg_eq_true_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* FIXED: x > x is always false, so CMP+JUMPIF GT folds to two NOPs (fall
 * through). */
UT_TEST(test_cmpfold_expr_same_vreg_gt_false_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* FIXED: unsigned compare of a value to itself.  (uint32_t)x < x is always
 * false, so the CMP+JUMPIF ULT folds to two NOPs. */
UT_TEST(test_cmpfold_expr_same_vreg_ult_false_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* A plain value and a dereference of the same vreg are different values,
 * so the equality fold must not fire. */
UT_TEST(test_cmpfold_expr_same_vreg_lval_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();
  IROperand rhs = utb_lval(utb_temp(0, I32));

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), rhs);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* FIXED: two equal immediate operands fold (EQ is true).  The both-nonvreg
 * branch now compares integer immediates by value.  (Enabling this also
 * required fixing a latent use-before-def bug in `single_value_tmp` that the
 * extra fold exposed — see Findings #6 in PASS_COVERAGE.md.) */
UT_TEST(test_cmpfold_expr_imm_imm_equal_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(7, I32), utb_imm(7, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: single-def temp assigned an immediate equal to the other
 * CMP operand.  EQ folds to an unconditional jump. */
UT_TEST(test_cmpfold_expr_asymmetric_vreg_imm_eq_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(9, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: same shape as above, NE of equal values is false. */
UT_TEST(test_cmpfold_expr_asymmetric_vreg_imm_ne_false_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* Two distinct temps that compute the same pure expression are value-equal,
 * so the comparison folds. */
UT_TEST(test_cmpfold_expr_pure_def_equal_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_param(0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(4, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* FIXPOINT: a foldable CMP+JUMPIF converges in one run; the second run makes
 * no changes and the IR stays well-formed. */
UT_TEST(test_cmpfold_expr_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(9, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_expr_fold, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* DEGENERATE: empty or too-short IR returns 0 without crashing. */
UT_TEST(test_cmpfold_expr_empty_and_tiny)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  return 0;
}

/* -------------------------------- cmp_field_fuse corner cases */

/* Field width 1: two single-bit fields (bit 0 and bit 31) fuse. */
UT_TEST(test_cmpfold_field_fuse_width_1_bits)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00000001, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00000001, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x80000000, 0);
  utb_emit_and_extract(ir, 3, 1, 0x80000000, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);

  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 4));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 5)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, 5)), 0x80000001u);

  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, c2)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, c2)), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* Field width 31: a 31-bit mask fused with a single-bit mask. */
UT_TEST(test_cmpfold_field_fuse_width_31)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x7FFFFFFF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x7FFFFFFF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x80000000, 0);
  utb_emit_and_extract(ir, 3, 1, 0x80000000, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* ≥3 fields: three byte fields fuse into one XOR+AND+CMP. */
UT_TEST(test_cmpfold_field_fuse_three_fields)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 6, 16);

  utb_emit_and_extract(ir, 0, 0, 0x000000FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x000000FF, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x0000FF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0x0000FF00, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 4, 0, 0x00FF0000, 0);
  utb_emit_and_extract(ir, 5, 1, 0x00FF0000, 0);
  int c3 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(4, I32), utb_temp(5, I32));
  int j3 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c3), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j3), TCCIR_OP_JUMPIF);

  UT_ASSERT_EQ(utb_op(ir, 8), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 6));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  UT_ASSERT_EQ(utb_op(ir, 9), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 9)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 7));
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, 9)), 0x00FFFFFFu);

  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c3)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 7));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, c3)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, c3)), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* FIXPOINT: cmp_field_fuse converges after one fusion. */
UT_TEST(test_cmpfold_field_fuse_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_field_fuse, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

UT_COVERS("cmp_fold");
