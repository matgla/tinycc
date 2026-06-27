/*
 *  test_opt_branch_fold.c - suite for the legacy branch-folding pass
 *  (ir/opt_branch.c: tcc_ir_opt_branch_folding, driven by the generators in
 *   ir/opt_gens_branch.c).
 *
 *  The pass folds statically-known comparisons into unconditional jumps or NOPs:
 *    - CMP #imm1, #imm2 followed by JUMPIF(cond) -> JUMP / NOP pair.
 *    - TEST_ZERO #imm followed by JUMPIF(EQ/NE) -> JUMP / NOP pair.
 *
 *  These tests drive the bare pass entry point on hand-built IR and inspect the
 *  resulting instructions directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_branch_folding(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Comparison condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_EQ  0x94 /* ==        */
#define TOK_NE  0x95 /* !=        */
#define TOK_UGE 0x93 /* unsigned >= */
#define TOK_LT  0x9c /* signed <  */
#define TOK_GT  0x9f /* signed >  */

/* Read a jump's current target index. */
static int jump_target(TCCIRState *ir, int i)
{
  return (int)utb_dest(ir, i).u.imm32;
}

static IROperand branch_utb_imm64(TCCIRState *ir, int64_t val, int btype)
{
  uint32_t idx = tcc_ir_pool_add_i64(ir, val);
  return irop_make_i64(-1, idx, btype);
}

/* --------------------------------------------------------- positive cases */

/* CMP #3, #5 ; JUMPIF(<) -> branch is taken (3 < 5).
 *   i0: CMP   3, 5
 *   i1: JUMPIF (<) -> #2
 *   i2: RETURNVOID
 * After folding: CMP -> NOP, JUMPIF -> unconditional JUMP to #2. */
UT_TEST(test_branch_fold_cmp_signed_lt_taken)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(3, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, ijmp), 2);

  utb_free(ir);
  return 0;
}

/* TEST_ZERO #0 ; JUMPIF(NE) -> branch is NOT taken (0 != 0 is false).
 *   i0: TEST_ZERO 0
 *   i1: JUMPIF (NE) -> #2
 *   i2: RETURNVOID
 * After folding: both instructions become NOP (fall-through). */
UT_TEST(test_branch_fold_test_zero_zero_ne_not_taken)
{
  TCCIRState *ir = utb_new();

  int itest = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(0, I32), UTB_NONE);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, itest), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* TEST_ZERO #42 ; JUMPIF(EQ) -> branch is NOT taken (42 == 0 is false).
 *   i0: TEST_ZERO 42
 *   i1: JUMPIF (EQ) -> #2
 *   i2: RETURNVOID
 * After folding: both become NOP. */
UT_TEST(test_branch_fold_test_zero_nonzero_eq_not_taken)
{
  TCCIRState *ir = utb_new();

  int itest = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(42, I32), UTB_NONE);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, itest), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* CMP #0xabcd0000, #-1412628480 ; JUMPIF(NE) -> branch is NOT taken.
 * const_prop_tmp may materialize one side as a pooled 64-bit raw unsigned
 * value and the other as an IMM32 sign-extended value; branch folding must
 * compare the target-width 32-bit bit pattern, not the host int64 value. */
UT_TEST(test_branch_fold_cmp_i32_raw_bits_eq)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int64_t raw = (int64_t)(uint32_t)0xabcd0000u;
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, branch_utb_imm64(ir, raw, I32), utb_imm((int32_t)0xabcd0000u, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* CMP #-2, #-16 ; JUMPIF(>=U) -> branch is taken in 32-bit unsigned space.
 * This mirrors vrp-6's `a - b < UINT_MAX - 15U` guard after const propagation. */
UT_TEST(test_branch_fold_cmp_i32_unsigned_ge_taken)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(-2, I32), utb_imm(-16, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_UGE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, ijmp), 2);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- negative cases */

/* CMP T0, #0 with a non-immediate operand cannot be folded.
 *   i0: CMP   T0, #0
 *   i1: JUMPIF (EQ) -> #2
 *   i2: RETURNVOID
 * The pass must leave both instructions unchanged. */
UT_TEST(test_branch_fold_cmp_non_immediate_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, ijmp), 2);

  utb_free(ir);
  return 0;
}

/* TEST_ZERO with a non-immediate operand cannot be folded.
 *   i0: TEST_ZERO T1
 *   i1: JUMPIF (NE) -> #2
 *   i2: RETURNVOID
 * The pass must leave both instructions unchanged. */
UT_TEST(test_branch_fold_test_zero_non_immediate_no_fold)
{
  TCCIRState *ir = utb_new();

  int itest = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(1, I32), UTB_NONE);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, itest), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* CMP #1, #1 not followed by a JUMPIF has no consumer to fold.
 *   i0: CMP   1, 1
 *   i1: ASSIGN T0 <- #0
 * Nothing should change. */
UT_TEST(test_branch_fold_cmp_no_jump_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(1, I32));
  int iassign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_branch_folding(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, iassign), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------- idempotence case */

/* A foldable sequence must converge: the first run makes the expected change,
 * the second run reports zero changes and leaves the IR well-formed.
 *   i0: CMP   9, 4
 *   i1: JUMPIF (>) -> #2
 *   i2: RETURNVOID          (jump target, keeps the target in-bounds) */
UT_TEST(test_branch_fold_cmp_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(9, I32), utb_imm(4, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_branch_folding, 5);

  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, ijmp), 2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_branch_fold)
{
  UT_COVERS("branch_fold");

  UT_RUN(test_branch_fold_cmp_signed_lt_taken);
  UT_RUN(test_branch_fold_test_zero_zero_ne_not_taken);
  UT_RUN(test_branch_fold_test_zero_nonzero_eq_not_taken);
  UT_RUN(test_branch_fold_cmp_i32_raw_bits_eq);
  UT_RUN(test_branch_fold_cmp_i32_unsigned_ge_taken);
  UT_RUN(test_branch_fold_cmp_non_immediate_no_fold);
  UT_RUN(test_branch_fold_test_zero_non_immediate_no_fold);
  UT_RUN(test_branch_fold_cmp_no_jump_no_fold);
  UT_RUN(test_branch_fold_cmp_idempotent);
}
