/*
 *  test_opt_cmpfold.c - suite for the comparison fold/fuse passes
 *  (ir/opt_constprop.c: tcc_ir_opt_cmp_expr_fold / tcc_ir_opt_cmp_const_offset_fold,
 *   ir/opt_cmp_fuse.c: tcc_ir_opt_cmp_field_fuse).
 *
 *  These isolated tests drive tcc_ir_opt_cmp_const_offset_fold, the entry point
 *  that is cleanest to exercise on a hand-built IR: it needs no live-interval
 *  arrays nor a real Sym, only the linear def-finding helpers.
 *
 *  cmp_const_offset_fold collapses `A = B (+/-) K ; CMP A,B ; JUMPIF cond`
 *  into a constant branch by substituting A = B + K (so the comparison reduces
 *  to `K cond 0`).  When the condition is statically true it rewrites the CMP
 *  to NOP and the JUMPIF to an unconditional JUMP (then runs DCE); when false
 *  it NOPs both.  Guards (asserted as no-ops here): the ADD base's lval-ness
 *  must match the CMP operand's (the historical wide-string-literal heap crash
 *  came from a missing is_lval guard in these cmp-fold passes), the condition
 *  must be signed or EQ/NE, and K must be non-zero.
 *
 *  A hand-built IR sequence is run through the bare pass entry point and the
 *  resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir);
int tcc_ir_opt_cmp_const_offset_fold(TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Comparison condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_GT 0x9f  /* signed >  */
#define TOK_LT 0x9c  /* signed <  */
#define TOK_ULT 0x92 /* unsigned < */

/* ------------------------------------------------------------------ tests */

/* POSITIVE: `A = B + 5 ; CMP A,B ; JUMPIF >` reduces to `5 > 0` == true.
 *   i0: ADD   T1 <- T0 + 5
 *   i1: CMP   T1, T0
 *   i2: JUMPIF (>) -> #4
 *   i3: RETURNVOID         (dead after fold: only reachable via fall-through
 *                           from the JUMPIF, which became an unconditional JUMP)
 *   i4: RETURNVOID         (jump target)
 * After fold: CMP -> NOP, JUMPIF -> unconditional JUMP to the same target. */
UT_TEST(test_cmpfold_offset_signed_true_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  /* The pass fired: CMP folded away, JUMPIF became an unconditional JUMP to the
   * original target.  changes also includes the follow-up DCE, so assert > 0. */
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* GUARD (the historical is_lval bug): the ADD base is a plain value `T0` but the
 * CMP's second operand is a deref `*(T0)` (is_lval).  `*(p)+K` (a loaded value)
 * does not make `A == p + K` provable from `B == p`, so the pass must NOT fold.
 *   i0: ADD   T1 <- T0 + 5      (base T0 NOT lval)
 *   i1: CMP   T1, *(T0)         (src2 = T0 with is_lval = 1)
 *   i2: JUMPIF (>) -> #4 */
UT_TEST(test_cmpfold_offset_lval_base_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();

  IROperand t0_deref = utb_temp(0, I32);
  t0_deref.is_lval = 1;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), t0_deref);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: unsigned conditions need an overflow proof and are skipped.  Same
 * foldable arithmetic shape as the positive test but with an unsigned `<`. */
UT_TEST(test_cmpfold_offset_unsigned_cond_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_ULT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a zero offset (`A = B + 0`) gives delta 0; the pass bails on k == 0
 * (the comparison is genuinely `B vs B`, handled elsewhere), so no fold here. */
UT_TEST(test_cmpfold_offset_zero_delta_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: no ADD/SUB feeding either CMP operand, so there is no provable
 * constant offset and nothing folds. */
UT_TEST(test_cmpfold_offset_no_arith_def_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_cmpfold)
{
  UT_COVERS("cmp_fold");
  UT_RUN(test_cmpfold_offset_signed_true_folds_to_jump);
  UT_RUN(test_cmpfold_offset_lval_base_mismatch_no_fold);
  UT_RUN(test_cmpfold_offset_unsigned_cond_no_fold);
  UT_RUN(test_cmpfold_offset_zero_delta_no_fold);
  UT_RUN(test_cmpfold_offset_no_arith_def_no_fold);
}
