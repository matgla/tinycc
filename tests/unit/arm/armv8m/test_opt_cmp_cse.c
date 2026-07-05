/*
 *  test_opt_cmp_cse.c - suite for ir/opt.c :: tcc_ir_opt_cmp_setif_cse
 *
 *  CSEs adjacent-equivalent CMP+SETIF pairs across non-clobbering ops:
 *
 *      CMP A, B            CMP A, B   (structurally equal)
 *      V1 <-- (cond=C)  => NOP
 *      ...safe...          V2 <-- V1  [ASSIGN]
 *      CMP A, B
 *      V2 <-- (cond=C)
 *
 *  Scoped to one basic block (any is_jump_target / terminator / STORE / CALL
 *  between the pairs breaks the forward scan) and gated on: the first SETIF
 *  result being single-def, no intervening redefinition of that result or of a
 *  CMP operand vreg, matching condition codes, matching operand btymes, and
 *  structural operand equality.  Each gate has a dedicated test.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_cmp_setif_cse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I16 IROP_BTYPE_INT16

#define UT_TOK_NE 0x95
#define UT_TOK_EQ 0x94

#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

/* -------------------------------------------------- positive paths */

UT_TEST(test_cse_two_identical_imm_pairs_fold)
{
  /* Two CMP #5,#7 ; SETIF NE pairs with a benign intervening ASSIGN.  The
   * second CMP is NOPed and its SETIF becomes ASSIGN of the first result. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));       /* 0 */
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);   /* 2: benign */
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32)); /* 3 */
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_setif_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, setif2)), VR_TEMP(1));
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_two_identical_vreg_pairs_fold)
{
  /* Same pattern but CMP operands are vregs tracing to the same constant def.
   * Exercises the pure_expr_equal vreg-def equivalence path (both resolve to
   * ASSIGN #5 at idx 0). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);   /* 0: T0 = #5 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32));      /* 1 */
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);   /* 3: benign */
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32)); /* 4 */
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_setif_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_ASSIGN);
  utb_free(ir);
  return 0;
}

/* -------------------------------------------------- guard branches */

UT_TEST(test_cse_too_few_instructions)
{
  /* n < 4 -> immediate 0 (need >= CMP+SETIF+CMP+SETIF). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_intervening_clobber_blocks)
{
  /* A STORE between the pairs is a hard clobber -> forward scan breaks. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_temp(8, I32), utb_imm(0, I32), UTB_NONE); /* clobber */
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_intervening_redef_of_result_blocks)
{
  /* Redefining the first SETIF result (T1) between the pairs invalidates the
   * value the second pair would copy -> break. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32), UTB_NONE); /* redef T1 */
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_intervening_redef_of_operand_blocks)
{
  /* Redefining a CMP operand vreg between the pairs could change the second
   * comparison's inputs -> break (even though the vreg name matches). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);   /* T0 = #5 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE); /* redef T0 */
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_bb_boundary_blocks)
{
  /* is_jump_target on an intervening instr marks a basic-block boundary -> the
   * forward scan must not cross it. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);
  ir->compact_instructions[mid].is_jump_target = 1;
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_cond_mismatch_no_fold)
{
  /* First pair NE, second pair EQ -> conditions differ -> no fold. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_EQ, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_btype_mismatch_no_fold)
{
  /* Operand btype differs between the two CMPs (I32 vs I16) -> no fold. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I16), utb_imm(7, I16));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_structural_inequality_no_fold)
{
  /* Identical conditions/btypes but different compared values -> operands are
   * not structurally equal -> no fold. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(9, I32)); /* 7 vs 9 */
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_setif_dest_lval_skipped)
{
  /* A SETIF whose dest is an lvalue is not a normal result -> the outer loop
   * `setif1_dest.is_lval` guard skips this anchor. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_lval(utb_temp(1, I32)), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32), UTB_NONE);
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp2), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_cse_empty_ir_no_crash)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_cmp_setif_cse(ir), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_cmp_cse)
{
  UT_COVERS("cmp_setif_cse");
  UT_RUN(test_cse_two_identical_imm_pairs_fold);
  UT_RUN(test_cse_two_identical_vreg_pairs_fold);
  UT_RUN(test_cse_too_few_instructions);
  UT_RUN(test_cse_intervening_clobber_blocks);
  UT_RUN(test_cse_intervening_redef_of_result_blocks);
  UT_RUN(test_cse_intervening_redef_of_operand_blocks);
  UT_RUN(test_cse_bb_boundary_blocks);
  UT_RUN(test_cse_cond_mismatch_no_fold);
  UT_RUN(test_cse_btype_mismatch_no_fold);
  UT_RUN(test_cse_structural_inequality_no_fold);
  UT_RUN(test_cse_setif_dest_lval_skipped);
  UT_RUN(test_cse_empty_ir_no_crash);
}
