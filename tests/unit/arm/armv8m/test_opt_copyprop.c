/*
 *  test_opt_copyprop.c - suite for ir/opt_copyprop.c (copy propagation)
 *
 *  tcc_ir_opt_copy_prop tracks ASSIGN "copies" of the form
 *      TMP:X <- VAR:Y | PARAM:Y | TMP:Y      (src not constant, not lval)
 *  and rewrites later uses of TMP:X with the recorded source operand, as long
 *  as the source has not been redefined between the copy and the use and no
 *  basic-block boundary / terminator / FUNCCALL has cleared the copy table.
 *
 *  Key guards verified here:
 *    - lval (DEREF) uses keep their is_lval / load-width bits when the source is
 *      substituted in, and a VAR/PARAM source is NOT propagated into an lval use
 *      (only a TMP source is).
 *    - ASSIGN with an lval source is a LOAD, not a copy, so it is NOT recorded.
 *    - a constant source is not a copy and is NOT recorded.
 *    - redefining the source before the use invalidates the copy.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_copy_prop(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I16 IROP_BTYPE_INT16
#define I64 IROP_BTYPE_INT64

/* Encoded vreg helpers for assertions. */
#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))
#define VR_PARAM(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, (p))

/* ------------------------------------------------------------------ tests */

/* POSITIVE: a plain VAR copy propagates into an arithmetic use.
 *   T1 <- V0            [ASSIGN copy]
 *   T2 = T1 ADD #1      -> src1 rewritten to V0
 * changes > 0, and T2.src1 becomes V0. */
UT_TEST(test_copyprop_var_copy_propagates_to_add)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  /* The ADD's src1 must now reference the copy source V0, not T1. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_VAR(0));
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a TMP->TMP copy propagates into BOTH src1 and src2 of one use.
 *   T1 <- T0
 *   T3 = T1 ADD T1      -> both operands rewritten to T0 (two changes) */
UT_TEST(test_copyprop_tmp_copy_propagates_both_operands)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  /* src1 and src2 are each rewritten -> at least two propagations. */
  UT_ASSERT(changes >= 2);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, iadd)), VR_TMP(0));

  utb_free(ir);
  return 0;
}

/* is_lval PRESERVATION: a TMP->TMP copy of an address propagates into an lval
 * (DEREF) use while keeping the deref + load-width bits taken from the use site.
 *   T1 <- T0                         (register-to-register address copy)
 *   T2 = LOAD T1***DEREF*** (INT16)  -> src1 becomes T0 but stays lval, INT16 */
UT_TEST(test_copyprop_lval_use_preserves_deref_and_width)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  /* Build the LOAD's lval src1 by hand (DEREF, narrow INT16 load). */
  IROperand load_src = utb_temp(1, I16);
  load_src.is_lval = 1;
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), load_src, UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand s1 = utb_src1(ir, iload);
  /* Substituted to the copy source T0... */
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(0));
  /* ...but the DEREF semantics and the use-site load width are preserved. */
  UT_ASSERT_EQ((int)s1.is_lval, 1);
  UT_ASSERT_EQ(irop_get_btype(s1), I16);

  utb_free(ir);
  return 0;
}

/* GUARD (lval source NOT propagated for VAR): an lval use whose copy source is a
 * VAR must NOT be rewritten, because propagating a VAR into a DEREF would extend
 * its live range and can corrupt register allocation. Only TMP sources qualify.
 *   T1 <- V0
 *   T2 = LOAD T1***DEREF***   -> NOT rewritten (still T1, still lval) */
UT_TEST(test_copyprop_lval_use_var_source_not_propagated)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);

  IROperand load_src = utb_temp(1, I32);
  load_src.is_lval = 1;
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), load_src, UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  /* The lval use is left untouched; the only possible change would have been
   * this propagation, so the pass must report no changes. */
  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, iload);
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(1));
  UT_ASSERT_EQ((int)s1.is_lval, 1);

  utb_free(ir);
  return 0;
}

/* GUARD (ASSIGN with lval source is a LOAD, not a copy): must NOT be recorded,
 * so a later use of the destination is NOT rewritten.
 *   T1 <- V0***DEREF***   (this is a LOAD-shaped ASSIGN)
 *   T2 = T1 ADD #1        -> NOT rewritten */
UT_TEST(test_copyprop_lval_source_assign_not_recorded)
{
  TCCIRState *ir = utb_new();

  IROperand lval_src = utb_var(0, I32);
  lval_src.is_lval = 1;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), lval_src, UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD (constant source): T1 <- #5 is not a copy; no propagation.
 *   T1 <- #5
 *   T2 = T1 ADD #1        -> NOT rewritten */
UT_TEST(test_copyprop_const_source_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (source redefined before use): the copy is invalidated when its
 * source VAR is reassigned between the copy and the use, so it must NOT
 * propagate past the redefinition.
 *   T1 <- V0
 *   V0 <- #9          (redefines the source)
 *   T2 = T1 ADD #1    -> NOT rewritten (still T1) */
UT_TEST(test_copyprop_source_redef_blocks_propagation)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(9, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (btype mismatch on the copy): T9 is a 64-bit value and T10 <- T9
 * truncates to 32-bit; that ASSIGN is NOT a copy (different register width
 * class), so a later use of T10 must NOT be rewritten.
 *   T10(INT32) <- T9(INT64)
 *   T11 = T10 ADD #1      -> NOT rewritten */
UT_TEST(test_copyprop_btype_mismatch_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(10, I32), utb_temp(9, I64), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(11, I32), utb_temp(10, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(10));

  utb_free(ir);
  return 0;
}

/* POSITIVE (STORE dest propagation): copy of an address propagates into the
 * STORE destination pointer while preserving the DEREF + store width.
 *   T1 <- T0
 *   STORE T1***DEREF*** <- V5   -> dest pointer rewritten to T0 (still lval) */
UT_TEST(test_copyprop_store_dest_tmp_source_propagates)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  /* STORE: dest = address (lval pointer), src1 = value. */
  IROperand store_addr = utb_temp(1, I32);
  store_addr.is_lval = 1;
  int istore = utb_emit(ir, TCCIR_OP_STORE, store_addr, utb_var(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand d = utb_dest(ir, istore);
  UT_ASSERT_EQ(utb_vreg(d), VR_TMP(0));
  UT_ASSERT_EQ((int)d.is_lval, 1);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_copyprop)
{
  UT_COVERS("copy_prop");
  UT_RUN(test_copyprop_var_copy_propagates_to_add);
  UT_RUN(test_copyprop_tmp_copy_propagates_both_operands);
  UT_RUN(test_copyprop_lval_use_preserves_deref_and_width);
  UT_RUN(test_copyprop_lval_use_var_source_not_propagated);
  UT_RUN(test_copyprop_lval_source_assign_not_recorded);
  UT_RUN(test_copyprop_const_source_not_recorded);
  UT_RUN(test_copyprop_source_redef_blocks_propagation);
  UT_RUN(test_copyprop_btype_mismatch_not_recorded);
  UT_RUN(test_copyprop_store_dest_tmp_source_propagates);
}
