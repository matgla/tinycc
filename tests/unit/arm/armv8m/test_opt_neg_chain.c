/*
 *  test_opt_neg_chain.c - suite for ir/opt_neg_chain.c (negation-chain CSE)
 *
 *  tcc_ir_opt_neg_chain_cse tracks each TEMP as a canonical (base, sign) pair
 *  (sign = parity of accumulated negations).  When `T_b = #0 SUB T_a` recomputes
 *  a (base, sign) already produced by an earlier TEMP T_y, the SUB is rewritten
 *  as `T_b = T_y` (ASSIGN), to be collapsed by a later copy-prop + DCE.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_neg_chain_cse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* ------------------------------------------------------------------ tests */

/* T0 = 5            (anchor)
 * T1 = -T0          (first negation of T0)
 * T2 = -T1 == T0    -> folds to  T2 = T0  (ASSIGN), because the canonical
 *                      (base=T0, sign=+) was already produced by the anchor. */
UT_TEST(test_neg_chain_double_negation_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));

  utb_free(ir);
  return 0;
}

/* A single negation has nothing to CSE against -> no change, SUB preserved. */
UT_TEST(test_neg_chain_single_negation_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);

  utb_free(ir);
  return 0;
}

/* `T_b = imm - T_a` with imm != 0 is plain subtraction, not negation -> no fold,
 * even when it forms a chain that would otherwise be canonicalizable. */
UT_TEST(test_neg_chain_nonzero_minuend_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(7, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(7, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);

  utb_free(ir);
  return 0;
}

/* Width guard: the canonical match exists, but the final SUB's dest width
 * differs from the negated source width, so folding it to an ASSIGN could
 * drop/extend bits the SUB wouldn't have -> the pass must NOT fold. */
UT_TEST(test_neg_chain_width_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));
  /* dest T2 is 64-bit while src2 T1 is 32-bit -> dest_btype != src_btype. */
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I64), utb_imm(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_neg_chain)
{
  UT_COVERS("neg_chain_cse");
  UT_RUN(test_neg_chain_double_negation_folds);
  UT_RUN(test_neg_chain_single_negation_no_fold);
  UT_RUN(test_neg_chain_nonzero_minuend_no_fold);
  UT_RUN(test_neg_chain_width_mismatch_no_fold);
}
