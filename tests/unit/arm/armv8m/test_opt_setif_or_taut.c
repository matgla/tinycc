/*
 *  test_opt_setif_or_taut.c - suite for ir/opt_setif_or_taut.c
 *                             (SETIF OR-chain tautology fold)
 *
 *  tcc_ir_opt_setif_or_tautology recognizes bitwise-OR chains over CMP+SETIF
 *  booleans that all compare the *same* operands.  Each comparison token is
 *  mapped to a 3-bit cover mask over the integer-compare outcomes
 *  {LT=bit0, EQ=bit1, GT=bit2} via cond_to_mask().  An `OR Td = Ta | Tb`
 *  whose two SETIF sources were recorded for a compatible compare context
 *  combines their masks; when the union reaches 0b111 (covers LT, EQ and GT)
 *  the OR is provably always 1 and the instruction is rewritten in place to
 *  `ASSIGN Td = #1`.  The pass returns the number of such folds.
 *
 *  How the pass reads the pattern (mirrored exactly by these hand-built IRs):
 *    - SETIF (config {dest, src1}): dest is the boolean TEMP; src1 is an
 *      immediate holding the comparison token (vtop->cmp_op, e.g. TOK_LT).
 *    - The CMP that feeds a SETIF is the most-recent non-NOP instruction
 *      *immediately before* the SETIF; its src1/src2 are snapshotted as the
 *      compare context (vreg or immediate, signed vs unsigned).
 *    - Two SETIF booleans are "compatible" only within the same basic block,
 *      same signedness, and identical compare operands.
 *
 *  Isolated tests: a tiny IR sequence is run through the bare pass entry point
 *  and the resulting instructions are inspected directly (no QEMU / frontend).
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here to avoid
 * pulling in the optimizer-engine headers). */
int tcc_ir_opt_setif_or_tautology(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* ------------------------------------------------------------------ tests */

/* Minimal tautology: (a < 0) | (a >= 0).
 *
 *   i0: CMP   a, #0
 *   i1: SETIF T0, #TOK_LT     -> mask LT  = 0b001
 *   i2: CMP   a, #0
 *   i3: SETIF T1, #TOK_GE     -> mask GE  = 0b110
 *   i4: OR    T2 = T0 | T1    -> combined = 0b111  =>  ASSIGN T2 = #1
 *
 * LT and GE together cover all three compare outcomes, so the OR is always 1.
 * Positive / non-vacuous: would FAIL if the pass were a no-op. */
UT_TEST(test_setif_or_lt_ge_covers_all_folds_to_one)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  /* OR rewritten to ASSIGN T2 = #1. */
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, ior);
  UT_ASSERT(irop_is_immediate(s1));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, s1), 1);
  /* dest TEMP untouched. */
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ior)),
               TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));

  utb_free(ir);
  return 0;
}

/* The sc.c torture pattern collapsed: (a==0) | (a!=0).
 *
 *   EQ mask = 0b010, NE mask = 0b101  ->  union = 0b111  =>  fold to #1.
 *
 * A second positive proving the EQ/NE pairing (single OR) also triggers. */
UT_TEST(test_setif_or_eq_ne_covers_all_folds_to_one)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);

  utb_free(ir);
  return 0;
}

/* Non-tautological: (a < 0) | (a == 0)  ==  (a <= 0).
 *
 *   LT mask = 0b001, EQ mask = 0b010  ->  union = 0b011  != 0b111.
 *
 * The GT outcome is NOT covered, so the OR is a genuine boolean computation
 * and must be left unchanged. Negative test: returns 0, OR preserved. */
UT_TEST(test_setif_or_lt_eq_partial_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* Operand-mismatch guard: masks DO union to 0b111, but the two SETIFs compare
 * different variables, so they are not the same boolean predicate and the OR
 * is not a tautology.
 *
 *   (a < 0) | (b >= 0)   -> LT|GE = 0b111  but operands differ (param0 vs param1)
 *
 * bool_info_compatible() rejects the pair on the s1_vr mismatch.
 * Negative test: returns 0, OR preserved. */
UT_TEST(test_setif_or_different_operands_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(1, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* Missing-CMP guard: a SETIF whose immediately-preceding non-NOP instruction
 * is NOT a CMP yields no recorded boolean, so the OR has nothing to combine.
 *
 *   i0: ASSIGN T0 = #5          (filler so T0 exists; not a CMP)
 *   i1: SETIF  T1, #TOK_LT      (preceding non-NOP is ASSIGN, not CMP -> drop)
 *   i2: CMP    a, #0
 *   i3: SETIF  T2, #TOK_GE      (properly tracked)
 *   i4: OR     T3 = T1 | T2     (T1 not tracked -> incompatible -> no fold)
 *
 * Even though the cond tokens would union to 0b111, the un-tracked SETIF
 * source blocks the fold. Negative test: returns 0, OR preserved. */
UT_TEST(test_setif_or_setif_without_cmp_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32),
                     utb_temp(1, I32), utb_temp(2, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_setif_or_taut)
{
  UT_COVERS("setif_or_tautology");
  UT_RUN(test_setif_or_lt_ge_covers_all_folds_to_one);
  UT_RUN(test_setif_or_eq_ne_covers_all_folds_to_one);
  UT_RUN(test_setif_or_lt_eq_partial_no_fold);
  UT_RUN(test_setif_or_different_operands_no_fold);
  UT_RUN(test_setif_or_setif_without_cmp_no_fold);
}
