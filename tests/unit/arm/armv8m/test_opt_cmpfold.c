/*
 *  test_opt_cmpfold.c - suite for the comparison fold/fuse passes
 *  (ir/opt_constprop.c: tcc_ir_opt_cmp_expr_fold / tcc_ir_opt_cmp_const_offset_fold,
 *   ir/opt_cmp_fuse.c: tcc_ir_opt_cmp_field_fuse).
 *
 *  These isolated tests drive the three comparison-fold entry points on
 *  hand-built IR: tcc_ir_opt_cmp_const_offset_fold and tcc_ir_opt_cmp_expr_fold
 *  (ir/opt_constprop.c) and tcc_ir_opt_cmp_field_fuse (ir/opt_cmp_fuse.c).
 *
 *  cmp_const_offset_fold collapses `A = B (+/-) K ; CMP A,B ; JUMPIF cond`
 *  into a constant branch by substituting A = B + K (so the comparison reduces
 *  to `K cond 0`).  When the condition is statically true it rewrites the CMP
 *  to NOP and the JUMPIF to an unconditional JUMP (then runs DCE); when false
 *  it NOPs both.  Guards (asserted as no-ops here): the ADD base's lval-ness
 *  must match the CMP operand's (the historical wide-string-literal heap crash
 *  came from a missing is_lval guard in these cmp-fold passes), the condition
 *  must be signed or EQ/NE, and K must be non-zero.
 *
 *  A hand-built IR sequence is run through the bare pass entry point and the
 *  resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

#include <limits.h>

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir);
int tcc_ir_opt_cmp_const_offset_fold(TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Comparison condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_EQ  0x94 /* ==        */
#define TOK_NE  0x95 /* !=        */
#define TOK_LT  0x9c /* signed <  */
#define TOK_LE  0x9e /* signed <= */
#define TOK_GT  0x9f /* signed >  */
#define TOK_GE  0x9d /* signed >= */
#define TOK_ULT 0x92 /* unsigned < */

/* ------------------------------------------------------------------ tests */

/* POSITIVE: `A = B + 5 ; CMP A,B ; JUMPIF >` reduces to `5 > 0` == true.
 *   i0: ADD   T1 <- T0 + 5
 *   i1: CMP   T1, T0
 *   i2: JUMPIF (>) -> #4
 *   i3: RETURNVOID         (dead after fold: only reachable via fall-through
 *                           from the JUMPIF, which became an unconditional JUMP)
 *   i4: RETURNVOID         (jump target)
 * After fold: CMP -> NOP, JUMPIF -> unconditional JUMP to the same target. */
UT_TEST(test_cmpfold_offset_signed_true_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  /* The pass fired: CMP folded away, JUMPIF became an unconditional JUMP to the
   * original target.  changes also includes the follow-up DCE, so assert > 0. */
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* GUARD (the historical is_lval bug): the ADD base is a plain value `T0` but the
 * CMP's second operand is a deref `*(T0)` (is_lval).  `*(p)+K` (a loaded value)
 * does not make `A == p + K` provable from `B == p`, so the pass must NOT fold.
 *   i0: ADD   T1 <- T0 + 5      (base T0 NOT lval)
 *   i1: CMP   T1, *(T0)         (src2 = T0 with is_lval = 1)
 *   i2: JUMPIF (>) -> #4 */
UT_TEST(test_cmpfold_offset_lval_base_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();

  IROperand t0_deref = utb_temp(0, I32);
  t0_deref.is_lval = 1;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), t0_deref);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: unsigned conditions need an overflow proof and are skipped.  Same
 * foldable arithmetic shape as the positive test but with an unsigned `<`. */
UT_TEST(test_cmpfold_offset_unsigned_cond_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_ULT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a zero offset (`A = B + 0`) gives delta 0; the pass bails on k == 0
 * (the comparison is genuinely `B vs B`, handled elsewhere), so no fold here. */
UT_TEST(test_cmpfold_offset_zero_delta_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: no ADD/SUB feeding either CMP operand, so there is no provable
 * constant offset and nothing folds. */
UT_TEST(test_cmpfold_offset_no_arith_def_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ----------------------------------------------------------- helpers */

/* Live-interval setup needed by passes that allocate fresh temporaries
 * (cmp_field_fuse) or read interval flags (cmp_expr_fold asymmetric path). */
static void utb_init_intervals_with_temp_base(TCCIRState *ir, int temp_base, int size)
{
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->temporary_variables_live_intervals_size = size;
  ir->next_temporary_variable = temp_base;

  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->variables_live_intervals_size = size;
  ir->next_local_variable = 0;

  ir->parameters_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->parameters_live_intervals_size = size;
  ir->next_parameter = 0;
}

static void utb_free_intervals(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_free(ir->temporary_variables_live_intervals);
  tcc_free(ir->variables_live_intervals);
  tcc_free(ir->parameters_live_intervals);
  ir->temporary_variables_live_intervals = NULL;
  ir->variables_live_intervals = NULL;
  ir->parameters_live_intervals = NULL;
}

/* AND-extract feeder for cmp_field_fuse tests.  src_is_lval lets us test the
 * lval base guard without mutating operand bitfields by hand. */
static int utb_emit_and_extract(TCCIRState *ir, int dest_pos, int src_param_pos,
                                int32_t mask, int src_is_lval)
{
  IROperand src = utb_param(src_param_pos, I32);
  if (src_is_lval)
    src = utb_lval(src);
  return utb_emit(ir, TCCIR_OP_AND, utb_temp(dest_pos, I32), src, utb_imm(mask, I32));
}

/* -------------------------------- cmp_const_offset_fold corner cases */

/* ★ SEMI-ORACLE: negative constant offset.  A = B - 3; CMP A,B; JUMPIF <S
 * reduces to "(-3) < 0", which is independently true. */
UT_TEST(test_cmpfold_offset_negative_delta_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(-3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: non-zero offset makes A != B, so EQ is always false. */
UT_TEST(test_cmpfold_offset_eq_false_nops_both)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: non-zero offset makes A != B, so NE is always true. */
UT_TEST(test_cmpfold_offset_ne_true_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Integer-boundary / overflow guard: a delta that does not fit in int32 must
 * not be folded, even on a 32-bit CMP. */
UT_TEST(test_cmpfold_offset_int64_delta_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, (int64_t)INT32_MAX + 1);
  IROperand big = irop_make_i64(-1, pool_idx, I32);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), big);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* When the ADD base and the CMP base are *both* lvals, the loaded value is
 * the same on both sides and folding is sound. */
UT_TEST(test_cmpfold_offset_lval_base_match_folds)
{
  TCCIRState *ir = utb_new();
  IROperand base = utb_lval(utb_temp(0, I32));

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), base, utb_imm(2, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), base);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* An address-taken base between the ADD and the CMP can be mutated through
 * aliases, so the offset relation is no longer provable. */
UT_TEST(test_cmpfold_offset_address_taken_base_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(2, I32), utb_temp(0, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_GT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* If the base vreg is redefined between the offset-producing ADD and the CMP,
 * the two operands no longer share a single reaching definition. */
UT_TEST(test_cmpfold_offset_base_redefined_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_GT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* The same offset fold works for CMP followed by SELECT (4th operand cond). */
UT_TEST(test_cmpfold_offset_select_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(6, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int isel = utb_emit4(ir, TCCIR_OP_SELECT, utb_temp(2, I32),
                       utb_imm(10, I32), utb_imm(20, I32),
                       utb_imm(TOK_GT, I32));

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, isel), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, isel)), 10);

  utb_free(ir);
  return 0;
}

/* The ADD feeding the CMP need not be immediately adjacent; the pass scans
 * backward to find the single definition. */
UT_TEST(test_cmpfold_offset_non_adjacent_def_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(7, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(2, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 6);

  utb_free(ir);
  return 0;
}

/* FIXPOINT: a second run of the pass must make no further changes and leave
 * a structurally well-formed IR. */
UT_TEST(test_cmpfold_offset_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_const_offset_fold, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* DEGENERATE: empty function, single instruction, and no-CMP function all
 * return 0 without crashing. */
UT_TEST(test_cmpfold_offset_empty_and_tiny)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_cmp_const_offset_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_cmp_const_offset_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_cmp_const_offset_fold(ir), 0);
  utb_free(ir);

  return 0;
}

/* SUB-defined offset: A = B - 4; CMP A,B; JUMPIF <S reduces to "(-4) < 0" == true.
 * Exercises the dq->op == TCCIR_OP_SUB branch (k negated), distinct from the
 * ADD-with-negative-immediate path covered above. */
UT_TEST(test_cmpfold_offset_sub_def_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Swap orientation: the offset temp is the CMP's *second* operand.
 *   A = B + 5 ; CMP B, A ; JUMPIF <S   (src1 = B, src2 = A = B + 5)
 * The pass must try swap=1 (a = vr2 = A, b = vr1 = B), yielding delta = -k = -5,
 * so "(-5) < 0" == true. Covers the swap==1 search branch. */
UT_TEST(test_cmpfold_offset_swap_orientation_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 4);

  utb_free(ir);
  return 0;
}

/* Commutative ADD base: A = K + B (immediate first). The pass must match the
 * `irop_get_vreg(ds2) == b` branch (ADD only). "5 > 0" == true. */
UT_TEST(test_cmpfold_offset_commutative_add_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(5, I32), utb_temp(0, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* COMMON-BASE fold: neither CMP operand is the other's base, but both are
 * constant offsets of the same base X.
 *   A = X + 1 ; B = X + 3 ; CMP A,B ; JUMPIF <S   ⇒  (1 - 3) < 0 == true. */
UT_TEST(test_cmpfold_offset_common_base_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(2, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_LT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_dest(ir, ijmp).u.imm32, 5);

  utb_free(ir);
  return 0;
}

/* COMMON-BASE, GE orientation: A = X + 3 ; B = X + 1 ; CMP A,B ; JUMPIF <=S
 * ⇒ (3 - 1) <= 0 == false, so both NOP. Exercises the >0 delta / false path. */
UT_TEST(test_cmpfold_offset_common_base_le_false_nops)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(2, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* GUARD: distinct bases (X vs Y) give no provable offset relation. */
UT_TEST(test_cmpfold_offset_common_base_distinct_base_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(3, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(2, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* GUARD: shared base redefined between the two offset ADDs — A used the old X,
 * B the new one, so K1-K2 is not the real delta. Reaching-def guard must reject. */
UT_TEST(test_cmpfold_offset_common_base_redefined_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(50, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(2, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_const_offset_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* -------------------------------- cmp_expr_fold corner cases */

/* FIXED: comparing a register value to itself.  EQ is always true, so the
 * fold is NOP(CMP) + JUMP(target).  The pass now folds identical (non-lval)
 * vregs via evaluate_compare_condition(0,0,tok). */
UT_TEST(test_cmpfold_expr_same_vreg_eq_true_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* FIXED: x > x is always false, so CMP+JUMPIF GT folds to two NOPs (fall
 * through). */
UT_TEST(test_cmpfold_expr_same_vreg_gt_false_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_GT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* FIXED: unsigned compare of a value to itself.  (uint32_t)x < x is always
 * false, so the CMP+JUMPIF ULT folds to two NOPs. */
UT_TEST(test_cmpfold_expr_same_vreg_ult_false_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(0, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* A plain value and a dereference of the same vreg are different values,
 * so the equality fold must not fire. */
UT_TEST(test_cmpfold_expr_same_vreg_lval_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();
  IROperand rhs = utb_lval(utb_temp(0, I32));

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), rhs);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* FIXED: two equal immediate operands fold (EQ is true).  The both-nonvreg
 * branch now compares integer immediates by value.  (Enabling this also
 * required fixing a latent use-before-def bug in `single_value_tmp` that the
 * extra fold exposed — see Findings #6 in PASS_COVERAGE.md.) */
UT_TEST(test_cmpfold_expr_imm_imm_equal_no_fold)
{
  TCCIRState *ir = utb_new();

  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(7, I32), utb_imm(7, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: single-def temp assigned an immediate equal to the other
 * CMP operand.  EQ folds to an unconditional jump. */
UT_TEST(test_cmpfold_expr_asymmetric_vreg_imm_eq_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(9, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* ★ SEMI-ORACLE: same shape as above, NE of equal values is false. */
UT_TEST(test_cmpfold_expr_asymmetric_vreg_imm_ne_false_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* Two distinct temps that compute the same pure expression are value-equal,
 * so the comparison folds. */
UT_TEST(test_cmpfold_expr_pure_def_equal_folds)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_param(0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(4, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_expr_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* FIXPOINT: a foldable CMP+JUMPIF converges in one run; the second run makes
 * no changes and the IR stays well-formed. */
UT_TEST(test_cmpfold_expr_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 0, 16);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(9, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_expr_fold, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* DEGENERATE: empty or too-short IR returns 0 without crashing. */
UT_TEST(test_cmpfold_expr_empty_and_tiny)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  UT_ASSERT_EQ(tcc_ir_opt_cmp_expr_fold(ir), 0);
  utb_free(ir);

  return 0;
}

/* -------------------------------- cmp_field_fuse corner cases */

/* Field width 1: two single-bit fields (bit 0 and bit 31) fuse. */
UT_TEST(test_cmpfold_field_fuse_width_1_bits)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00000001, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00000001, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x80000000, 0);
  utb_emit_and_extract(ir, 3, 1, 0x80000000, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);

  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 4));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, 4)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 5)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, 5)), 0x80000001u);

  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, c2)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, c2)), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* Field width 31: a 31-bit mask fused with a single-bit mask. */
UT_TEST(test_cmpfold_field_fuse_width_31)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x7FFFFFFF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x7FFFFFFF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x80000000, 0);
  utb_emit_and_extract(ir, 3, 1, 0x80000000, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* ≥3 fields: three byte fields fuse into one XOR+AND+CMP. */
UT_TEST(test_cmpfold_field_fuse_three_fields)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 6, 16);

  utb_emit_and_extract(ir, 0, 0, 0x000000FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x000000FF, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0x0000FF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0x0000FF00, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 4, 0, 0x00FF0000, 0);
  utb_emit_and_extract(ir, 5, 1, 0x00FF0000, 0);
  int c3 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(4, I32), utb_temp(5, I32));
  int j3 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c3), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j3), TCCIR_OP_JUMPIF);

  UT_ASSERT_EQ(utb_op(ir, 8), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 6));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, 8)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  UT_ASSERT_EQ(utb_op(ir, 9), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 9)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 7));
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, 9)), 0x00FFFFFFu);

  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c3)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 7));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, c3)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, c3)), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

/* FIXPOINT: cmp_field_fuse converges after one fusion. */
UT_TEST(test_cmpfold_field_fuse_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_init_intervals_with_temp_base(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(TOK_NE, I32), UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_field_fuse, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free_intervals(ir);
  utb_free(ir);
  return 0;
}

UT_COVERS("cmp_fold");
