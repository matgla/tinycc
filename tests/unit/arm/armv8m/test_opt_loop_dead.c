/*
 *  test_opt_loop_dead.c - suite for ssa_opt_first_iter_exit
 *  (ir/opt/ssa_opt_loop.c, first-iteration-exit loop elimination)
 *
 *  ssa_opt_first_iter_exit eliminates a top-tested natural loop whose header
 *  exit-test (TEST_ZERO / CMP + JUMPIF) is statically true on entry.  Loop
 *  candidates are dominance-verified back-edges on a throwaway CFG; the
 *  header's predecessors must be exactly {function-entry block, latch} and no
 *  other member block may be entered from outside the loop.  The proof walks
 *  linearly from function entry through the header to the exit JUMPIF,
 *  tracking VAR/TEMP constants and LEA(&VAR) addresses (bailing on any
 *  intervening branch); if the branch provably exits on iteration 1 the
 *  conditional JUMPIF is rewritten into an unconditional JUMP to the exit
 *  target and the loop's MEMBER blocks are NOPed (non-member code interleaved
 *  in the flat index range survives — unlike the retired legacy pass
 *  tcc_ir_opt_loop_dead_first_iter, whose flat-range NOPing this suite used
 *  to cover).
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *
 *  IR shape that triggers the pass (a natural loop is a backward JUMP whose
 *  target < its own index; the loop range is [target, back-edge]):
 *
 *    0: ASSIGN V0 = 0      (entry block: non-jump instrs before the header)
 *    1: TEST_ZERO V0       (header / back-edge target)
 *    2: JUMPIF #N, EQ      (exit branch -> target N, outside the loop)
 *    3: <body, NOP-able>
 *    4: JUMP 1             (back-edge / latch)
 *    5: <live sentinel>    (keeps the redirect JUMP from being a fallthrough)
 *    6: RETURNVOID         (exit target = N)
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt/ssa_opt.h; forward-declared here to
 * avoid pulling in the SSA optimizer engine headers). */
int ssa_opt_first_iter_exit(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* JUMPIF condition tokens (match evaluate_compare_condition / the pass). */
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_ULT 0x92
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

/* ------------------------------------------------------------------ tests */

/* Headline case: V0 == 0 on entry, header tests `V0 == 0` and exits.  The loop
 * provably exits on iteration 1, so the JUMPIF is rewritten to an unconditional
 * JUMP to the exit target and the loop body/header is NOPed. */
UT_TEST(test_loop_dead_test_zero_eq_fires)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 entry */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 header */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 exit */
  int b = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE); /* 3 body */
  int e = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE); /* 4 back-edge */
  int s = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE); /* 5 live sentinel */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 6 exit target */

  int changes = ssa_opt_first_iter_exit(ir);

  /* Independently: V0=0, test `==0` -> true -> exit branch taken on iter 1. */
  UT_ASSERT_EQ(changes, 1);
  /* JUMPIF rewritten to an unconditional JUMP, still to the exit target 6. */
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, j)), 6);
  /* Header test and entire loop body NOPed. */
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, b), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, e), TCCIR_OP_NOP);
  /* Non-loop code is untouched. */
  UT_ASSERT_EQ(utb_op(ir, s), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* V0 = 5 (nonzero); header tests `V0 != 0` and exits -> 5 != 0 is true, so the
 * loop exits on iteration 1 and is eliminated. */
UT_TEST(test_loop_dead_test_zero_ne_fires)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE);     /* 1 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);          /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE); /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);           /* 6 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, j)), 6);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* V0 = 5; header tests `V0 == 0` and exits.  5 == 0 is false, so the exit
 * branch is NOT taken on entry -> the loop may run, the pass must not fire. */
UT_TEST(test_loop_dead_test_zero_eq_not_taken_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE); /* 0 */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 */
  int b = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE); /* 3 */
  int e = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE); /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 6 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, b), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, e), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* CMP V0(3) < V1(10) with a signed LT exit token: 3 < 10 is true -> the loop
 * provably exits on iteration 1 and is eliminated.  Oracle value computed
 * independently. */
UT_TEST(test_loop_dead_cmp_lt_fires)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(3, I32), UTB_NONE);  /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(10, I32), UTB_NONE); /* 1 entry */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_var(1, I32));     /* 2 header */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(9, I32), UTB_NONE);  /* 4 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(3, I32), utb_imm(7, I32), UTB_NONE);  /* 6 sentinel */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 exit target */

  /* Independent oracle: 3 < 10 (signed) == true. */
  UT_ASSERT_EQ((3 < 10), 1);

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, j)), 7);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* CMP V0(10) < V1(3) with signed LT exit token: 10 < 3 is false -> the exit
 * branch is not taken on entry, the pass must not fire. */
UT_TEST(test_loop_dead_cmp_lt_false_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(10, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(3, I32), UTB_NONE);  /* 1 */
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_var(1, I32)); /* 2 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(9, I32), UTB_NONE);  /* 4 */
  int e = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);   /* 5 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(3, I32), utb_imm(7, I32), UTB_NONE);  /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 */

  UT_ASSERT_EQ((10 < 3), 0);

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, e), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Unsigned comparator: ULT of (unsigned)(-1)=0xFFFFFFFF vs 1 is false, even
 * though signed -1 < 1 is true.  The pass reuses evaluate_compare_condition,
 * which treats the operands as unsigned for ULT.  With a false outcome the
 * pass must not fire. */
UT_TEST(test_loop_dead_cmp_ult_unsigned_semantics_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(-1, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);  /* 1 */
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_var(1, I32)); /* 2 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_ULT, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(9, I32), UTB_NONE);  /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 5 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(3, I32), utb_imm(7, I32), UTB_NONE);  /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 */

  /* Independent oracle: the pass sign-extends the int32 -1 to int64 -1, so the
   * ULT compares (uint64)-1 vs 1 == false.  Either way (-1 as 0xFFFFFFFF or as
   * 0xFFFF...FF) unsigned-< 1 is false. */
  UT_ASSERT_EQ(((uint64_t)(int64_t)-1 < (uint64_t)1), 0);

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Headline torture pattern `for (p = &s; *p; ...)` with s == 0: LEA T0 = &V0,
 * then `*T0` (a deref of the address) tests V0's value == 0 -> exits iter 1. */
UT_TEST(test_loop_dead_lea_deref_fires)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 V0=0 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);    /* 1 T0=&V0 (entry) */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_lval(utb_temp(0, I32)), UTB_NONE); /* 2 *T0 (header) */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 4 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 6 sentinel */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 exit */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, j)), 7);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* STORE through the pointer updates the pointed-to VAR: LEA T0=&V0, *T0 = 0,
 * then `*T0` tests V0 == 0 -> the stored constant flows through and the loop
 * is eliminated. */
UT_TEST(test_loop_dead_store_through_ptr_fires)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);    /* 0 T0=&V0 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE); /* 1 *T0=0 (entry) */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_lval(utb_temp(0, I32)), UTB_NONE); /* 2 *T0 (header) */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 5 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* An impure call between the address-taken VAR's store and the test
 * invalidates V0's tracked value (the callee could write through a stored
 * pointer), so the value is unknown and the pass must not fire. */
UT_TEST(test_loop_dead_call_invalidates_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 V0=0 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);    /* 1 T0=&V0 (addrtaken) */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_temp(1, I32), utb_imm(0, I32)); /* 2 call() */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_lval(utb_temp(0, I32)), UTB_NONE); /* 3 *T0 (header) */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);           /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The exit-test value is never set to a known constant (V0 is read without any
 * defining instruction), so the branch outcome is unknown and the pass must not
 * fire. */
UT_TEST(test_loop_dead_unknown_value_no_fire)
{
  TCCIRState *ir = utb_new();

  /* entry: an instruction that does not define V0 with a constant. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(5, I32), utb_imm(0, I32), UTB_NONE);  /* 0 */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 V0 unknown */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 6 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* No loop at all (no backward jump): the pass detects no back-edge and returns
 * 0, leaving the IR untouched. */
UT_TEST(test_loop_dead_no_loop_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 forward */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 3 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The header's conditional jump targets a location INSIDE the loop, so it is
 * not an exit branch and the pass must not fire (the JUMPIF target's block is
 * a member of the natural loop). */
UT_TEST(test_loop_dead_jumpif_target_inside_loop_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 entry */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 header */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 -> 3 (inside) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 3 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 5 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The JUMPIF carries a token the pass does not understand for a TEST_ZERO
 * (only EQ/NE are handled).  The branch evaluation returns unknown for a
 * TEST_ZERO with a relational token, so the pass must not fire even though V0
 * is a known constant. */
UT_TEST(test_loop_dead_test_zero_unknown_tok_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 2 LT on TEST_ZERO */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 6 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* An intervening control-flow JUMP before the exit JUMPIF on the entry path
 * breaks the straight-line walk (it bails on the first JUMP/JUMPIF), so the
 * first-iteration values cannot be trusted and the pass must not fire. */
UT_TEST(test_loop_dead_intervening_jump_bails_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 1 forward jump on entry path */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 2 header */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 4 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 */

  int changes = ssa_opt_first_iter_exit(ir);

  /* The walk from 0..jumpif hits the JUMP at idx 1 and bails -> no fire. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Idempotence: the loop is eliminated on the first application; a second
 * application finds no remaining back-edge (it was NOPed) and makes no
 * further changes.  Run to fixpoint and confirm exactly one elimination. */
UT_TEST(test_loop_dead_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE);      /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE);  /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 6 */

  int total = utb_run_to_fixpoint(ir, ssa_opt_first_iter_exit, 10);
  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(ssa_opt_first_iter_exit(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEW vs legacy: the header has an extra predecessor from outside the loop (a
 * second JUMP into the header from below).  The explicit single-entry guard
 * (header preds must be exactly {entry block, latch}) must decline, even
 * though the walked value would prove the exit taken. */
UT_TEST(test_loop_dead_extra_header_pred_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 entry */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 header */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);  /* 3 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 back-edge */
  int x = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);   /* 5 extra pred -> header */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 6 exit target */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, x), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEW vs legacy: non-loop code interleaved INSIDE the flat [header, latch]
 * index range must survive the elimination.  The body jumps over an
 * interleaved chunk (reached from the exit path) to the latch; block-membership
 * NOPing keeps the chunk, where the legacy flat-range NOPing corrupted it.
 *
 *    0: ASSIGN V0=0        entry
 *    1: TEST_ZERO V0       header
 *    2: JUMPIF 8, EQ       exit -> 8
 *    3: ASSIGN V1=9        body
 *    4: JUMP 7             skip the interleaved chunk
 *    5: ASSIGN V5=1        interleaved non-loop code (reached from 9)
 *    6: JUMP 10            interleaved chunk leaves
 *    7: JUMP 1             latch / back-edge
 *    8: ASSIGN V2=7        exit target
 *    9: JUMP 5             jumps into the interleaved chunk from outside
 *   10: RETURNVOID
 */
UT_TEST(test_loop_dead_interleaved_nonloop_code_survives)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 1 */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 */
  int b = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE); /* 3 */
  int sk = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(7, I32), UTB_NONE, UTB_NONE);   /* 4 */
  int i1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(5, I32), utb_imm(1, I32), UTB_NONE); /* 5 */
  int i2 = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(10, I32), UTB_NONE, UTB_NONE);  /* 6 */
  int e = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);    /* 7 */
  int xt = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(7, I32), UTB_NONE); /* 8 */
  int r = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);    /* 9 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 10 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 1);
  /* Exit rewritten. */
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, j)), 8);
  /* Loop member blocks NOPed: header test, body, skip-jump, latch. */
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, b), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, sk), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, e), TCCIR_OP_NOP);
  /* Interleaved non-loop chunk inside the flat range SURVIVES. */
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, i2)), 10);
  /* Post-loop code untouched. */
  UT_ASSERT_EQ(utb_op(ir, xt), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, r), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEW vs legacy: a bottom-tested (already-rotated) loop declines.  The only
 * conditional branch is the back-edge itself: its target is a member of the
 * loop, so no exit branch is found — even though the walked V0 == 0 would
 * satisfy the EQ token if it were misread as an exit. */
UT_TEST(test_loop_dead_bottom_tested_no_fire)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 entry */
  int b = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE); /* 1 body (back-edge target) */
  int t = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 2 bottom test */
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 3 cond back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 4 */

  int changes = ssa_opt_first_iter_exit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, b), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, t), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_loop_dead)
{
  UT_COVERS("ssa_first_iter_exit");
  UT_RUN(test_loop_dead_test_zero_eq_fires);
  UT_RUN(test_loop_dead_test_zero_ne_fires);
  UT_RUN(test_loop_dead_test_zero_eq_not_taken_no_fire);
  UT_RUN(test_loop_dead_cmp_lt_fires);
  UT_RUN(test_loop_dead_cmp_lt_false_no_fire);
  UT_RUN(test_loop_dead_cmp_ult_unsigned_semantics_no_fire);
  UT_RUN(test_loop_dead_lea_deref_fires);
  UT_RUN(test_loop_dead_store_through_ptr_fires);
  UT_RUN(test_loop_dead_call_invalidates_no_fire);
  UT_RUN(test_loop_dead_unknown_value_no_fire);
  UT_RUN(test_loop_dead_no_loop_no_fire);
  UT_RUN(test_loop_dead_jumpif_target_inside_loop_no_fire);
  UT_RUN(test_loop_dead_test_zero_unknown_tok_no_fire);
  UT_RUN(test_loop_dead_intervening_jump_bails_no_fire);
  UT_RUN(test_loop_dead_idempotent);
  UT_RUN(test_loop_dead_extra_header_pred_no_fire);
  UT_RUN(test_loop_dead_interleaved_nonloop_code_survives);
  UT_RUN(test_loop_dead_bottom_tested_no_fire);
}
