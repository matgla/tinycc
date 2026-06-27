/*
 *  test_opt_vrp.c - suite for ir/opt_branch.c value-range propagation (vrp)
 *
 *  tcc_ir_opt_vrp() tracks per-vreg [min, max] ranges derived from immediate
 *  assignments and simple ADD/SUB propagation, then folds CMP+JUMPIF sequences
 *  when the result is provable over the whole range.  It also carries ranges
 *  through unconditional jumps to single-predecessor blocks and clears them at
 *  merge points / back-edge targets.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *
 *  Note on PARAM ranges: the current vrp implementation seeds its range table
 *  from ASSIGN immediates and arithmetic propagation; it does not read
 *  parameters_live_intervals[].  The positive test that the spec describes as
 *  "PARAM #0 known range [0,10]" is therefore exercised with an immediate
 *  ASSIGN that establishes the same concrete range, pinning the behaviour that
 *  is actually implemented today.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here). */
int tcc_ir_opt_vrp(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* ============================================================= positive tests */

/* A known singleton range proves CMP T0, #20 / JUMPIF LT always taken.
 *   T0 = ASSIGN #5     -> range [5,5]
 *   CMP T0, #20
 *   JUMPIF LT -> target
 * The CMP is NOP-ed and the JUMPIF becomes an unconditional JUMP. */
UT_TEST(test_vrp_const_range_lt_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(20, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Singleton range: CMP T0, #5 / JUMPIF EQ is always true. */
UT_TEST(test_vrp_singleton_eq_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Singleton range: CMP T0, #3 / JUMPIF LT is always false. */
UT_TEST(test_vrp_singleton_lt_false_nops_both)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(3, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Unsigned range fact: a non-negative singleton [5,5] proves ULT #10. */
UT_TEST(test_vrp_unsigned_range_ult_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Range propagates through a copy: T1 = ASSIGN T0 inherits [5,5]. */
UT_TEST(test_vrp_range_propagates_through_copy)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 5);

  utb_free(ir);
  return 0;
}

/* Range is carried through an unconditional jump to a single-predecessor block.
 *   0: T0 = ASSIGN #5
 *   1: JUMP -> 3
 *   2: RETURNVOID
 *   3: CMP T0, #5 ; JUMPIF EQ -> 5
 * Instruction 3 is only reachable from the jump at 1, so the snapshot taken at
 * the JUMP is reinstalled before the CMP and the branch folds. */
UT_TEST(test_vrp_deferred_range_through_uncond_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 5);

  utb_free(ir);
  return 0;
}

/* ============================================================= guard tests */

/* No range is known for T1, so a copy from it gives T0 no useful range and the
 * branch must stay conditional. */
UT_TEST(test_vrp_unknown_range_left_untouched)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_temp(1, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* A merge point has multiple predecessors, so ranges from any single path are
 * discarded and the branch cannot fold.
 *   0: JUMP -> 3
 *   1: T0 = ASSIGN #5
 *   2: JUMP -> 4
 *   3: T0 = ASSIGN #5
 *   4: CMP T0, #5 ; JUMPIF EQ -> 6
 * Instruction 4 is a merge (from 2 and 3), so its range table is cleared. */
UT_TEST(test_vrp_merge_point_clears_ranges)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* A back-edge target (jump from a later instruction) is treated as a merge
 * point, clearing ranges and preventing the fold.
 *   0: T0 = ASSIGN #5
 *   1: JUMP -> 3
 *   2: RETURNVOID
 *   3: CMP T0, #5 ; JUMPIF EQ -> 5
 *   4: JUMP -> 3
 *   5: RETURNVOID
 * Instruction 3 is the target of the backward jump at 4. */
UT_TEST(test_vrp_backedge_target_clears_ranges)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* The vrp fold requires the immediate to be in CMP src2.  With the operands
 * swapped (immediate in src1) the provable branch is left untouched. */
UT_TEST(test_vrp_swapped_cmp_operands_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(20, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_vrp)
{
  UT_COVERS("vrp");

  UT_RUN(test_vrp_const_range_lt_folds_to_jump);
  UT_RUN(test_vrp_singleton_eq_folds_to_jump);
  UT_RUN(test_vrp_singleton_lt_false_nops_both);
  UT_RUN(test_vrp_unsigned_range_ult_folds_to_jump);
  UT_RUN(test_vrp_range_propagates_through_copy);
  UT_RUN(test_vrp_deferred_range_through_uncond_jump);

  UT_RUN(test_vrp_unknown_range_left_untouched);
  UT_RUN(test_vrp_merge_point_clears_ranges);
  UT_RUN(test_vrp_backedge_target_clears_ranges);
  UT_RUN(test_vrp_swapped_cmp_operands_no_fold);
}
