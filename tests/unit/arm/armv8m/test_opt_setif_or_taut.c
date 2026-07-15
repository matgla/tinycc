/*
 *  test_opt_setif_or_taut.c - suite for source/opt/flat/scalar/setif_or_taut.c
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

/* ---- corner-case tests (plan_corner_case_tests.md §C) -------------------- */

/* Semi-oracle: a tautological OR always folds to the constant 1, regardless of
 * how the pass implements the rewrite.  expected is computed independently. */
UT_TEST(test_setif_or_tautology_fold_value_is_independently_one)
{
  TCCIRState *ir = utb_new();
  int expected = 1; /* LT|GE covers {LT,EQ,GT} */

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), expected);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Unsigned condition codes: TOK_ULT (mask 0b001) | TOK_UGE (mask 0b110) = 0b111. */
UT_TEST(test_setif_or_unsigned_ult_uge_covers_all_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_UGE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Completes unsigned coverage: TOK_ULE (0b011) | TOK_UGT (0b100) = 0b111. */
UT_TEST(test_setif_or_unsigned_ule_ugt_covers_all_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_ULE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_UGT, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Another signed pairing: TOK_LE (0b011) | TOK_GT (0b100) = 0b111. */
UT_TEST(test_setif_or_signed_le_gt_covers_all_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Partial unsigned union: TOK_ULT (0b001) | TOK_UGT (0b100) = 0b101, EQ missing. */
UT_TEST(test_setif_or_unsigned_ult_ugt_partial_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_UGT, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* "Contradiction" shape: LT|GT covers only strict outcomes, EQ is missing.
 * The OR is not always true, so it must not fold to #1. */
UT_TEST(test_setif_or_lt_gt_partial_missing_eq_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Signedness must match even when masks would otherwise cover all outcomes. */
UT_TEST(test_setif_or_signed_unsigned_same_operands_do_not_mix)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* signed */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_UGE, I32), UTB_NONE); /* unsigned */
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Different immediate on the RHS of the CMP breaks operand compatibility. */
UT_TEST(test_setif_or_different_immediate_operands_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* A JUMP target creates a basic-block boundary; tracker state resets there,
 * so SETIFs on opposite sides cannot be merged. */
UT_TEST(test_setif_or_basic_block_boundary_resets_tracker)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  (void)jump;
  utb_free(ir);
  return 0;
}

/* Rewriting a CMP operand between the two SETIFs invalidates the earlier tracker
 * entry because the compare context has changed. */
UT_TEST(test_setif_or_operand_rewrite_invalidates_tracker)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_param(0, I32), utb_param(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Three-way OR chain: the first OR accumulates a partial mask, the second OR
 * completes the cover and folds.  Verifies mask inheritance across ORs. */
UT_TEST(test_setif_or_three_way_chain_inherits_mask)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  int ior1 = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32),
                      utb_temp(0, I32), utb_temp(1, I32));
  int ior2 = utb_emit(ir, TCCIR_OP_OR, utb_temp(4, I32),
                      utb_temp(3, I32), utb_temp(2, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior1), TCCIR_OP_OR); /* partial LT|EQ, not folded */
  UT_ASSERT_EQ(utb_op(ir, ior2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior2)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 5), 0);

  utb_free(ir);
  return 0;
}

/* The fold preserves the source btype of the OR; here an INT64 compare folds
 * to ASSIGN #1 with an INT64 immediate. */
UT_TEST(test_setif_or_int64_tautology_folds_with_int64_immediate)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, IROP_BTYPE_INT64), utb_imm(0, IROP_BTYPE_INT64));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, IROP_BTYPE_INT64), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, IROP_BTYPE_INT64), utb_imm(0, IROP_BTYPE_INT64));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, IROP_BTYPE_INT64), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, IROP_BTYPE_INT64),
                     utb_temp(0, IROP_BTYPE_INT64), utb_temp(1, IROP_BTYPE_INT64));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_btype(utb_src1(ir, ior)), IROP_BTYPE_INT64);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Floating-point compares are not integer compares; the SETIFs must not be
 * tracked, so no tautology fold happens even with matching tokens. */
UT_TEST(test_setif_or_float_cmp_not_tracked)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, IROP_BTYPE_FLOAT32), utb_imm(0, IROP_BTYPE_FLOAT32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, IROP_BTYPE_FLOAT32), utb_imm(0, IROP_BTYPE_FLOAT32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* SETIF with a token that has no cover mask (here 0) is not tracked. */
UT_TEST(test_setif_or_unrecognized_condition_token_not_tracked)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* An OR whose destination is an lvalue must not be folded. */
UT_TEST(test_setif_or_or_dest_lval_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_lval(utb_temp(2, I32)),
                     utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_setif_or_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

/* Idempotence: after the first fold, a second run finds nothing. */
UT_TEST(test_setif_or_pass_is_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32),
                     utb_temp(0, I32), utb_temp(1, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_setif_or_tautology, 5);

  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, ior)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 3), 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("setif_or_tautology");
