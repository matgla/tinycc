/*
 *  test_opt_utils.c - suite for ir/opt_utils.c (shared pre-SSA optimizer
 *  utilities: condition-token helpers, constant evaluators, BB/CFG
 *  predicates, purity tables, expression equality, call-param helpers, and
 *  the callee-symbol-replacement helpers).
 *
 *  opt_utils.c is a *shared library* consumed by many other passes
 *  (opt_branch.c, opt_promote.c, opt_dce.c, opt_memory.c, ...); until this
 *  file, none of its ~35 exported entry points had a dedicated direct-call
 *  unit test -- other suites only reference it in comments as the origin of
 *  the JUMPIF condition-token constants (see e.g. test_opt_cmpfold.c,
 *  test_opt_branch_fold.c).  These tests call the opt_utils.c functions
 *  directly rather than through a higher-level pass, so a regression here is
 *  pinned at its source instead of only showing up as a symptom three passes
 *  downstream.
 *
 *  Oracle asserts (exact return values / operand shapes), not
 *  characterization.  Each testable helper gets at least one positive and
 *  one negative/guard case; a few helpers are documented as untestable in
 *  this harness (env-var-cached pass_disabled, elfsym()-gated constant
 *  string extraction, external_global_sym()-gated callee replacement) with
 *  the concrete reason inline.
 */

#include "ir_build.h"

#include "ut.h"

#include "opt_utils.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define I8  IROP_BTYPE_INT8
#define I16 IROP_BTYPE_INT16

/* Comparison condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_ULE 0x96
#define TOK_UGT 0x97
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

/* ----------------------------------------------------------------- helpers */

/* Build a SYMREF callee operand whose token is `tok` (utb_set_tok_str maps
 * `tok` to a name for get_tok_str()-gated logic).  Caller must have called
 * utb_pools_init(ir) first.  Same pattern as test_opt_float_branch.c /
 * test_opt_promote_extra.c. */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  memset(sym, 0, sizeof(*sym));
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* =========================================================================
 * tcc_ir_opt_pass_disabled
 * ========================================================================= */

/* NOTE ON TESTABILITY: tcc_ir_opt_pass_disabled() memoizes getenv(
 * "TCC_DISABLE_PASS") into a function-local `static` on its FIRST call ever
 * made in the process (see opt_utils.c:30-35, `if (!checked) { checked = 1;
 * disabled = getenv(...); }`).  The shared UT binary runs test_opt_knownbits.c
 * (tcc_ir_opt_known_bits -> pass_disabled("known_bits")) and
 * test_opt_branch_fold.c-family suites (-> pass_disabled("branch_fold"))
 * BEFORE this suite in test_main.c's UT_RUN_SUITE order, so by the time this
 * test runs the cache is already primed from whatever TCC_DISABLE_PASS was
 * (or was not) set to when the process started.  There is no supported way
 * to reset the cache from a unit test (no accessor), and setenv() after the
 * first call would have no effect.  This is safe to test ONLY for the
 * environment this binary always runs under in CI/local dev:
 * TCC_DISABLE_PASS unset -- `disabled` resolves to NULL either way (whether
 * read now or already cached), so `not disabled` is a deterministic oracle
 * regardless of call ordering. */
UT_TEST(test_pass_disabled_unset_env_never_disables)
{
  if (getenv("TCC_DISABLE_PASS") != NULL)
  {
    /* Environment doesn't match the assumption this test relies on --
     * skip rather than assert something we can't reason about. */
    return 0;
  }
  UT_ASSERT_EQ(tcc_ir_opt_pass_disabled("dse"), 0);
  UT_ASSERT_EQ(tcc_ir_opt_pass_disabled("const_prop"), 0);
  UT_ASSERT_EQ(tcc_ir_opt_pass_disabled(""), 0);
  return 0;
}

/* NULL name is handled defensively regardless of cache state (the `!name`
 * check short-circuits before any string comparison). */
UT_TEST(test_pass_disabled_null_name_returns_0)
{
  UT_ASSERT_EQ(tcc_ir_opt_pass_disabled(NULL), 0);
  return 0;
}

/* =========================================================================
 * is_power_of_2
 * ========================================================================= */

UT_TEST(test_is_power_of_2_positive_powers)
{
  UT_ASSERT_EQ(is_power_of_2(1), 0);
  UT_ASSERT_EQ(is_power_of_2(2), 1);
  UT_ASSERT_EQ(is_power_of_2(4), 2);
  UT_ASSERT_EQ(is_power_of_2(8), 3);
  UT_ASSERT_EQ(is_power_of_2(1024), 10);
  UT_ASSERT_EQ(is_power_of_2((int64_t)1 << 40), 40);
  return 0;
}

UT_TEST(test_is_power_of_2_non_powers_and_nonpositive)
{
  UT_ASSERT_EQ(is_power_of_2(0), -1);
  UT_ASSERT_EQ(is_power_of_2(-1), -1);
  UT_ASSERT_EQ(is_power_of_2(-8), -1);
  UT_ASSERT_EQ(is_power_of_2(3), -1);
  UT_ASSERT_EQ(is_power_of_2(6), -1);
  UT_ASSERT_EQ(is_power_of_2(100), -1);
  return 0;
}

/* =========================================================================
 * evaluate_compare_condition
 * ========================================================================= */

UT_TEST(test_evaluate_compare_condition_signed_tokens)
{
  UT_ASSERT_EQ(evaluate_compare_condition(5, 5, TOK_EQ), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 6, TOK_EQ), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 6, TOK_NE), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 5, TOK_NE), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(-1, 1, TOK_LT), 1);   /* signed: -1 < 1 */
  UT_ASSERT_EQ(evaluate_compare_condition(1, -1, TOK_LT), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 5, TOK_GE), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(4, 5, TOK_GE), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 5, TOK_LE), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(6, 5, TOK_LE), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(6, 5, TOK_GT), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 6, TOK_GT), 0);
  return 0;
}

UT_TEST(test_evaluate_compare_condition_unsigned_tokens_treat_negative_as_huge)
{
  /* -1 as uint64_t is UINT64_MAX -- the opposite of the signed comparison. */
  UT_ASSERT_EQ(evaluate_compare_condition(-1, 1, TOK_ULT), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(1, -1, TOK_ULT), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(-1, 1, TOK_UGE), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(1, -1, TOK_UGE), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(5, 5, TOK_ULE), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(-1, 1, TOK_ULE), 0);
  UT_ASSERT_EQ(evaluate_compare_condition(-1, 1, TOK_UGT), 1);
  UT_ASSERT_EQ(evaluate_compare_condition(1, -1, TOK_UGT), 0);
  return 0;
}

UT_TEST(test_evaluate_compare_condition_unknown_token_returns_minus1)
{
  UT_ASSERT_EQ(evaluate_compare_condition(1, 1, 0x00), -1);
  UT_ASSERT_EQ(evaluate_compare_condition(1, 1, TOK_LAND), -1);
  return 0;
}

/* =========================================================================
 * ir_opt_eval_const_u64
 * ========================================================================= */

/* POSITIVE: a plain immediate operand evaluates to itself without touching
 * `ir` (depth/vreg lookups never engaged). */
UT_TEST(test_eval_const_u64_immediate)
{
  TCCIRState *ir = utb_new();
  uint64_t out = 0;
  IROperand imm = utb_imm(42, I32);
  int ok = ir_opt_eval_const_u64(ir, imm, /*use_idx*/ 0, &out, 0);
  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ((int)out, 42);
  utb_free(ir);
  return 0;
}

/* POSITIVE: T0 = #7; T1 = T0 + #3 evaluated at the ADD's use site folds to 10
 * through one level of ASSIGN-free ADD recursion. */
UT_TEST(test_eval_const_u64_add_chain_folds)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32), UTB_NONE);       /* 0 */
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32)); /* 1 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_temp(1, I32), add + 1, &out, 0);
  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ((int)out, 10);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a 32-bit SHR of a value whose top bits look like a sign-extended
 * negative (0xFFFFFFFF80000000 stored as a 64-bit constant, but the source
 * operand is declared INT32) is masked to 32 bits before shifting, per the
 * shift_is_64 width-detection comment in opt_utils.c. */
UT_TEST(test_eval_const_u64_shr_uses_32bit_width_for_int32_operand)
{
  TCCIRState *ir = utb_new();
  /* T0 = #-8 (an I32 immediate; the recursive immediate evaluator sign-
   * extends it to the 64-bit v1 = 0xFFFFFFFFFFFFFFF8); T1 = T0 >> 1.  Since
   * T0's declared type is I32 (not I64/F64), shift_is_64 is false and SHR
   * truncates v1 to 32 bits BEFORE shifting: (uint32_t)v1 == 0xFFFFFFF8. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-8, I32), UTB_NONE);         /* 0 */
  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32)); /* 1 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_temp(1, I32), shr + 1, &out, 0);
  UT_ASSERT_EQ(ok, 1);
  /* 32-bit logical shift: (uint32_t)0xFFFFFFF8 >> 1 == 0x7FFFFFFC. */
  UT_ASSERT_EQ((uint64_t)out, (uint64_t)0x7FFFFFFCu);

  utb_free(ir);
  return 0;
}

/* POSITIVE: ROR by a non-multiple-of-32 amount rotates within 32 bits. */
UT_TEST(test_eval_const_u64_ror_rotates_32bit)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);          /* 0 */
  int ror = utb_emit(ir, TCCIR_OP_ROR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32)); /* 1 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_temp(1, I32), ror + 1, &out, 0);
  UT_ASSERT_EQ(ok, 1);
  /* ROR(1, by 1) == 0x80000000 */
  UT_ASSERT_EQ((uint64_t)out, (uint64_t)0x80000000u);

  utb_free(ir);
  return 0;
}

/* POSITIVE: ZEXT masks the recursively-evaluated source to its declared
 * (narrower) width before extending. */
UT_TEST(test_eval_const_u64_zext_masks_to_source_width)
{
  TCCIRState *ir = utb_new();
  /* T0 (INT8) = #-1 (0xFF as int8); T1 (INT32) = ZEXT(T0). */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I8), utb_imm(-1, I8), UTB_NONE);          /* 0 */
  int zext = utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, I32), utb_temp(0, I8), UTB_NONE); /* 1 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_temp(1, I32), zext + 1, &out, 0);
  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ((uint64_t)out, (uint64_t)0xFFu);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a vreg with TWO definitions cannot be soundly traced --
 * ir_opt_eval_const_u64 must bail (tcc_ir_vreg_has_single_def guard) rather
 * than fold using whichever def tcc_ir_find_defining_instruction happens to
 * find first. */
UT_TEST(test_eval_const_u64_multi_def_vreg_bails_out)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32), UTB_NONE);  /* 0: def #1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);  /* 1: def #2 (redefinition) */
  int use = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE); /* 2 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_temp(0, I32), use, &out, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a vreg whose address was taken (LEA src1==vreg) between
 * def and use must not be traced -- the value could have been mutated
 * through the taken pointer between definition and this use. */
UT_TEST(test_eval_const_u64_address_taken_between_bails_out)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);        /* 0: V0 = 5 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);          /* 1: T1 = &V0 */
  int use = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(0, I32), UTB_NONE); /* 2: read V0 */

  uint64_t out = 0;
  int ok = ir_opt_eval_const_u64(ir, utb_var(0, I32), use, &out, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): depth limit (>12) stops runaway recursion even on a
 * well-formed chain -- exercise the exact boundary the code checks
 * (`depth > 12`). */
UT_TEST(test_eval_const_u64_depth_limit_bails_out)
{
  TCCIRState *ir = utb_new();
  uint64_t out = 0;
  /* depth=13 already exceeds the `> 12` gate on entry, before any operand
   * inspection -- immediate or not, the function must refuse. */
  int ok = ir_opt_eval_const_u64(ir, utb_imm(1, I32), 0, &out, 13);
  UT_ASSERT_EQ(ok, 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): NULL ir or NULL out returns 0 defensively. */
UT_TEST(test_eval_const_u64_null_args)
{
  uint64_t out = 0;
  UT_ASSERT_EQ(ir_opt_eval_const_u64(NULL, utb_imm(1, I32), 0, &out, 0), 0);

  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(ir_opt_eval_const_u64(ir, utb_imm(1, I32), 0, NULL, 0), 0);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_eval_const_string
 * ========================================================================= */

/* NEGATIVE (guard): an lval TEMP operand (dereferenced through a computed
 * temp -- `*(T)`) is explicitly excluded up front (op.is_lval &&
 * vreg_type==TEMP), regardless of the underlying definition. */
UT_TEST(test_eval_const_string_lval_temp_rejected)
{
  TCCIRState *ir = utb_new();
  const char *out = NULL;
  IROperand lval_temp = utb_lval(utb_temp(0, I32));
  int ok = ir_opt_eval_const_string(ir, lval_temp, 0, &out, 0);
  UT_ASSERT_EQ(ok, 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a plain vreg with no SYMREF base and no reaching def
 * (never assigned) fails to resolve to a string. */
UT_TEST(test_eval_const_string_no_def_fails)
{
  TCCIRState *ir = utb_new();
  const char *out = NULL;
  int ok = ir_opt_eval_const_string(ir, utb_temp(0, I32), 0, &out, 0);
  UT_ASSERT_EQ(ok, 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): NULL ir / NULL out returns 0. */
UT_TEST(test_eval_const_string_null_args)
{
  const char *out = NULL;
  UT_ASSERT_EQ(ir_opt_eval_const_string(NULL, utb_temp(0, I32), 0, &out, 0), 0);

  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(ir_opt_eval_const_string(ir, utb_temp(0, I32), 0, NULL, 0), 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): depth limit (>16) stops recursion at entry. */
UT_TEST(test_eval_const_string_depth_limit_bails_out)
{
  TCCIRState *ir = utb_new();
  const char *out = NULL;
  int ok = ir_opt_eval_const_string(ir, utb_temp(0, I32), 0, &out, 17);
  UT_ASSERT_EQ(ok, 0);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * Condition token helpers: vrp_negate_cmp_tok / vrp_swap_cmp_tok /
 * vrp_cmp_implies / fcmp_cmp_implies / invert_cond_token / invert_condition /
 * ir_negate_condition
 * ========================================================================= */

UT_TEST(test_vrp_negate_cmp_tok_all_pairs)
{
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_EQ), TOK_NE);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_NE), TOK_EQ);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_LT), TOK_GE);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_GE), TOK_LT);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_LE), TOK_GT);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_GT), TOK_LE);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_ULT), TOK_UGE);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_UGE), TOK_ULT);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_ULE), TOK_UGT);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(TOK_UGT), TOK_ULE);
  UT_ASSERT_EQ(vrp_negate_cmp_tok(0x00), -1);
  return 0;
}

UT_TEST(test_vrp_swap_cmp_tok_all_pairs)
{
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_EQ), TOK_EQ);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_NE), TOK_NE);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_LT), TOK_GT);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_GT), TOK_LT);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_LE), TOK_GE);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_GE), TOK_LE);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_ULT), TOK_UGT);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_UGT), TOK_ULT);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_ULE), TOK_UGE);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(TOK_UGE), TOK_ULE);
  UT_ASSERT_EQ(vrp_swap_cmp_tok(0x00), -1);
  return 0;
}

UT_TEST(test_vrp_cmp_implies_reflexive_and_families)
{
  /* known_true == check is always true, even for a token with no explicit
   * switch case (default branch never runs since the equality check short-
   * circuits first). */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_NE, TOK_NE), 1);

  /* EQ implies LE, GE, ULE, UGE but not LT/GT/NE. */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_LE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_GE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_ULE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_UGE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_LT), 0);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_EQ, TOK_NE), 0);

  /* LT implies LE, NE but not GT/EQ. */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_LT, TOK_LE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_LT, TOK_NE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_LT, TOK_GT), 0);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_LT, TOK_EQ), 0);

  /* GT implies GE, NE. */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_GT, TOK_GE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_GT, TOK_NE), 1);

  /* ULT implies ULE, NE; UGT implies UGE, NE. */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_ULT, TOK_ULE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_ULT, TOK_NE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_UGT, TOK_UGE), 1);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_UGT, TOK_NE), 1);

  /* A known_true token with no case in the switch (default) implies nothing
   * beyond reflexivity -- e.g. NE implies neither LE nor GE. */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_NE, TOK_LE), 0);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_NE, TOK_GE), 0);

  /* Signed/unsigned families never cross (LT does not imply ULE). */
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_LT, TOK_ULE), 0);
  UT_ASSERT_EQ(vrp_cmp_implies(TOK_ULT, TOK_LE), 0);

  return 0;
}

UT_TEST(test_fcmp_cmp_implies_families)
{
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_EQ, TOK_EQ), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_EQ, TOK_LE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_EQ, TOK_GE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_EQ, TOK_NE), 0);

  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_NE, TOK_NE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_NE, TOK_EQ), 0);

  /* LT and ULT are treated as the SAME family here (fcmp has no signedness
   * distinction) -- both imply LE, NE, ULE. */
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_LT, TOK_LE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_LT, TOK_NE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_LT, TOK_ULE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_ULT, TOK_LE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_LT, TOK_GT), 0);

  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_GT, TOK_GE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_GT, TOK_NE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_GT, TOK_UGE), 1);
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_UGT, TOK_GE), 1);

  /* default branch: a token with no case implies nothing beyond reflexivity. */
  UT_ASSERT_EQ(fcmp_cmp_implies(TOK_LE, TOK_LT), 0);

  return 0;
}

UT_TEST(test_invert_cond_token_all_pairs)
{
  UT_ASSERT_EQ(invert_cond_token(TOK_EQ), TOK_NE);
  UT_ASSERT_EQ(invert_cond_token(TOK_NE), TOK_EQ);
  UT_ASSERT_EQ(invert_cond_token(TOK_LT), TOK_GE);
  UT_ASSERT_EQ(invert_cond_token(TOK_GE), TOK_LT);
  UT_ASSERT_EQ(invert_cond_token(TOK_LE), TOK_GT);
  UT_ASSERT_EQ(invert_cond_token(TOK_GT), TOK_LE);
  UT_ASSERT_EQ(invert_cond_token(TOK_ULT), TOK_UGE);
  UT_ASSERT_EQ(invert_cond_token(TOK_UGE), TOK_ULT);
  UT_ASSERT_EQ(invert_cond_token(TOK_ULE), TOK_UGT);
  UT_ASSERT_EQ(invert_cond_token(TOK_UGT), TOK_ULE);
  UT_ASSERT_EQ(invert_cond_token(0x00), -1);
  return 0;
}

/* invert_condition() and invert_cond_token() are structurally identical
 * (same case values, disjoint case ORDER in source but same mapping) --
 * pin both independently since callers use each name in different files
 * (invert_cond_token in the CMP-fold family, invert_condition in
 * opt_promote.c per test_opt_promote_extra.c). */
UT_TEST(test_invert_condition_all_pairs)
{
  UT_ASSERT_EQ(invert_condition(TOK_GE), TOK_LT);
  UT_ASSERT_EQ(invert_condition(TOK_GT), TOK_LE);
  UT_ASSERT_EQ(invert_condition(TOK_LT), TOK_GE);
  UT_ASSERT_EQ(invert_condition(TOK_LE), TOK_GT);
  UT_ASSERT_EQ(invert_condition(TOK_EQ), TOK_NE);
  UT_ASSERT_EQ(invert_condition(TOK_NE), TOK_EQ);
  UT_ASSERT_EQ(invert_condition(TOK_UGE), TOK_ULT);
  UT_ASSERT_EQ(invert_condition(TOK_UGT), TOK_ULE);
  UT_ASSERT_EQ(invert_condition(TOK_ULT), TOK_UGE);
  UT_ASSERT_EQ(invert_condition(TOK_ULE), TOK_UGT);
  UT_ASSERT_EQ(invert_condition(0x00), -1);
  return 0;
}

UT_TEST(test_ir_negate_condition_xor_1)
{
  /* ir_negate_condition is a bare `cond ^ 1` -- used on boolean 0/1 SETIF-
   * style conditions, NOT on TOK_* comparison tokens (see
   * test_opt_promote_extra.c's backedge_phi_hoist which XORs a raw 0/1
   * "then_cond" bit, not a TOK_* value). */
  UT_ASSERT_EQ(ir_negate_condition(0), 1);
  UT_ASSERT_EQ(ir_negate_condition(1), 0);
  /* Documents the literal `^1` semantics on a non-boolean input too --
   * this is NOT condition-token-aware. */
  UT_ASSERT_EQ(ir_negate_condition(TOK_EQ), TOK_EQ ^ 1);
  return 0;
}

/* =========================================================================
 * ir_opt_build_merge_bitmap
 * ========================================================================= */

/* POSITIVE: JUMPIF contributes TWO edges -- its jump target AND a
 * fallthrough edge to i+1 (JUMPIF is not in the fallthrough-exclusion list,
 * unlike JUMP/RETURNVALUE/RETURNVOID/SWITCH_TABLE).  With a single JUMPIF
 * and JUMP in this layout every block still has exactly one predecessor
 * (no pred_count exceeds 1) and no branch targets backward, so no bit is
 * ever set -- this is the "nothing to merge" baseline the two POSITIVE
 * cases below are contrasted against. */
UT_TEST(test_build_merge_bitmap_no_merge_when_every_block_has_one_pred)
{
  TCCIRState *ir = utb_new();
  /*
   *  0: CMP                              (fallthrough -> 1)
   *  1: JUMPIF EQ -> 3                    (target -> 3; fallthrough -> 2)
   *  2: JUMP -> 4                         (target -> 4; JUMP has no fallthrough)
   *  3: RETURNVALUE #1                    (RETURNVALUE has no fallthrough)
   *  4: RETURNVALUE #2
   *
   * pred_count: [1]=1 (from 0's fallthrough), [2]=1 (from 1's fallthrough),
   * [3]=1 (from 1's jump target), [4]=1 (from 2's jump target).  All <= 1.
   */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));        /* 0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);                 /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);          /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);          /* 4 */

  int n = ir->next_instruction_index;
  uint8_t *bm = ir_opt_build_merge_bitmap(ir, n);

  for (int i = 0; i < n; i++)
    UT_ASSERT_EQ((bm[i / 8] >> (i % 8)) & 1, 0);

  tcc_free(bm);
  utb_free(ir);
  return 0;
}

/* POSITIVE: three separate unconditional JUMPs all targeting the same block
 * give it pred_count==3 (JUMP contributes no fallthrough edge of its own),
 * which is > 1 -- the merge bit is set purely from the pred_count tally,
 * independent of the direct backward-edge bit-set path (all three jumps
 * here are forward, i < target). */
UT_TEST(test_build_merge_bitmap_multiple_preds_sets_bit)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);   /* 0 -> 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);   /* 1 -> 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);   /* 2 -> 3 (fallthrough excluded: JUMP has none) */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);    /* 3: target of 0,1,2 -> pred_count=3 */

  int n = ir->next_instruction_index;
  uint8_t *bm = ir_opt_build_merge_bitmap(ir, n);

  UT_ASSERT_EQ((bm[3 / 8] >> (3 % 8)) & 1, 1); /* 3 has 3 predecessors -> merge */
  UT_ASSERT_EQ((bm[0 / 8] >> (0 % 8)) & 1, 0);
  UT_ASSERT_EQ((bm[1 / 8] >> (1 % 8)) & 1, 0);
  UT_ASSERT_EQ((bm[2 / 8] >> (2 % 8)) & 1, 0);

  tcc_free(bm);
  utb_free(ir);
  return 0;
}

/* POSITIVE: a backward branch (loop back-edge, i > target) marks its target
 * as a merge point directly, even with only ONE predecessor recorded via
 * pred_count from that edge (the loop header is also reached by the
 * preheader fallthrough, giving pred_count 2 here, but the direct `i >
 * target` bit-set path is what's under test: it fires unconditionally on
 * any backward edge). */
UT_TEST(test_build_merge_bitmap_backward_edge_sets_bit)
{
  TCCIRState *ir = utb_new();
  /*
   *  0: RETURNVOID (dummy, filler before header so header isn't index 0)
   *  1: CMP            (loop header)
   *  2: JUMPIF GE -> 4  (exit)
   *  3: JUMPIF NE -> 1  (back-edge: 3 > 1)
   *  4: RETURNVOID
   */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));       /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GE, I32), UTB_NONE);  /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(TOK_NE, I32), UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 4 */

  int n = ir->next_instruction_index;
  uint8_t *bm = ir_opt_build_merge_bitmap(ir, n);

  UT_ASSERT_EQ((bm[1 / 8] >> (1 % 8)) & 1, 1); /* header: target of backward edge 3->1 */

  tcc_free(bm);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_mark_block_starts / ir_opt_build_block_starts_bitmap
 * ========================================================================= */

UT_TEST(test_mark_block_starts_marks_jump_targets_and_entry)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);   /* 0 -> 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);    /* 1 (not a target) */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);    /* 2 (jump target) */

  int n = ir->next_instruction_index;
  int *seen = (int *)tcc_mallocz(sizeof(int) * n);
  ir_opt_mark_block_starts(ir, seen, /*gen*/ 7, n);

  UT_ASSERT_EQ(seen[0], 7); /* entry always marked */
  UT_ASSERT_EQ(seen[1], 0); /* not a jump target: untouched */
  UT_ASSERT_EQ(seen[2], 7); /* jump target */

  tcc_free(seen);
  utb_free(ir);
  return 0;
}

/* Out-of-range jump targets (>= n, defensively also < 0 though not
 * constructible here) must not write out of bounds. */
UT_TEST(test_mark_block_starts_out_of_range_target_ignored)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(99, I32), UTB_NONE, UTB_NONE); /* target way out of range */

  int n = ir->next_instruction_index;
  int *seen = (int *)tcc_mallocz(sizeof(int) * n);
  ir_opt_mark_block_starts(ir, seen, 3, n); /* must not crash / OOB write */

  UT_ASSERT_EQ(seen[0], 3);

  tcc_free(seen);
  utb_free(ir);
  return 0;
}

UT_TEST(test_build_block_starts_bitmap_entry_target_and_fallthrough)
{
  TCCIRState *ir = utb_new();
  /*
   *  0: JUMPIF EQ -> 3    (marks 3 AND 1, the fallthrough-after-JUMPIF)
   *  1: RETURNVOID
   *  2: RETURNVOID
   *  3: RETURNVOID
   */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 1 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 3 */

  int n = ir->next_instruction_index;
  uint8_t *bs = ir_opt_build_block_starts_bitmap(ir, n);

  UT_ASSERT_EQ((bs[0 / 8] >> (0 % 8)) & 1, 1); /* entry always a start */
  UT_ASSERT_EQ((bs[1 / 8] >> (1 % 8)) & 1, 1); /* fallthrough right after JUMPIF */
  UT_ASSERT_EQ((bs[2 / 8] >> (2 % 8)) & 1, 0); /* neither a target nor a fallthrough-after-branch */
  UT_ASSERT_EQ((bs[3 / 8] >> (3 % 8)) & 1, 1); /* JUMPIF's own target */

  tcc_free(bs);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_next_non_nop / ir_skip_nops_forward
 * ========================================================================= */

UT_TEST(test_next_non_nop_skips_nops_and_returns_minus1_at_end)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);            /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);            /* 1 */
  int real = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);            /* 3 */

  UT_ASSERT_EQ(ir_opt_next_non_nop(ir, 0), real);
  UT_ASSERT_EQ(ir_opt_next_non_nop(ir, real), real); /* starting ON a non-nop returns itself */
  UT_ASSERT_EQ(ir_opt_next_non_nop(ir, real + 1), -1); /* only NOPs remain -> -1 */

  utb_free(ir);
  return 0;
}

UT_TEST(test_skip_nops_forward_returns_n_when_all_nops)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);  /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);  /* 1 */

  int n = ir->next_instruction_index;
  UT_ASSERT_EQ(ir_skip_nops_forward(ir, 0, n), n); /* sentinel: reached n without a non-NOP */

  utb_free(ir);
  return 0;
}

UT_TEST(test_skip_nops_forward_finds_first_non_nop)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);            /* 0 */
  int real = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* 1 */

  int n = ir->next_instruction_index;
  UT_ASSERT_EQ(ir_skip_nops_forward(ir, 0, n), real);

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_has_other_jump_to_fast
 * ========================================================================= */

UT_TEST(test_has_other_jump_to_fast_excludes_named_jump_and_counts_rest)
{
  TCCIRState *ir = utb_new();
  int j0 = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE); /* 0 -> 3 */
  int j1 = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE); /* 1 -> 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 3 */

  int n = ir->next_instruction_index;
  int *jt_cnt = (int *)tcc_mallocz(sizeof(int) * n);
  jt_cnt[3] = 2; /* two jumps (j0, j1) target block 3 */

  /* Excluding j0 still leaves j1 targeting 3 -> "other jump" exists. */
  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, 3, j0), 1);
  /* Excluding a non-jump instruction (2) doesn't decrement -> still 2 total -> true. */
  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, 3, 2), 1);

  tcc_free(jt_cnt);
  utb_free(ir);
  (void)j1;
  return 0;
}

UT_TEST(test_has_other_jump_to_fast_single_jump_excluded_leaves_none)
{
  TCCIRState *ir = utb_new();
  int j0 = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE); /* 0 -> 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 1 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 2 */

  int n = ir->next_instruction_index;
  int *jt_cnt = (int *)tcc_mallocz(sizeof(int) * n);
  jt_cnt[2] = 1; /* only j0 targets 2 */

  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, 2, j0), 0); /* excluding the only one leaves none */

  tcc_free(jt_cnt);
  utb_free(ir);
  return 0;
}

UT_TEST(test_has_other_jump_to_fast_target_out_of_range_or_zero_count)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* 0 */

  int n = ir->next_instruction_index;
  int *jt_cnt = (int *)tcc_mallocz(sizeof(int) * n);

  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, -1, -1), 0); /* target < 0 */
  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, n, -1), 0);  /* target >= n */
  UT_ASSERT_EQ(ir_has_other_jump_to_fast(ir, jt_cnt, 0, -1), 0);  /* jt_cnt[0]==0 */

  tcc_free(jt_cnt);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * tcc_ir_is_pure_aeabi / ir_opt_is_pure_helper_name /
 * ir_opt_is_readonly_str_helper_name / ir_opt_is_flag_cmp_helper_name
 * ========================================================================= */

UT_TEST(test_is_pure_aeabi_recognizes_categories_and_rejects_others)
{
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_lcmp"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_ulcmp"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_lmul"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_llsl"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_dadd"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_fcmpeq"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_f2d"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__bswapsi2"), 1);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__bswapdi3"), 1);

  /* Not pure: memcpy-like helpers, non-aeabi-prefixed names, and a
   * dunder-prefixed-but-unlisted aeabi name. */
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("__aeabi_memcpy"), 0);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("memcpy"), 0);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi(NULL), 0);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi(""), 0);
  UT_ASSERT_EQ(tcc_ir_is_pure_aeabi("_"), 0); /* only one leading underscore */
  return 0;
}

UT_TEST(test_is_pure_helper_name_isnan_and_narrow_family)
{
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("isnan"), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("__isnan"), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("__isnanf"), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("__aeabi_f2d"), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("__aeabi_d2f"), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name("__aeabi_dadd"), 0); /* pure-aeabi but not in THIS table */
  UT_ASSERT_EQ(ir_opt_is_pure_helper_name(NULL), 0);
  return 0;
}

UT_TEST(test_is_readonly_str_helper_name_table)
{
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strcmp"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strncmp"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strlen"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strnlen"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strchr"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strrchr"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strpbrk"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strstr"), 1);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strcspn"), 1);
  /* strcpy is NOT in this table -- it writes memory, so it must not be
   * treated as read-only-and-droppable. */
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("__tcc_strcpy"), 0);
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name("strlen"), 0); /* unprefixed libc name not matched */
  UT_ASSERT_EQ(ir_opt_is_readonly_str_helper_name(NULL), 0);
  return 0;
}

UT_TEST(test_is_flag_cmp_helper_name_table)
{
  UT_ASSERT_EQ(ir_opt_is_flag_cmp_helper_name("__aeabi_cfcmple"), 1);
  UT_ASSERT_EQ(ir_opt_is_flag_cmp_helper_name("__aeabi_cdcmple"), 1);
  UT_ASSERT_EQ(ir_opt_is_flag_cmp_helper_name("__aeabi_dcmple"), 0); /* different helper */
  UT_ASSERT_EQ(ir_opt_is_flag_cmp_helper_name(NULL), 0);
  return 0;
}

/* =========================================================================
 * ir_opt_is_pure_fallthrough_instruction
 * ========================================================================= */

UT_TEST(test_is_pure_fallthrough_instruction_simple_ops)
{
  TCCIRState *ir = utb_new();
  int nop = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  int orop = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int fpv = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I32),
                     utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));

  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, nop), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, asg), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, orop), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, fpv), 1);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, cmp), 0); /* CMP not in the allow-list */

  utb_free(ir);
  return 0;
}

/* POSITIVE: a FUNCCALLVAL to a name in ir_opt_is_pure_helper_name's table
 * (e.g. isnan) is itself a pure fallthrough instruction. */
UT_TEST(test_is_pure_fallthrough_instruction_pure_call_true)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 200);
  utb_set_tok_str(200, "isnan");

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, call), 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a FUNCCALLVAL to a name NOT in the pure-helper table is
 * not a pure fallthrough instruction. */
UT_TEST(test_is_pure_fallthrough_instruction_impure_call_false)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 201);
  utb_set_tok_str(201, "memcpy");

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, call), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_is_pure_fallthrough_instruction_bounds_and_null)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(NULL, 0), 0);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, -1), 0);
  UT_ASSERT_EQ(ir_opt_is_pure_fallthrough_instruction(ir, 99), 0);

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_nonvreg_expr_equal
 * ========================================================================= */

UT_TEST(test_nonvreg_expr_equal_stackoff_same_slot)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_stackoff(8, /*is_lval*/ 1, 0, 0, I32);
  IROperand b = utb_stackoff(8, 1, 0, 0, I32);
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_stackoff_different_offset)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_stackoff(8, 1, 0, 0, I32);
  IROperand b = utb_stackoff(12, 1, 0, 0, I32);
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_stackoff_different_lval_flag)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_stackoff(8, 1, 0, 0, I32);
  IROperand b = utb_stackoff(8, 0, 0, 0, I32); /* address-of vs deref */
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_different_tags_false)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_stackoff(8, 1, 0, 0, I32);
  IROperand b = utb_imm(8, I32); /* IROP_TAG_IMM32, not STACKOFF/SYMREF */
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_symref_same_sym_and_addend)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  memset(&s, 0, sizeof(s));
  s.v = 60;
  uint32_t idx1 = tcc_ir_pool_add_symref(ir, &s, /*addend*/ 4, 0);
  uint32_t idx2 = tcc_ir_pool_add_symref(ir, &s, /*addend*/ 4, 0);
  IROperand a = irop_make_symref(0, idx1, 1, 0, 0, I32);
  IROperand b = irop_make_symref(0, idx2, 1, 0, 0, I32);
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_symref_different_addend)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  memset(&s, 0, sizeof(s));
  s.v = 61;
  uint32_t idx1 = tcc_ir_pool_add_symref(ir, &s, 0, 0);
  uint32_t idx2 = tcc_ir_pool_add_symref(ir, &s, 4, 0);
  IROperand a = irop_make_symref(0, idx1, 1, 0, 0, I32);
  IROperand b = irop_make_symref(0, idx2, 1, 0, 0, I32);
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_nonvreg_expr_equal_symref_different_sym)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s1, s2;
  memset(&s1, 0, sizeof(s1));
  memset(&s2, 0, sizeof(s2));
  s1.v = 62;
  s2.v = 63;
  uint32_t idx1 = tcc_ir_pool_add_symref(ir, &s1, 0, 0);
  uint32_t idx2 = tcc_ir_pool_add_symref(ir, &s2, 0, 0);
  IROperand a = irop_make_symref(0, idx1, 1, 0, 0, I32);
  IROperand b = irop_make_symref(0, idx2, 1, 0, 0, I32);
  UT_ASSERT_EQ(ir_opt_nonvreg_expr_equal(ir, a, b), 0);
  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_pure_expr_equal (and transitively ir_opt_pure_def_equal)
 * ========================================================================= */

UT_TEST(test_pure_expr_equal_immediates)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(ir_opt_pure_expr_equal(ir, utb_imm(5, I32), 0, utb_imm(5, I32), 0, 0), 1);
  UT_ASSERT_EQ(ir_opt_pure_expr_equal(ir, utb_imm(5, I32), 0, utb_imm(6, I32), 0, 0), 0);
  /* one immediate, one not -> false regardless of value */
  UT_ASSERT_EQ(ir_opt_pure_expr_equal(ir, utb_imm(5, I32), 0, utb_temp(0, I32), 0, 0), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: same single-def source vreg read at two different use sites
 * (same underlying def instruction) is trivially equal. */
UT_TEST(test_pure_expr_equal_same_def_site_true)
{
  TCCIRState *ir = utb_new();
  int def = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                              /* 1 */
  int use1 = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);   /* 2 */

  int ok = ir_opt_pure_expr_equal(ir, utb_temp(0, I32), use1, utb_temp(0, I32), use1, 0);
  UT_ASSERT_EQ(ok, 1);
  (void)def;
  utb_free(ir);
  return 0;
}

/* POSITIVE: two structurally-identical single-def ADD chains (`T2=T0+T1`
 * computed twice from the same two source values) with no intervening
 * memory-changing op are recognized as value-equal via ir_opt_pure_def_equal
 * -> ir_opt_pure_expr_equal_impl's def comparison path. */
UT_TEST(test_pure_expr_equal_identical_add_defs_true)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);          /* 0: T0=3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(4, I32), UTB_NONE);          /* 1: T1=4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32)); /* 2: T2=T0+T1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32)); /* 3: T3=T0+T1 (same operands) */
  int use = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);  /* 4 */

  /* Both use-indices must be *after* the respective defs (index 4, the
   * RETURNVALUE site) -- tcc_ir_find_defining_instruction searches strictly
   * before use_idx, so passing T2's own def index (2) as its use_idx would
   * make the search never see index 2 itself and spuriously return -1. */
  int ok = ir_opt_pure_expr_equal(ir, utb_temp(2, I32), use, utb_temp(3, I32), use, 0);
  UT_ASSERT_EQ(ok, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a STORE between the two LOAD defs invalidates the
 * "memory stable" precondition -- two structurally-identical LOADs of the
 * same address must NOT be considered equal if a store could have changed
 * the value in between. */
UT_TEST(test_pure_expr_equal_load_with_intervening_store_false)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(1, I32)), utb_imm(99, I32), UTB_NONE);              /* 1: mutates V1 */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_var(1, I32)), UTB_NONE);  /* 2 */
  int use = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);               /* 3 */

  /* use-index must be after BOTH defs (index 3) so tcc_ir_find_defining_instruction
   * actually locates each LOAD's own def instead of bailing out with -1 --
   * otherwise this would spuriously "pass" via the a_def_idx<0 early-return
   * without ever reaching the memory-stability gate under test. */
  int ok = ir_opt_pure_expr_equal(ir, utb_temp(0, I32), use, utb_temp(2, I32), use, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: the SAME shape but with NO intervening store IS considered
 * equal -- isolates that the negative case above is specifically about the
 * intervening store, not about LOAD-vs-LOAD comparison being unsupported. */
UT_TEST(test_pure_expr_equal_load_without_intervening_store_true)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_var(1, I32)), UTB_NONE);  /* 1 */
  int use = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);               /* 2 */

  /* use-index must be after BOTH defs (index 2, the RETURNVALUE site) --
   * tcc_ir_find_defining_instruction searches strictly before use_idx, so
   * passing a LOAD's own def index as its use_idx would make the search
   * never see that instruction and spuriously return -1. */
  int ok = ir_opt_pure_expr_equal(ir, utb_temp(0, I32), use, utb_temp(2, I32), use, 0);
  UT_ASSERT_EQ(ok, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): an lval (dereferenced) operand and a plain (address)
 * operand referencing the same vreg definition are NOT equal -- one loads
 * from memory, the other is the address itself.  This is the exact
 * regression the comment above ir_opt_pure_expr_equal_impl's `is_lval`
 * guard documents (c->field0 + K vs &c->field0 + K). */
UT_TEST(test_pure_expr_equal_lval_vs_address_mismatch_false)
{
  TCCIRState *ir = utb_new();
  int def = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE); /* 0 */
  IROperand deref = utb_lval(utb_temp(0, I32));
  IROperand addr = utb_temp(0, I32); /* not is_lval */

  int ok = ir_opt_pure_expr_equal(ir, deref, def, addr, def, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): FUNCCALLVAL defs to an IMPURE callee are never
 * considered equal, even with identical arguments -- ir_opt_pure_def_equal's
 * FUNCCALLVAL case requires ir_opt_is_pure_helper_name(a_name). */
UT_TEST(test_pure_expr_equal_impure_call_defs_false)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 202);
  utb_set_tok_str(202, "memcpy");

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                              /* 0 */
  int call_a = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));                  /* 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));                              /* 2 */
  int call_b = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));                  /* 3 */

  int ok = ir_opt_pure_def_equal(ir, call_a, call_b, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: two FUNCCALLVAL defs to a PURE callee (isnan) with identical
 * constant arguments ARE value-equal. */
UT_TEST(test_pure_def_equal_pure_call_defs_identical_args_true)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 203);
  utb_set_tok_str(203, "isnan");

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                              /* 0 */
  int call_a = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));                  /* 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));                              /* 2 */
  int call_b = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));                  /* 3 */

  int ok = ir_opt_pure_def_equal(ir, call_a, call_b, 0);
  UT_ASSERT_EQ(ok, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): same pure callee, but argc differs -> not equal. */
UT_TEST(test_pure_def_equal_pure_call_defs_different_argc_false)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 204);
  utb_set_tok_str(204, "isnan");

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                              /* 0 */
  int call_a = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));                  /* 1: argc=1 */
  int call_b = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), fn,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 0), I32));                  /* 2: argc=0 */

  int ok = ir_opt_pure_def_equal(ir, call_a, call_b, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: MLA's src1*src2 is commutative -- swapped operand order still
 * compares equal (accum must match exactly). */
UT_TEST(test_pure_def_equal_mla_commutative_operands_true)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  int def_a = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32));
  int def_b = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I32), utb_temp(1, I32), utb_temp(0, I32), utb_temp(2, I32));

  int ok = ir_opt_pure_def_equal(ir, def_a, def_b, 0);
  UT_ASSERT_EQ(ok, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): MLA with a different accumulator is not equal even if
 * src1*src2 match. */
UT_TEST(test_pure_def_equal_mla_different_accum_false)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  int def_a = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32));
  int def_b = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I32), utb_temp(0, I32), utb_temp(1, I32), utb_imm(99, I32));

  int ok = ir_opt_pure_def_equal(ir, def_a, def_b, 0);
  UT_ASSERT_EQ(ok, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): mismatched opcodes at the two def sites are never equal
 * (ADD vs SUB), even with identical operands. */
UT_TEST(test_pure_def_equal_mismatched_opcode_false)
{
  TCCIRState *ir = utb_new();
  int def_a = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int def_b = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  UT_ASSERT_EQ(ir_opt_pure_def_equal(ir, def_a, def_b, 0), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): negative def indices always report false. */
UT_TEST(test_pure_def_equal_negative_index_false)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(ir_opt_pure_def_equal(ir, -1, 0, 0), 0);
  UT_ASSERT_EQ(ir_opt_pure_def_equal(ir, 0, -1, 0), 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): depth limit (>12) bails immediately regardless of def
 * shape. */
UT_TEST(test_pure_def_equal_depth_limit_false)
{
  TCCIRState *ir = utb_new();
  int def_a = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int def_b = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  UT_ASSERT_EQ(ir_opt_pure_def_equal(ir, def_a, def_b, 13), 0);

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_get_call_param_operand / ir_opt_get_call_param_index /
 * ir_opt_nop_call_params / ir_opt_nop_call_param / ir_opt_change_call_argc
 * ========================================================================= */

UT_TEST(test_get_call_param_operand_finds_matching_param)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 205);

  int p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(11, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(5, 0), I32));  /* 0: call_id=5, param 0 */
  int p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(22, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(5, 1), I32));  /* 1: call_id=5, param 1 */
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(5, 2), I32)); /* 2 */

  IROperand out;
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, call, 0, &out), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, out), 11);
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, call, 1, &out), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, out), 22);

  UT_ASSERT_EQ(ir_opt_get_call_param_index(ir, call, 0), p0);
  UT_ASSERT_EQ(ir_opt_get_call_param_index(ir, call, 1), p1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a param_idx with no matching FUNCPARAMVAL/VOID (wrong
 * index) is not found; ir_opt_get_call_param_index returns -1. */
UT_TEST(test_get_call_param_operand_missing_param_index_fails)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 206);

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(11, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(6, 0), I32));
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(6, 1), I32));

  IROperand out;
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, call, 3, &out), 0);
  UT_ASSERT_EQ(ir_opt_get_call_param_index(ir, call, 3), -1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a FUNCPARAMVAL belonging to a DIFFERENT call_id must not
 * be matched even if the param_idx coincides. */
UT_TEST(test_get_call_param_operand_different_call_id_not_matched)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 207);

  /* param belongs to call_id 1, but we query call_id 2's call instruction */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(11, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int call2 = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));

  IROperand out;
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, call2, 0, &out), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): invalid call_idx (out of range, or pointing at a
 * non-call instruction) fails cleanly. */
UT_TEST(test_get_call_param_operand_invalid_call_idx)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE); /* 0: not a call */

  IROperand out;
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(NULL, 0, 0, &out), 0);
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, -1, 0, &out), 0);
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, 99, 0, &out), 0);
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, 0, 0, &out), 0); /* NOP, not FUNCCALL* */
  UT_ASSERT_EQ(ir_opt_get_call_param_operand(ir, 0, 0, NULL), 0); /* NULL out */

  UT_ASSERT_EQ(ir_opt_get_call_param_index(NULL, 0, 0), -1);
  UT_ASSERT_EQ(ir_opt_get_call_param_index(ir, -1, 0), -1);
  UT_ASSERT_EQ(ir_opt_get_call_param_index(ir, 99, 0), -1);

  utb_free(ir);
  return 0;
}

/* POSITIVE: ir_opt_nop_call_params NOPs every FUNCPARAMVAL/VOID for a given
 * call_id but leaves other calls' params untouched. */
UT_TEST(test_nop_call_params_nops_only_matching_call_id)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 208);

  int p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));  /* call 1 */
  int p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(2, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));  /* call 1 */
  int other = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(3, I32),
                       utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32)); /* call 2 (unrelated) */
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));

  ir_opt_nop_call_params(ir, call);

  UT_ASSERT_EQ(utb_op(ir, p0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, p1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, other), TCCIR_OP_FUNCPARAMVAL); /* different call_id: untouched */

  utb_free(ir);
  return 0;
}

/* POSITIVE: ir_opt_nop_call_param NOPs ONLY the one matching (call_id,
 * param_idx) pair, leaving sibling params of the same call intact. */
UT_TEST(test_nop_call_param_nops_only_matching_param_idx)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 209);

  int p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(2, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));

  ir_opt_nop_call_param(ir, call, 1);

  UT_ASSERT_EQ(utb_op(ir, p0), TCCIR_OP_FUNCPARAMVAL); /* param 0: untouched */
  UT_ASSERT_EQ(utb_op(ir, p1), TCCIR_OP_NOP);           /* param 1: nopped */

  utb_free(ir);
  return 0;
}

/* POSITIVE: ir_opt_change_call_argc rewrites the call's src2 argc field
 * while preserving the call_id. */
UT_TEST(test_change_call_argc_updates_argc_preserves_call_id)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 210);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(9, 3), I32));

  ir_opt_change_call_argc(ir, call, 1);

  IROperand src2 = utb_src2(ir, call);
  uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID(encoded), 9);   /* call_id preserved */
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ARGC(encoded), 1); /* argc updated */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): out-of-range / non-call instr_idx is a silent no-op for
 * the void-returning helpers (must not crash). */
UT_TEST(test_call_param_void_helpers_out_of_range_no_crash)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);

  ir_opt_nop_call_params(NULL, 0);
  ir_opt_nop_call_params(ir, -1);
  ir_opt_nop_call_params(ir, 99);
  ir_opt_nop_call_params(ir, 0); /* NOP, not a call: op check fails, no-op */

  ir_opt_nop_call_param(NULL, 0, 0);
  ir_opt_nop_call_param(ir, -1, 0);
  ir_opt_nop_call_param(ir, 99, 0);
  ir_opt_nop_call_param(ir, 0, 0);

  ir_opt_change_call_argc(NULL, 0, 1);
  ir_opt_change_call_argc(ir, -1, 1);
  ir_opt_change_call_argc(ir, 99, 1);
  ir_opt_change_call_argc(ir, 0, 1); /* NOP: op check fails, no-op */

  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP); /* untouched throughout */

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * ir_opt_vreg_address_taken_between
 * ========================================================================= */

UT_TEST(test_vreg_address_taken_between_lea_detected)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);     /* 1: &V0 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 2 */

  int32_t vr = irop_get_vreg(utb_var(0, I32));
  UT_ASSERT_EQ(ir_opt_vreg_address_taken_between(ir, vr, 0, 2), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_vreg_address_taken_between_no_lea_returns_0)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(1, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 2 */

  int32_t vr = irop_get_vreg(utb_var(0, I32));
  UT_ASSERT_EQ(ir_opt_vreg_address_taken_between(ir, vr, 0, 2), 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a LEA of a DIFFERENT vreg doesn't count. */
UT_TEST(test_vreg_address_taken_between_different_vreg_not_counted)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_var(5, I32), UTB_NONE);     /* 1: &V5, not V0 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 2 */

  int32_t vr = irop_get_vreg(utb_var(0, I32));
  UT_ASSERT_EQ(ir_opt_vreg_address_taken_between(ir, vr, 0, 2), 0);
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a LEA OUTSIDE the (start_idx, end_idx) open interval
 * (at or before start_idx, or at/after end_idx) is not counted -- the scan
 * is strictly `start_idx+1 .. end_idx-1`. */
UT_TEST(test_vreg_address_taken_between_outside_window_not_counted)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);     /* 0: at start_idx itself */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32), UTB_NONE);  /* 1 */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(3, I32), utb_var(0, I32), UTB_NONE);     /* 2: at end_idx itself */

  int32_t vr = irop_get_vreg(utb_var(0, I32));
  UT_ASSERT_EQ(ir_opt_vreg_address_taken_between(ir, vr, 0, 2), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_vreg_address_taken_between_null_ir_returns_0)
{
  UT_ASSERT_EQ(ir_opt_vreg_address_taken_between(NULL, 0, 0, 10), 0);
  return 0;
}

/* =========================================================================
 * ir_opt_get_constant_string_from_symref
 *
 * NOTE ON TESTABILITY: every non-degenerate path requires elfsym(sym) to
 * return a real ElfSym with a valid, allocated, read-only section
 * (SHF_ALLOC, not SHF_WRITE) containing the string bytes.  The shared
 * elfsym() stub in stubs.c (tests/unit/README.md-documented, reused by
 * several other suites -- see plan_ut_next_steps.md's `global_base_share`
 * write-up for the identical limitation) unconditionally returns NULL.
 * So only the "no ELF state" early-out is reachable from this harness; the
 * positive (string actually extracted) path needs fake-ELF-section stub
 * infrastructure that doesn't exist yet, same gap already logged for
 * ir_opt_global_base_share.
 * ========================================================================= */

UT_TEST(test_get_constant_string_from_symref_no_elf_state_returns_null)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym s;
  memset(&s, 0, sizeof(s));
  s.v = 90;
  uint32_t idx = tcc_ir_pool_add_symref(ir, &s, /*addend*/ 0, 0);
  IROperand op = irop_make_symref(0, idx, 0, 0, 1, I32);

  const char *out = ir_opt_get_constant_string_from_symref(ir, op);
  UT_ASSERT(out == NULL);

  utb_free(ir);
  return 0;
}

UT_TEST(test_get_constant_string_from_symref_non_symref_tag_returns_null)
{
  TCCIRState *ir = utb_new();
  const char *out = ir_opt_get_constant_string_from_symref(ir, utb_imm(5, I32));
  UT_ASSERT(out == NULL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_get_constant_string_from_symref_null_ir_returns_null)
{
  UT_ASSERT(ir_opt_get_constant_string_from_symref(NULL, utb_imm(5, I32)) == NULL);
  return 0;
}

/* Negative-addend symref (LVAL flag set / addend<0 guard): the function
 * bails before ever reaching elfsym() when addend<0 or the LVAL flag is set
 * on the symref pool entry itself. */
UT_TEST(test_get_constant_string_from_symref_negative_addend_returns_null)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym s;
  memset(&s, 0, sizeof(s));
  s.v = 91;
  uint32_t idx = tcc_ir_pool_add_symref(ir, &s, /*addend*/ -1, 0);
  IROperand op = irop_make_symref(0, idx, 0, 0, 1, I32);

  UT_ASSERT(ir_opt_get_constant_string_from_symref(ir, op) == NULL);

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * change_callee_sym / change_callee_sym_keep_type
 *
 * NOTE ON TESTABILITY: both call sym_push2() / external_global_sym(), which
 * are link stubs in stubs.c that unconditionally return NULL (documented at
 * their definition and already noted by test_opt_constfold.c's
 * float_narrowing / const_string_calls suspected-bug writeups for the SAME
 * root cause).  change_callee_sym() NULL-checks sym_push2()'s result
 * (if (!ftype.ref) return 0;) so its "always returns 0 under this stub"
 * behavior IS itself a deterministic, sound oracle -- assert it explicitly
 * here so a change to that guard (e.g. losing the NULL check) is caught.
 * change_callee_sym_keep_type() additionally requires entry->sym to be
 * non-NULL, which our hand-built symref satisfies, but it too always
 * returns 0 here because external_global_sym() returns NULL.
 * ========================================================================= */

UT_TEST(test_change_callee_sym_no_symtab_stub_returns_0_documents_current_behavior)
{
  /* documents current (stub-environment) behavior, see docs/bugs.md-style
   * note above -- NOT a claim that change_callee_sym is unreachable in a
   * real compilation (there sym_push2/external_global_sym succeed). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 211);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changed = change_callee_sym(ir, call, "__new_helper", IROP_BTYPE_INT32);
  UT_ASSERT_EQ(changed, 0);
  /* the callee symref is left as-is (entry->sym unmodified) since the stub
   * bails before ever assigning entry->sym = new_sym. */
  IROperand src1 = utb_src1(ir, call);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  UT_ASSERT(entry->sym == &callee);

  utb_free(ir);
  return 0;
}

UT_TEST(test_change_callee_sym_keep_type_no_symtab_stub_returns_0)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 212);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changed = change_callee_sym_keep_type(ir, call, "__new_helper2");
  UT_ASSERT_EQ(changed, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): change_callee_sym_keep_type with a NULL entry->sym
 * bails before ever calling external_global_sym. */
UT_TEST(test_change_callee_sym_keep_type_null_sym_returns_0)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  uint32_t idx = tcc_ir_pool_add_symref(ir, NULL, 0, 0);
  IROperand fn = irop_make_symref(0, idx, 0, 0, 0, I32);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changed = change_callee_sym_keep_type(ir, call, "whatever");
  UT_ASSERT_EQ(changed, 0);

  utb_free(ir);
  return 0;
}

/* =========================================================================
 * tcc_ir_vreg_has_single_def
 * ========================================================================= */

UT_TEST(test_vreg_has_single_def_true_for_exactly_one_def)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32), UTB_NONE); /* different vreg */

  int32_t vr0 = irop_get_vreg(utb_temp(0, I32));
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_def(ir, vr0), 1);

  utb_free(ir);
  return 0;
}

UT_TEST(test_vreg_has_single_def_false_for_multiple_defs)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32), UTB_NONE);

  int32_t vr0 = irop_get_vreg(utb_temp(0, I32));
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_def(ir, vr0), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a vreg with ZERO defs also reports "not single-def"
 * (def_count==0 != 1) -- distinguishes "exactly one" from "at most one". */
UT_TEST(test_vreg_has_single_def_false_for_zero_defs)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int32_t vr_never_defined = irop_get_vreg(utb_temp(9, I32));
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_def(ir, vr_never_defined), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): NOP instructions are skipped even though NOP's
 * irop_config has no dest anyway -- verifies the early `continue` doesn't
 * accidentally count a stale/garbage dest from an unrelated opcode slot. */
UT_TEST(test_vreg_has_single_def_skips_nops)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);

  int32_t vr0 = irop_get_vreg(utb_temp(0, I32));
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_def(ir, vr0), 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): CMP has no dest (irop_config[CMP].has_dest==0) so it can
 * never contribute a def, even though it "writes" flags conceptually. */
UT_TEST(test_vreg_has_single_def_ops_without_dest_dont_count)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));

  int32_t vr0 = irop_get_vreg(utb_temp(0, I32));
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_def(ir, vr0), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_utils)
{
  /* pass-disabled */
  UT_RUN(test_pass_disabled_unset_env_never_disables);
  UT_RUN(test_pass_disabled_null_name_returns_0);

  /* is_power_of_2 */
  UT_RUN(test_is_power_of_2_positive_powers);
  UT_RUN(test_is_power_of_2_non_powers_and_nonpositive);

  /* evaluate_compare_condition */
  UT_RUN(test_evaluate_compare_condition_signed_tokens);
  UT_RUN(test_evaluate_compare_condition_unsigned_tokens_treat_negative_as_huge);
  UT_RUN(test_evaluate_compare_condition_unknown_token_returns_minus1);

  /* ir_opt_eval_const_u64 */
  UT_RUN(test_eval_const_u64_immediate);
  UT_RUN(test_eval_const_u64_add_chain_folds);
  UT_RUN(test_eval_const_u64_shr_uses_32bit_width_for_int32_operand);
  UT_RUN(test_eval_const_u64_ror_rotates_32bit);
  UT_RUN(test_eval_const_u64_zext_masks_to_source_width);
  UT_RUN(test_eval_const_u64_multi_def_vreg_bails_out);
  UT_RUN(test_eval_const_u64_address_taken_between_bails_out);
  UT_RUN(test_eval_const_u64_depth_limit_bails_out);
  UT_RUN(test_eval_const_u64_null_args);

  /* ir_opt_eval_const_string */
  UT_RUN(test_eval_const_string_lval_temp_rejected);
  UT_RUN(test_eval_const_string_no_def_fails);
  UT_RUN(test_eval_const_string_null_args);
  UT_RUN(test_eval_const_string_depth_limit_bails_out);

  /* condition token helpers */
  UT_RUN(test_vrp_negate_cmp_tok_all_pairs);
  UT_RUN(test_vrp_swap_cmp_tok_all_pairs);
  UT_RUN(test_vrp_cmp_implies_reflexive_and_families);
  UT_RUN(test_fcmp_cmp_implies_families);
  UT_RUN(test_invert_cond_token_all_pairs);
  UT_RUN(test_invert_condition_all_pairs);
  UT_RUN(test_ir_negate_condition_xor_1);

  /* BB/CFG helpers */
  UT_RUN(test_build_merge_bitmap_no_merge_when_every_block_has_one_pred);
  UT_RUN(test_build_merge_bitmap_multiple_preds_sets_bit);
  UT_RUN(test_build_merge_bitmap_backward_edge_sets_bit);
  UT_RUN(test_mark_block_starts_marks_jump_targets_and_entry);
  UT_RUN(test_mark_block_starts_out_of_range_target_ignored);
  UT_RUN(test_build_block_starts_bitmap_entry_target_and_fallthrough);
  UT_RUN(test_next_non_nop_skips_nops_and_returns_minus1_at_end);
  UT_RUN(test_skip_nops_forward_returns_n_when_all_nops);
  UT_RUN(test_skip_nops_forward_finds_first_non_nop);
  UT_RUN(test_has_other_jump_to_fast_excludes_named_jump_and_counts_rest);
  UT_RUN(test_has_other_jump_to_fast_single_jump_excluded_leaves_none);
  UT_RUN(test_has_other_jump_to_fast_target_out_of_range_or_zero_count);

  /* purity tables */
  UT_RUN(test_is_pure_aeabi_recognizes_categories_and_rejects_others);
  UT_RUN(test_is_pure_helper_name_isnan_and_narrow_family);
  UT_RUN(test_is_readonly_str_helper_name_table);
  UT_RUN(test_is_flag_cmp_helper_name_table);
  UT_RUN(test_is_pure_fallthrough_instruction_simple_ops);
  UT_RUN(test_is_pure_fallthrough_instruction_pure_call_true);
  UT_RUN(test_is_pure_fallthrough_instruction_impure_call_false);
  UT_RUN(test_is_pure_fallthrough_instruction_bounds_and_null);

  /* expression equality */
  UT_RUN(test_nonvreg_expr_equal_stackoff_same_slot);
  UT_RUN(test_nonvreg_expr_equal_stackoff_different_offset);
  UT_RUN(test_nonvreg_expr_equal_stackoff_different_lval_flag);
  UT_RUN(test_nonvreg_expr_equal_different_tags_false);
  UT_RUN(test_nonvreg_expr_equal_symref_same_sym_and_addend);
  UT_RUN(test_nonvreg_expr_equal_symref_different_addend);
  UT_RUN(test_nonvreg_expr_equal_symref_different_sym);
  UT_RUN(test_pure_expr_equal_immediates);
  UT_RUN(test_pure_expr_equal_same_def_site_true);
  UT_RUN(test_pure_expr_equal_identical_add_defs_true);
  UT_RUN(test_pure_expr_equal_load_with_intervening_store_false);
  UT_RUN(test_pure_expr_equal_load_without_intervening_store_true);
  UT_RUN(test_pure_expr_equal_lval_vs_address_mismatch_false);
  UT_RUN(test_pure_expr_equal_impure_call_defs_false);
  UT_RUN(test_pure_def_equal_pure_call_defs_identical_args_true);
  UT_RUN(test_pure_def_equal_pure_call_defs_different_argc_false);
  UT_RUN(test_pure_def_equal_mla_commutative_operands_true);
  UT_RUN(test_pure_def_equal_mla_different_accum_false);
  UT_RUN(test_pure_def_equal_mismatched_opcode_false);
  UT_RUN(test_pure_def_equal_negative_index_false);
  UT_RUN(test_pure_def_equal_depth_limit_false);

  /* call-param helpers */
  UT_RUN(test_get_call_param_operand_finds_matching_param);
  UT_RUN(test_get_call_param_operand_missing_param_index_fails);
  UT_RUN(test_get_call_param_operand_different_call_id_not_matched);
  UT_RUN(test_get_call_param_operand_invalid_call_idx);
  UT_RUN(test_nop_call_params_nops_only_matching_call_id);
  UT_RUN(test_nop_call_param_nops_only_matching_param_idx);
  UT_RUN(test_change_call_argc_updates_argc_preserves_call_id);
  UT_RUN(test_call_param_void_helpers_out_of_range_no_crash);

  /* misc helpers */
  UT_RUN(test_vreg_address_taken_between_lea_detected);
  UT_RUN(test_vreg_address_taken_between_no_lea_returns_0);
  UT_RUN(test_vreg_address_taken_between_different_vreg_not_counted);
  UT_RUN(test_vreg_address_taken_between_outside_window_not_counted);
  UT_RUN(test_vreg_address_taken_between_null_ir_returns_0);
  UT_RUN(test_get_constant_string_from_symref_no_elf_state_returns_null);
  UT_RUN(test_get_constant_string_from_symref_non_symref_tag_returns_null);
  UT_RUN(test_get_constant_string_from_symref_null_ir_returns_null);
  UT_RUN(test_get_constant_string_from_symref_negative_addend_returns_null);

  /* callee symbol replacement */
  UT_RUN(test_change_callee_sym_no_symtab_stub_returns_0_documents_current_behavior);
  UT_RUN(test_change_callee_sym_keep_type_no_symtab_stub_returns_0);
  UT_RUN(test_change_callee_sym_keep_type_null_sym_returns_0);

  /* tcc_ir_vreg_has_single_def */
  UT_RUN(test_vreg_has_single_def_true_for_exactly_one_def);
  UT_RUN(test_vreg_has_single_def_false_for_multiple_defs);
  UT_RUN(test_vreg_has_single_def_false_for_zero_defs);
  UT_RUN(test_vreg_has_single_def_skips_nops);
  UT_RUN(test_vreg_has_single_def_ops_without_dest_dont_count);
}
