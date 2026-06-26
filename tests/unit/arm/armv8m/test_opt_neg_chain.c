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

#define I8  IROP_BTYPE_INT8
#define I16 IROP_BTYPE_INT16

static inline int vreg_temp(int pos)
{
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos);
}

/* ------------------------------------------------------------------ tests */

/* INT8 boundary: a narrow negation chain must still CSE correctly. */
UT_TEST(test_neg_chain_int8_double_negation_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I8), utb_imm(0x80, I8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I8), utb_imm(0, I8), utb_temp(0, I8));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I8), utb_imm(0, I8), utb_temp(1, I8));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), vreg_temp(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* INT16 boundary: a 16-bit negation chain must still CSE correctly. */
UT_TEST(test_neg_chain_int16_double_negation_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I16), utb_imm(0x8000, I16), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I16), utb_imm(0, I16), utb_temp(0, I16));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I16), utb_imm(0, I16), utb_temp(1, I16));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), vreg_temp(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* INT64 boundary: 64-bit operands are not special-cased away. */
UT_TEST(test_neg_chain_int64_double_negation_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), utb_imm(1, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I64), utb_imm(0, I64), utb_temp(0, I64));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I64), utb_imm(0, I64), utb_temp(1, I64));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), vreg_temp(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Chain length 3: T0 -> -T0 -> T0 -> -T0 produces two folds. */
UT_TEST(test_neg_chain_length_3_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x80000000, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));
  int i3 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_imm(0, I32), utb_temp(2, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), vreg_temp(0));
  UT_ASSERT_EQ(utb_op(ir, i3), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i3)), vreg_temp(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Chain length 4/N: alternating negations continue to collapse. */
UT_TEST(test_neg_chain_length_4_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(42, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));
  int i3 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_imm(0, I32), utb_temp(2, I32));
  int i4 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(4, I32), utb_imm(0, I32), utb_temp(3, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 3);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), vreg_temp(0));
  UT_ASSERT_EQ(utb_op(ir, i3), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i3)), vreg_temp(1));
  UT_ASSERT_EQ(utb_op(ir, i4), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i4)), vreg_temp(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ★ Mixed-width link: an INT8 negation of an INT32 TEMP must not fold into the
 * 32-bit TEMP.  The first link itself cannot fold (widths differ); the second
 * link is same-width INT8 and must not be rewritten to an ASSIGN of T0. */
UT_TEST(test_neg_chain_mixed_width_int8_int32)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I8), utb_imm(0, I8), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I8), utb_imm(0, I8), utb_temp(1, I8));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  /* FIXED: a width-changing negation (INT8 = -INT32) is not value-preserving,
   * so it must not join the 32-bit base's canonical chain.  The second
   * same-width INT8 link therefore cannot be folded back to the wide T0; both
   * SUBs are preserved and changes == 0. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Merge-point reset: a back-edge makes the loop header a merge point, so the
 * canonical state accumulated before the loop must be cleared.  The post-loop
 * negation therefore cannot fold back to the pre-loop anchor. */
UT_TEST(test_neg_chain_merge_reset_clears_canon)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32), UTB_NONE);           /* 0 */
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32)); /* 1: loop header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_temp(9, I32), UTB_NONE);            /* 2: back-edge */
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32)); /* 3: after loop */

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Reuse after reset: once the canonical tables are cleared at a merge point, a
 * new canonical pair can be established and reused later in the same function. */
UT_TEST(test_neg_chain_reuse_after_reset)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);           /* 0 */
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));      /* 1: header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_temp(9, I32), UTB_NONE);            /* 2: back-edge */
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32)); /* 3 */
  int i3 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_imm(0, I32), utb_temp(2, I32)); /* 4 */

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i3), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i3)), vreg_temp(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Non-TEMP source operand: the pass only tracks TEMP negations, so a SUB of a
 * VAR must anchor to itself and not fold. */
UT_TEST(test_neg_chain_var_src_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_var(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Non-TEMP destination operand: a VAR destination is ignored entirely, so the
 * following TEMP negation has no canonical -T0 to fold against. */
UT_TEST(test_neg_chain_var_dest_ignored)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_var(0, I32), utb_imm(0, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* lval on the source operand: a dereferenced value is not a plain negation of
 * the underlying TEMP, so the chain must not be canonicalized. */
UT_TEST(test_neg_chain_lval_src2_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_lval(utb_temp(0, I32)));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* lval on the destination operand: the pass skips lval destinations, so the
 * SUB is not recorded as a negation. */
UT_TEST(test_neg_chain_lval_dest_ignored)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_SUB, utb_lval(utb_temp(1, I32)), utb_imm(0, I32), utb_temp(0, I32));
  int i2 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(0, I32));

  int changes = tcc_ir_opt_neg_chain_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Idempotence / fixpoint: one application folds all opportunities; a second
 * application makes no further changes. */
UT_TEST(test_neg_chain_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(0, I32), utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_imm(0, I32), utb_temp(1, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_neg_chain_cse, 10);
  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(tcc_ir_opt_neg_chain_cse(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

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
  UT_RUN(test_neg_chain_int8_double_negation_folds);
  UT_RUN(test_neg_chain_int16_double_negation_folds);
  UT_RUN(test_neg_chain_int64_double_negation_folds);
  UT_RUN(test_neg_chain_length_3_folds);
  UT_RUN(test_neg_chain_length_4_folds);
  UT_RUN(test_neg_chain_mixed_width_int8_int32);
  UT_RUN(test_neg_chain_merge_reset_clears_canon);
  UT_RUN(test_neg_chain_reuse_after_reset);
  UT_RUN(test_neg_chain_var_src_no_fold);
  UT_RUN(test_neg_chain_var_dest_ignored);
  UT_RUN(test_neg_chain_lval_src2_no_fold);
  UT_RUN(test_neg_chain_lval_dest_ignored);
  UT_RUN(test_neg_chain_idempotent);
}
