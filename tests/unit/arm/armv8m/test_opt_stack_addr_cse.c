/*
 *  test_opt_stack_addr_cse.c - suite for ir/opt.c :: tcc_ir_opt_stack_addr_cse
 *
 *  The pass has two phases:
 *    (1) Collect ASSIGN+ADD(StackOff) self-add pairs and FOLD each one into a
 *        single ASSIGN with combined offset, NOPing the ADD.  This always runs
 *        for every recognised pair, independent of duplication.
 *    (2) When >=2 pairs share the same (offset, constant) and the first result
 *        vreg is not redefined between them, CSE the duplicate pair: NOP it and
 *        rewrite every source use of the duplicate vreg to the first.
 *
 *  Corner cases pinned: bare ASSIGN (no ADD) ignored, non-self-add ignored,
 *  the per-pair fold, CSE dedup with use rewrite, mismatched offset/constant
 *  blocking CSE, redefinition between duplicates blocking CSE, and the
 *  STACK_CSE_MAX_ENTRIES (32) collection cap that leaves the 33rd pair unfolded.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_stack_addr_cse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

/* ASSIGN T<n> = StackOff[off] ; ADD T<n> = T<n> + #c.  Returns the ASSIGN idx. */
static int emit_pair(TCCIRState *ir, int n, int32_t off, int32_t c)
{
  int assign_idx = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(n, I32),
                            utb_stackoff(off, 0, 0, 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(n, I32), utb_temp(n, I32), utb_imm(c, I32));
  return assign_idx;
}

/* -------------------------------------------------- phase-1 fold */

UT_TEST(test_single_pair_folded_add_noped)
{
  /* One ASSIGN+ADD pair -> combined offset, ADD NOPed.  seq_count < 2 so no
   * CSE; returns exactly 1 change. */
  TCCIRState *ir = utb_new();
  int a = emit_pair(ir, 0, 16, 4);

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, a), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(ir, a).u.imm32, 20); /* 16 + 4 */
  UT_ASSERT_EQ(utb_op(ir, a + 1), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bare_assign_no_add_ignored)
{
  /* A bare ASSIGN of a stack address (no following self-add) is deliberately
   * not tracked: add_idx < 0 -> continue.  No change, IR untouched. */
  TCCIRState *ir = utb_new();
  int a = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                   utb_stackoff(16, 0, 0, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(ir, a).u.imm32, 16);
  utb_free(ir);
  return 0;
}

UT_TEST(test_non_self_add_not_paired)
{
  /* ADD dest != src1 (T1 = T0 + #4) is not a self-add -> nd_vr != vreg, so the
   * pair is not recognised.  Nothing folds. */
  TCCIRState *ir = utb_new();
  int a = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                   utb_stackoff(16, 0, 0, 0, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  utb_free(ir);
  return 0;
}

UT_TEST(test_lval_stackoff_src_rejected)
{
  /* src1.is_lval means the stack address is being *dereferenced* (a load), not
   * the address value itself -> rejected by the `|| src1.is_lval` guard. */
  TCCIRState *ir = utb_new();
  int a = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                   utb_stackoff(16, 1, 0, 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a), TCCIR_OP_ASSIGN);
  utb_free(ir);
  return 0;
}

UT_TEST(test_empty_ir_no_crash)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_stack_addr_cse(ir), 0);
  utb_free(ir);
  return 0;
}

/* -------------------------------------------------- phase-2 CSE */

UT_TEST(test_two_identical_pairs_cse_rewrites_use)
{
  /* Two identical pairs (off=16, +4).  Phase-1 folds both (2 changes), then
   * phase-2 CSEs the second: NOPs its ASSIGN and rewrites the reader of T1 to
   * use T0 instead. */
  TCCIRState *ir = utb_new();
  emit_pair(ir, 0, 16, 4); /* T0 */
  emit_pair(ir, 1, 16, 4); /* T1 (duplicate) */
  int reader = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 3); /* 2 folds + 1 CSE */
  /* Duplicate pair (instr 2 and 3) NOPed. */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_NOP);
  /* Reader now sources T0 instead of T1. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, reader)), VR_TEMP(0));
  utb_free(ir);
  return 0;
}

UT_TEST(test_mismatched_constant_no_cse)
{
  /* Same offset, different add-constant -> different combined offset -> no CSE.
   * Both pairs still fold (2 changes). */
  TCCIRState *ir = utb_new();
  int a0 = emit_pair(ir, 0, 16, 4); /* -> 20 */
  int a1 = emit_pair(ir, 1, 16, 8); /* -> 24 */

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_src1(ir, a0).u.imm32, 20);
  UT_ASSERT_EQ(utb_src1(ir, a1).u.imm32, 24);
  /* Both ASSIGNs survive (no CSE). */
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ASSIGN);
  utb_free(ir);
  return 0;
}

UT_TEST(test_mismatched_offset_no_cse)
{
  /* Different offset, same constant -> no CSE.  Both fold. */
  TCCIRState *ir = utb_new();
  int a0 = emit_pair(ir, 0, 16, 4); /* -> 20 */
  int a1 = emit_pair(ir, 1, 24, 4); /* -> 28 */

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_src1(ir, a0).u.imm32, 20);
  UT_ASSERT_EQ(utb_src1(ir, a1).u.imm32, 28);
  utb_free(ir);
  return 0;
}

UT_TEST(test_redefinition_between_duplicates_blocks_cse)
{
  /* An intervening redefinition of the first result vreg (T0) between the two
   * duplicate pairs breaks the dataflow assumption -> phase-2 bails for that
   * pair.  Phase-1 still folds both pairs (2 changes), no CSE. */
  TCCIRState *ir = utb_new();
  emit_pair(ir, 0, 16, 4);                                                        /* 0,1: T0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);  /* 2: redef T0 */
  emit_pair(ir, 1, 16, 4);                                                        /* 3,4: T1 */

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 2);
  /* The duplicate ASSIGN at instr 3 is NOT CSE'd. */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_ASSIGN);
  utb_free(ir);
  return 0;
}

UT_TEST(test_max_entries_cap_leaves_33rd_unfolded)
{
  /* seq_count is capped at STACK_CSE_MAX_ENTRIES (32).  With 33 distinct pairs
   * (unique offsets so no CSE), exactly 32 fold and the 33rd pair is left
   * untouched: its ADD is still ADD, not NOP. */
  TCCIRState *ir = utb_new();
  for (int i = 0; i < 33; ++i)
    emit_pair(ir, i, 16 + 8 * i, 4);

  int changes = tcc_ir_opt_stack_addr_cse(ir);

  UT_ASSERT_EQ(changes, 32);
  /* Pair k occupies instr [2k, 2k+1].  Pair 32 (the 33rd) is at [64,65]. */
  UT_ASSERT_EQ(utb_op(ir, 64), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 65), TCCIR_OP_ADD); /* NOT NOPed: over the cap */
  /* Pair 0's ADD WAS NOPed. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_stack_addr_cse)
{
  UT_COVERS("stack_addr_cse");
  UT_RUN(test_single_pair_folded_add_noped);
  UT_RUN(test_bare_assign_no_add_ignored);
  UT_RUN(test_non_self_add_not_paired);
  UT_RUN(test_lval_stackoff_src_rejected);
  UT_RUN(test_empty_ir_no_crash);
  UT_RUN(test_two_identical_pairs_cse_rewrites_use);
  UT_RUN(test_mismatched_constant_no_cse);
  UT_RUN(test_mismatched_offset_no_cse);
  UT_RUN(test_redefinition_between_duplicates_blocks_cse);
  UT_RUN(test_max_entries_cap_leaves_33rd_unfolded);
}
