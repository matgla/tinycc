/*
 *  test_opt_vrp.c - suite for ir/opt_branch.c value-range propagation (vrp)
 *
 *  tcc_ir_opt_vrp() tracks per-vreg [min, max] ranges derived from immediate
 *  assignments, ADD/SUB propagation, bitwise-AND masks, and shifts (SHL/SAR/SHR),
 *  then folds CMP+JUMPIF sequences when the result is provable over the whole
 *  range.  It carries ranges through unconditional jumps and EQ-branch taken
 *  edges into single-predecessor blocks, and clears them at merge points /
 *  back-edge targets.
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

/* ============================================================= arithmetic propagation */

/* T0 = ASSIGN #5 gives range [5,5]; T1 = T0 + #3 propagates to [8,8], proving
 * CMP T1,#8 / JUMPIF EQ is always taken. Exercises the ADD range-propagation
 * branch (irop.op == TCCIR_OP_ADD with an immediate src2). */
UT_TEST(test_vrp_add_propagates_range_folds_eq)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(8, I32));
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

/* T0 = ASSIGN #10 gives range [10,10]; T1 = T0 - #3 propagates to [7,7],
 * proving CMP T1,#7 / JUMPIF NE is always false. Exercises the SUB
 * range-propagation branch. */
UT_TEST(test_vrp_sub_propagates_range_folds_ne_false)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(7, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* T1 = T0 + #3 where T0 has no tracked range: the dest range must be
 * invalidated (not silently left stale/valid from a previous slot reuse),
 * so a later CMP relying on it cannot fold. */
UT_TEST(test_vrp_add_unknown_src_invalidates_dest_range)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(8, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ============================================================= zero tautology fold */

/* CMP T0,#0 / JUMPIF UGE is a tautology (unsigned >= 0 always holds)
 * regardless of T0's value -- no range information is required.  Exercises
 * the cmp_val==0 fast path independent of the range table. */
UT_TEST(test_vrp_uge_zero_tautology_always_taken)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_UGE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 3);

  utb_free(ir);
  return 0;
}

/* CMP T0,#0 / JUMPIF ULT is a contradiction (nothing is unsigned < 0) --
 * both instructions NOP regardless of T0's value. */
UT_TEST(test_vrp_ult_zero_tautology_never_taken)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ============================================================= fall-through constraint */

/* CMP T0,#5 / JUMPIF LT sets a fall-through constraint T0 in [5, INT32_MAX]
 * (branch-not-taken means NOT(T0<5)).  The very next instruction is a second
 * CMP T0,#5 / JUMPIF GE, which the constraint proves always taken.  Exercises
 * the `pending_apply_at`/`pending_slot` scheduling machinery. */
UT_TEST(test_vrp_fallthrough_constraint_folds_next_cmp)
{
  TCCIRState *ir = utb_new();

  int icmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  /* First CMP/JUMPIF pair is left as a genuine conditional branch: T0's
   * value is unknown at instruction 0, so it cannot fold. */
  UT_ASSERT_EQ(utb_op(ir, icmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  /* Second pair folds using the fall-through constraint from the first. */
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp2).u.imm32, 6);

  utb_free(ir);
  return 0;
}

/* ============================================================= register-register chain */

/* CMP T0,T1 / JUMPIF GE (fall-through implies T0<T1); a second identical
 * CMP T0,T1 / JUMPIF LE is implied by "T0<T1" (LT implies LE), so it always
 * taken -> unconditional JUMP.  Exercises the reg-reg comparison constraint
 * propagation block (cmp_vr1/cmp_vr2 both vregs). */
UT_TEST(test_vrp_regreg_chain_lt_implies_le_folds_jump)
{
  TCCIRState *ir = utb_new();

  int icmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp2).u.imm32, 6);

  utb_free(ir);
  return 0;
}

/* CMP T0,T1 / JUMPIF LE (fall-through implies T0>T1); a second identical
 * CMP T0,T1 / JUMPIF LT is impossible given "T0>T1" (GT implies NOT GE, and
 * LT's negation GE is implied by GT), so it is never taken -> both NOP. */
UT_TEST(test_vrp_regreg_chain_gt_implies_not_lt_nops_both)
{
  TCCIRState *ir = utb_new();

  int icmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Reg-reg chain guard: a merge point between the two CMPs blocks the
 * propagation (`!(is_merge[(i+2)/8] & ...)` check) even though the operands
 * and tokens would otherwise fold. */
UT_TEST(test_vrp_regreg_chain_merge_point_blocks_fold)
{
  TCCIRState *ir = utb_new();

  int icmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  /* Extra predecessor of instruction 2 (icmp2) makes it a merge point. */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* #7, fallthrough of #6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* #8: JUMPIF target, in-bounds */

  int changes = tcc_ir_opt_vrp(ir);
  (void)changes;

  UT_ASSERT_EQ(utb_op(ir, icmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ============================================================= CMP + SETIF */

/* T0 = ASSIGN #5 gives a singleton range [5,5]; CMP T0,#5 / SETIF EQ is
 * therefore always true.  The pass NOPs the CMP and rewrites the SETIF into
 * an ASSIGN of the constant fold result (1). Exercises the CMP+SETIF range
 * fold block. */
UT_TEST(test_vrp_cmp_setif_range_folds_to_const_one)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int isetif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, isetif), TCCIR_OP_ASSIGN);
  IROperand new_src1 = utb_src1(ir, isetif);
  UT_ASSERT(irop_is_immediate(new_src1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_src1), 1);
  /* The SETIF's original dest (T1) must be preserved on the rewritten ASSIGN. */
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, isetif)), 1);

  utb_free(ir);
  return 0;
}

/* Same shape but the range disproves the condition: T0 = ASSIGN #5, CMP
 * T0,#9 / SETIF EQ folds to constant 0. */
UT_TEST(test_vrp_cmp_setif_range_folds_to_const_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(9, I32));
  int isetif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, isetif), TCCIR_OP_ASSIGN);
  IROperand new_src1 = utb_src1(ir, isetif);
  UT_ASSERT(irop_is_immediate(new_src1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_src1), 0);

  utb_free(ir);
  return 0;
}

/* ============================================================= EQ/NE outside a wide range */

/* CMP T0,#10 / JUMPIF GT sets fall-through constraint T0 in [INT32_MIN,10].
 * A subsequent CMP T0,#20 / JUMPIF EQ is provably false (20 is outside the
 * range) even though the range is not a singleton -- exercises the
 * `cmp_val < rmin || cmp_val > rmax` branch of the EQ/NE fold (distinct from
 * the rmin==rmax singleton path already covered above). */
UT_TEST(test_vrp_wide_range_eq_outside_bounds_nops_both)
{
  TCCIRState *ir = utb_new();

  int icmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));
  int ijmp1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(20, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp1), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Same wide range [INT32_MIN,10], but this time the second branch is NE
 * against the out-of-range value 20 -- always true, folds to JUMP. */
UT_TEST(test_vrp_wide_range_ne_outside_bounds_folds_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  int icmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(20, I32));
  int ijmp2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp2).u.imm32, 6);

  utb_free(ir);
  return 0;
}

/* ============================================================= negative-range unsigned */

/* A singleton negative range [-100,-100] (both endpoints negative as int32)
 * still lets the unsigned-comparison fold apply: -100 as uint32 is close to
 * UINT32_MAX, so ULT #-1 (i.e. unsigned(-100) < unsigned(-1)) is true.
 * Exercises the `rmin < 0 && rmax < 0` branch of the unsigned range fold. */
UT_TEST(test_vrp_negative_range_unsigned_ult_folds_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-100, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(-1, I32));
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

/* ========================================================= range-source levers */

/* EQ taken edge: `CMP T0,#7 ; JUMPIF EQ -> target` carries T0==7 into the target block when that
 * target is a sole-predecessor forward block, where a second `CMP T0,#7 ; JUMPIF EQ` folds. */
UT_TEST(test_vrp_eq_taken_edge_carries_singleton_to_target)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32));            /* 0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);     /* 1: ==,->3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 2: != path */
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(7, I32)); /* 3: target */
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 6 */

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(ir, ijmp).u.imm32, 6);

  utb_free(ir);
  return 0;
}

/* AND with a non-negative mask bounds the result to [0, mask]: T1 = T0 & (1<<26) is >= 0, so the
 * signed `T1 >= 0` (JUMPIF GE) is always true and folds — the gcc.c-torture bit.c `foo` shape. */
UT_TEST(test_vrp_and_nonneg_mask_folds_ge_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1 << 26, I32)); /* 0 */
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(0, I32));     /* 1 */
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);  /* 4 */

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* A mask that includes the sign bit (negative as int32) yields no usable range: compare untouched. */
UT_TEST(test_vrp_and_negative_mask_no_range)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(-16, I32));   /* mask<0 */
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(1000, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* SHL by a constant scales a known range: T0=3, T1 = T0 << 2 = 12 → CMP T1,#12 / JUMPIF EQ folds. */
UT_TEST(test_vrp_shl_scales_range_folds_eq)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(12, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* SAR (arithmetic right shift) narrows a known range: T0=20, T1 = T0 >> 2 = 5. */
UT_TEST(test_vrp_sar_narrows_range_folds_eq)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(20, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SAR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* SHR (logical right shift) narrows a non-negative range: T0=40, T1 = T0 >>u 3 = 5. */
UT_TEST(test_vrp_shr_nonneg_narrows_range_folds_eq)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(40, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* SHR on a possibly-negative source is not monotone as a signed interval → no range recorded. */
UT_TEST(test_vrp_shr_negative_source_no_range)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(1000, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_vrp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* Regression lock for docs/bugs.md #6: VRP seeds ranges from `T = ASSIGN #imm`. */
UT_COVERS("vrp");
