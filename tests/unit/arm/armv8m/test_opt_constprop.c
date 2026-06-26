/*
 *  test_opt_constprop.c - suite for ir/opt_constprop.c (constant propagation)
 *
 *  Covers TWO entry points from the same TU:
 *
 *  1. tcc_ir_opt_const_var_prop — finds VAR vregs assigned exactly once with an
 *     immediate (`ASSIGN Vp <- #k`, def_count==1, not addrtaken) and rewrites
 *     later src1/src2 uses of that VAR with the immediate.  When the only uses
 *     are rewritten, the defining ASSIGN is NOP-ed (Phase 3 dead-store cleanup);
 *     a LOAD whose address operand folds to a constant becomes an ASSIGN.
 *
 *  2. tcc_ir_opt_const_prop — folds constants into arithmetic and compares:
 *     `T0 = #5 ADD #3` collapses to `T0 = ASSIGN #8`, a single-def immediate VAR
 *     propagates into a use, and one-constant algebraic identities (X+0 -> X,
 *     X*0 -> 0) simplify.  Non-constant operands are left alone.
 *
 *  Key behaviours / guards verified here:
 *    - const_var_prop POSITIVE: a single-def immediate VAR folds into a later
 *      arithmetic use (src operand becomes the immediate) and, with no other
 *      uses, the def is NOP-ed -> changes > 0.
 *    - const_var_prop POSITIVE: LOAD of a constant VAR address flips to ASSIGN.
 *    - const_var_prop GUARD: a VAR whose interval->addrtaken is set AND whose
 *      address is taken by a *live* LEA must NOT propagate.  changes == 0.
 *    - const_var_prop NEGATIVE: multiply-defined / non-immediate-source VARs are
 *      not constant and are not propagated.  changes == 0.
 *    - const_prop POSITIVE: two-constant fold of an ADD into a single ASSIGN
 *      (no VAR dests -> needs no live intervals).
 *    - const_prop POSITIVE: single-def immediate VAR propagated into an ADD and
 *      then constant-folded to ASSIGN.
 *    - const_prop POSITIVE: X + 0 -> X algebraic simplify (ADD becomes ASSIGN).
 *    - const_prop NEGATIVE: an ADD of two non-constant TEMPs is not folded.
 *
 *  Both passes call tcc_ir_get_live_interval() for every VAR destination, which
 *  exit(1)s when ir->variables_live_intervals is NULL/zero-sized (utb_new()
 *  leaves it so).  Tests that emit VAR destinations therefore allocate a zeroed
 *  interval table first; that table is also where the addrtaken guard reads.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

#include <limits.h>

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_const_var_prop(TCCIRState *ir);
int tcc_ir_opt_const_prop(TCCIRState *ir);
int tcc_ir_opt_global_init_prop(TCCIRState *ir);
int tcc_ir_opt_symref_const_prop(TCCIRState *ir);
int tcc_ir_opt_complex_const_param_fold(TCCIRState *ir);
int tcc_ir_opt_value_tracking(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define F32 IROP_BTYPE_FLOAT32

/* Encoded vreg helpers for assertions. */
#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))

/* Both passes dereference ir->variables_live_intervals[pos] for every VAR
 * destination they see.  utb_new() zeroes that pointer/size, which would make
 * tcc_ir_get_live_interval() report "out of bounds" and exit(1).  Allocate a
 * zeroed interval table large enough for all VAR positions a test uses. */
static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* ============================================================= const_var_prop */

/* POSITIVE: a single-def immediate VAR folds into a later ADD use.
 *   V0 <- #5            [constant def]
 *   T0 = V0 ADD #3      -> src1 rewritten to #5
 * V0 then has no remaining uses, so the def ASSIGN is NOP-ed (Phase 3).
 * changes > 0; the ADD's src1 becomes the immediate 5. */
UT_TEST(test_constvarprop_imm_var_folds_into_use)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  int idef = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);

  UT_ASSERT(changes > 0);

  /* The ADD's src1 is now the immediate 5 (no longer a VAR reference). */
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 5);

  /* With the only use rewritten, the defining ASSIGN is dead and NOP-ed. */
  UT_ASSERT_EQ(utb_op(ir, idef), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* POSITIVE (LOAD -> ASSIGN rewrite): a LOAD whose address operand is a constant
 * VAR folds: src1 becomes the immediate AND the op flips LOAD -> ASSIGN, because
 * the local's address now resolves to a known constant value.
 *   V0 <- #7
 *   T0 = LOAD V0    -> T0 = ASSIGN #7 */
UT_TEST(test_constvarprop_load_of_const_var_becomes_assign)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32), UTB_NONE);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_var_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iload);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 7);

  utb_free(ir);
  return 0;
}

/* GUARD (address-taken): V0 is single-def immediate, BUT its address is taken by
 * a live LEA and interval->addrtaken is set, so the value can be mutated through
 * the alias.  The pass must NOT propagate V0 into the later use.
 *
 *   V0 <- #5
 *   V1 = &V0          [LEA: address of V0 taken]
 *   T0 = V1 ADD #1    [reads V1 -> the LEA is "live", so refresh keeps addrtaken]
 *   T1 = V0 ADD #9    [use of V0 that must remain a VAR reference]
 *
 * Without a live LEA, refresh_stale_var_addrtaken() would clear addrtaken and
 * the value would propagate; the live LEA + addrtaken flag is what blocks it. */
UT_TEST(test_constvarprop_addrtaken_var_not_propagated)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  /* Mark V0's address as taken (frontend would set this for `&v`). */
  ir->variables_live_intervals[0].addrtaken = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LEA, utb_var(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(9, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  /* The use of V0 is untouched: src1 still references VAR 0, not an immediate. */
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ(irop_is_immediate(s1), 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_VAR(0));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (multiply defined): a VAR assigned an immediate twice is not a single
 * constant (def_count > 1 -> is_constant cleared), so it is not propagated.
 *   V0 <- #5
 *   V0 <- #6
 *   T0 = V0 ADD #1   -> NOT rewritten (still V0) */
UT_TEST(test_constvarprop_multiply_defined_not_propagated)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(6, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ(irop_is_immediate(s1), 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_VAR(0));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (non-immediate source): a VAR assigned from another vreg (not an
 * immediate, not a symref) is not constant, so it is not propagated.
 *   V0 <- T9          (source is a TEMP, not a constant)
 *   T0 = V0 ADD #1    -> NOT rewritten (still V0) */
UT_TEST(test_constvarprop_nonconst_source_not_propagated)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(9, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ(irop_is_immediate(s1), 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_VAR(0));

  utb_free(ir);
  return 0;
}

/* ================================================================= const_prop */

/* POSITIVE (two-constant fold): const_prop folds an arithmetic op whose both
 * operands are immediates into a single ASSIGN of the computed value.
 *   T0 = #5 ADD #3   ->  T0 = ASSIGN #8   (src2 cleared)
 * No VAR destinations exist, so no live-interval table is needed. */
UT_TEST(test_constprop_two_const_add_folds)
{
  TCCIRState *ir = utb_new();

  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(5, I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT(changes > 0);
  /* The ADD collapses to an ASSIGN of the constant result. */
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 8);

  utb_free(ir);
  return 0;
}

/* POSITIVE (two-constant fold, MUL): demonstrates folding is not ADD-specific.
 *   T0 = #6 MUL #7   ->  T0 = ASSIGN #42 */
UT_TEST(test_constprop_two_const_mul_folds)
{
  TCCIRState *ir = utb_new();

  int imul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_imm(6, I32), utb_imm(7, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imul), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, imul);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 42);

  utb_free(ir);
  return 0;
}

/* POSITIVE (VAR const propagated then folded): const_prop first propagates a
 * single-def immediate VAR into the ADD's src1, then folds the now all-constant
 * ADD into an ASSIGN.
 *   V0 <- #5
 *   T0 = V0 ADD #3   ->  T0 = ASSIGN #8 */
UT_TEST(test_constprop_var_const_propagated_and_folded)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 8);

  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic simplify): X + 0 = X.  With a non-constant src1 and a
 * constant 0 in src2, const_prop converts the ADD into an ASSIGN that copies
 * src1 unchanged (the non-constant operand is preserved, src2 cleared).
 *   T0 = T1 ADD #0   ->  T0 = ASSIGN T1 */
UT_TEST(test_constprop_add_zero_simplifies_to_copy)
{
  TCCIRState *ir = utb_new();

  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT(changes > 0);
  /* Becomes a plain copy of the non-constant src1. */
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic simplify): X * 0 = 0.  const_prop replaces the whole op
 * with an ASSIGN of constant 0, even though src1 is non-constant.
 *   T0 = T1 MUL #0   ->  T0 = ASSIGN #0 */
UT_TEST(test_constprop_mul_zero_simplifies_to_zero)
{
  TCCIRState *ir = utb_new();

  int imul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imul), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, imul);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: an ADD of two non-constant TEMPs has nothing to fold or simplify
 * (neither operand is an immediate, no identity applies) -> no change, the op
 * stays an ADD with both register operands intact.
 *   T0 = T1 ADD T2   ->  unchanged */
UT_TEST(test_constprop_two_nonconst_not_folded)
{
  TCCIRState *ir = utb_new();

  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_temp(2, I32));

  int changes = tcc_ir_opt_const_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, iadd)), VR_TMP(2));

  utb_free(ir);
  return 0;
}

/* Helper for 64-bit immediates that don't fit in int32_t. */
static IROperand utb_imm64(TCCIRState *ir, int64_t val, int btype)
{
  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
  return irop_make_i64(-1, pool_idx, btype);
}

/* ================================================= more const_var_prop tests */

/* IDEMPOTENCE: const_var_prop reaches a fixpoint in one iteration; a second
 * run makes no further changes. */
UT_TEST(test_constvarprop_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  int idef = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(3, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_const_var_prop, 5);
  UT_ASSERT(total > 0);
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 5);
  UT_ASSERT_EQ(utb_op(ir, idef), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a large (ARM pool-load) immediate assigned to a VAR is still
 * propagated when it has only a single use. */
UT_TEST(test_constvarprop_large_imm_single_use)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  int idef = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0x12345678, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);
  UT_ASSERT(changes > 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 0x12345678);
  UT_ASSERT_EQ(utb_op(ir, idef), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a VAR whose source is a stack-offset operand (not an immediate/symref)
 * is not treated as constant. */
UT_TEST(test_constvarprop_stackoff_source_not_propagated)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  IROperand slot = utb_stackoff(0, 0, 0, 0, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), slot, UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_var_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* ==================================================== more const_prop tests */

/* POSITIVE (algebraic): X - 0 -> X. */
UT_TEST(test_constprop_sub_zero_identity)
{
  TCCIRState *ir = utb_new();
  int isub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, isub), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, isub)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X | 0 -> X (commutative). */
UT_TEST(test_constprop_or_zero_identity)
{
  TCCIRState *ir = utb_new();
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(0, I32), utb_imm(0, I32), utb_temp(1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ior)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X & -1 -> X. */
UT_TEST(test_constprop_and_minusone_identity)
{
  TCCIRState *ir = utb_new();
  int iand = utb_emit(ir, TCCIR_OP_AND, utb_temp(0, I32), utb_temp(1, I32), utb_imm(-1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iand), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iand)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X | -1 -> -1. */
UT_TEST(test_constprop_or_minusone_to_const)
{
  TCCIRState *ir = utb_new();
  int ior = utb_emit(ir, TCCIR_OP_OR, utb_temp(0, I32), utb_temp(1, I32), utb_imm(-1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ior)), -1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X & 0 -> 0. */
UT_TEST(test_constprop_and_zero_to_zero)
{
  TCCIRState *ir = utb_new();
  int iand = utb_emit(ir, TCCIR_OP_AND, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iand), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iand)), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X ^ 0 -> X. */
UT_TEST(test_constprop_xor_zero_identity)
{
  TCCIRState *ir = utb_new();
  int ixor = utb_emit(ir, TCCIR_OP_XOR, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ixor), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ixor)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (constant fold): X ^ X -> 0 for a constant X. */
UT_TEST(test_constprop_xor_same_const_to_zero)
{
  TCCIRState *ir = utb_new();
  int ixor = utb_emit(ir, TCCIR_OP_XOR, utb_temp(0, I32), utb_imm(0xAA, I32), utb_imm(0xAA, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ixor), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ixor)), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (algebraic): X * 1 -> X. */
UT_TEST(test_constprop_mul_one_identity)
{
  TCCIRState *ir = utb_new();
  int imul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_temp(1, I32), utb_imm(1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imul), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, imul)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (constant fold): X - X -> 0 for a constant X. */
UT_TEST(test_constprop_sub_same_const_to_zero)
{
  TCCIRState *ir = utb_new();
  int isub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(0, I32), utb_imm(7, I32), utb_imm(7, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, isub), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, isub)), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (constant fold): X * 2^k computed as two-constant MUL. */
UT_TEST(test_constprop_mul_pow2_const)
{
  TCCIRState *ir = utb_new();
  int imul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_imm(3, I32), utb_imm(8, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imul), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, imul)), 24);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: INT_MAX + 1 wraps to INT_MIN in 32-bit two's complement. */
UT_TEST(test_constprop_intmax_plus_one_wraps)
{
  TCCIRState *ir = utb_new();
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(INT_MAX, I32), utb_imm(1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  int32_t expected = (int32_t)((uint32_t)INT_MAX + 1u);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iadd)), (int)expected);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: INT_MIN - 1 wraps to INT_MAX in 32-bit two's complement. */
UT_TEST(test_constprop_intmin_minus_one_wraps)
{
  TCCIRState *ir = utb_new();
  int isub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(0, I32), utb_imm(INT_MIN, I32), utb_imm(1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, isub), TCCIR_OP_ASSIGN);
  int32_t expected = (int32_t)((uint32_t)INT_MIN - 1u);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, isub)), (int)expected);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: sign-bit addition wraps to 0. */
UT_TEST(test_constprop_overflow_signbit_add_wraps)
{
  TCCIRState *ir = utb_new();
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(0x80000000, I32), utb_imm(0x80000000, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iadd)), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: shift by 0 is an identity (SHL). */
UT_TEST(test_constprop_shl_zero_identity)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ish)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: 1 << 31 = 0x80000000. */
UT_TEST(test_constprop_shl_31)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I32), utb_imm(1, I32), utb_imm(31, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ish)), (int)0x80000000);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* GUARD: 1 << 32 is not folded (shift width >= width). */
UT_TEST(test_constprop_shl_32_bails)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I32), utb_imm(1, I32), utb_imm(32, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: logical shift right by 31 of sign-bit value yields 1. */
UT_TEST(test_constprop_shr_31)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHR, utb_temp(0, I32), utb_imm(0x80000000, I32), utb_imm(31, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ish)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: arithmetic shift right by 31 of negative value yields -1. */
UT_TEST(test_constprop_sar_31)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SAR, utb_temp(0, I32), utb_imm(0x80000000, I32), utb_imm(31, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ish)), -1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: signed division rounds toward zero. */
UT_TEST(test_constprop_signed_div)
{
  TCCIRState *ir = utb_new();
  int idiv = utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), utb_imm(-5, I32), utb_imm(2, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, idiv), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, idiv)), -2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: signed modulo. */
UT_TEST(test_constprop_signed_mod)
{
  TCCIRState *ir = utb_new();
  int imod = utb_emit(ir, TCCIR_OP_IMOD, utb_temp(0, I32), utb_imm(-5, I32), utb_imm(2, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imod), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, imod)), -1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: unsigned division uses modulo 2^32. */
UT_TEST(test_constprop_unsigned_div)
{
  TCCIRState *ir = utb_new();
  int idiv = utb_emit(ir, TCCIR_OP_UDIV, utb_temp(0, I32), utb_unsigned(utb_imm(-5, I32)), utb_imm(2, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, idiv), TCCIR_OP_ASSIGN);
  uint32_t expected = (uint32_t)-5 / 2u;
  UT_ASSERT_EQ((unsigned)irop_get_imm64_ex(ir, utb_src1(ir, idiv)), expected);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: unsigned modulo. */
UT_TEST(test_constprop_unsigned_mod)
{
  TCCIRState *ir = utb_new();
  int imod = utb_emit(ir, TCCIR_OP_UMOD, utb_temp(0, I32), utb_unsigned(utb_imm(-5, I32)), utb_imm(2, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imod), TCCIR_OP_ASSIGN);
  uint32_t expected = (uint32_t)-5 % 2u;
  UT_ASSERT_EQ((unsigned)irop_get_imm64_ex(ir, utb_src1(ir, imod)), expected);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* BEHAVIOUR: division by constant zero is UB; the pass replaces it with TRAP
 * rather than folding. */
UT_TEST(test_constprop_div_by_zero_trap)
{
  TCCIRState *ir = utb_new();
  int idiv = utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), utb_imm(5, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, idiv), TCCIR_OP_TRAP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* BEHAVIOUR: modulo by constant zero is UB; the pass replaces it with TRAP. */
UT_TEST(test_constprop_mod_by_zero_trap)
{
  TCCIRState *ir = utb_new();
  int imod = utb_emit(ir, TCCIR_OP_IMOD, utb_temp(0, I32), utb_imm(5, I32), utb_imm(0, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, imod), TCCIR_OP_TRAP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* FIXED: INT_MIN / -1 overflows in two's-complement signed division.  The
 * folder now bails (matching the UB-bail convention already used by the
 * second folding routine), leaving the DIV in place rather than folding to
 * a target-dependent INT_MIN. */
UT_TEST(test_constprop_intmin_div_neg1_bugs)
{
  TCCIRState *ir = utb_new();
  int idiv = utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), utb_imm(INT_MIN, I32), utb_imm(-1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, idiv), TCCIR_OP_DIV);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: 64-bit addition does not get truncated to 32 bits. */
UT_TEST(test_constprop_int64_add)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I64), utb_imm(0x7fffffff, I64), utb_imm(1, I64));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, iadd)), 0x80000000LL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* SEMI-ORACLE: 64-bit addition with a value outside the 32-bit range. */
UT_TEST(test_constprop_int64_add_large)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  IROperand c1 = utb_imm64(ir, 0x100000000LL, I64);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I64), c1, utb_imm(1, I64));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, iadd)), 0x100000001LL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* GUARD: a large constant VAR with multiple uses is kept in the VAR so it is
 * materialised once; the pass suppresses propagation to avoid N pool loads. */
UT_TEST(test_constprop_large_const_multi_use_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0x12345678, I32), UTB_NONE);
  int i1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  int i2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i1)), VR_VAR(0));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i2)), VR_VAR(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: a large constant VAR with a single use is propagated. */
UT_TEST(test_constprop_large_const_single_use_propagates)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0x12345678, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iadd)), 0x12345679);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* GUARD: a VAR marked as complex is not propagated, even when single-def
 * immediate. */
UT_TEST(test_constprop_complex_var_not_propagated)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  ir->variables_live_intervals[0].is_complex = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_VAR(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE: const_prop reaches a fixpoint in a single iteration. */
UT_TEST(test_constprop_idempotent)
{
  TCCIRState *ir = utb_new();
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(5, I32), utb_imm(3, I32));
  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_const_prop, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iadd)), 8);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: SHL #24 then SHR #24 (byte cast) folds to AND #0xFF. */
UT_TEST(test_constprop_byte_cast_shl_shr_to_and)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I32), utb_temp(1, I32), utb_imm(24, I32));
  int ishr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(24, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ishr), TCCIR_OP_AND);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, ishr)), 0xFF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: SHR #8 then AND #0xFF fuses to UBFX #8,#8. */
UT_TEST(test_constprop_shr_and_to_ubfx)
{
  TCCIRState *ir = utb_new();
  int ish = utb_emit(ir, TCCIR_OP_SHR, utb_temp(0, I32), utb_temp(1, I32), utb_imm(8, I32));
  int iand = utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, iand), TCCIR_OP_UBFX);
  int param = (int)irop_get_imm64_ex(ir, utb_src2(ir, iand));
  UT_ASSERT_EQ(param, 8 | (8 << 5));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: (x ^ C) ^ C -> x. */
UT_TEST(test_constprop_xor_cancellation)
{
  TCCIRState *ir = utb_new();
  int ix1 = utb_emit(ir, TCCIR_OP_XOR, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0xAA, I32));
  int ix2 = utb_emit(ir, TCCIR_OP_XOR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(0xAA, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ix1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ix2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ix2)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: CMP of two constants followed by SETIF folds to the boolean result. */
UT_TEST(test_constprop_cmp_setif_fold_gt)
{
  TCCIRState *ir = utb_new();
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(3, I32));
  int iset = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0x9f, I32), UTB_NONE);
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iset)), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 4), 0);
  utb_free(ir);
  return 0;
}

/* GUARD: BOOL_OR with only one constant operand is left untouched because the
 * backend cannot materialise mixed const/reg boolean ops. */
UT_TEST(test_constprop_bool_or_one_const_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);
  int ior = utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(0, I32), utb_var(0, I32), utb_temp(1, I32));
  int changes = tcc_ir_opt_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ior), TCCIR_OP_BOOL_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);
  utb_free(ir);
  return 0;
}

/* ============================================================ global_init_prop
 *
 * tcc_ir_opt_global_init_prop reads the initialized value of a const/static
 * global out of its section data and folds a LOAD (or read-side `is_sym &&
 * is_lval` deref operand) into an immediate.  In this isolated harness elfsym()
 * is stubbed to return NULL, so the data-read fold can never reach the section;
 * every test here therefore drives the GATE logic (linkage/attribute/type
 * filters) and asserts the pass makes no change.  The gates exercised are the
 * load-bearing safety filters that decide whether a symbol's value is foldable
 * at all. */

/* Build a SYMREF operand for a global `sym` with a chosen addend, lval flag and
 * btype.  is_local=0, is_const flag passed through; mirrors the production
 * `&sym` deref operand shape that global_init_prop inspects. */
static IROperand utb_gsymref(TCCIRState *ir, Sym *sym, int32_t addend, int is_lval, int is_const, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
  return irop_make_symref(0, sidx, is_lval, 0 /*is_local*/, is_const, btype);
}

/* NULL-IR guard. */
UT_TEST(test_globalinitprop_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_opt_global_init_prop(NULL), 0);
  return 0;
}

/* GUARD: no instructions -> 0. */
UT_TEST(test_globalinitprop_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_global_init_prop(ir), 0);
  utb_free(ir);
  return 0;
}

/* GUARD: a LOAD whose address operand is NOT a symref (a plain VAR) is ignored —
 * the pass only considers `is_sym && is_lval` operands. */
UT_TEST(test_globalinitprop_non_sym_operand_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* GUARD: a symref operand that is NOT is_lval (an address-by-value, not a
 * read-side deref) is skipped — only is_sym && is_lval operands are folded. */
UT_TEST(test_globalinitprop_non_lval_symref_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.type.t = VT_INT | VT_CONSTANT;
  gsym.type.ref = NULL;
  /* is_lval=0 -> address-by-value, not a deref the pass folds. */
  IROperand sref = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 0, /*is_const*/ 1, I32);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), sref, utb_imm(1, I32));
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);
  utb_free(ir);
  return 0;
}

/* GUARD (weak): a const+static global whose symbol is weak must not be folded —
 * a weak symbol may be overridden at link time with a different initializer. */
UT_TEST(test_globalinitprop_weak_sym_guard)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.a.weak = 1;
  gsym.type.t = VT_INT | VT_CONSTANT | VT_STATIC;
  gsym.type.ref = NULL;
  IROperand addr = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 1, /*is_const*/ 1, I32);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), addr, UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* GUARD (volatile): a volatile-qualified global is never foldable — each access
 * must hit memory. */
UT_TEST(test_globalinitprop_volatile_guard)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.type.t = VT_INT | VT_CONSTANT | VT_STATIC | VT_VOLATILE;
  gsym.type.ref = NULL;
  IROperand addr = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 1, /*is_const*/ 1, I32);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), addr, UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* GUARD (linkage): a non-static, non-const global (ordinary external/automatic
 * linkage) is not foldable — its definitive initializer is not knowable here. */
UT_TEST(test_globalinitprop_nonstatic_nonconst_guard)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.type.t = VT_INT; /* neither VT_STATIC nor VT_CONSTANT */
  gsym.type.ref = NULL;
  IROperand addr = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 1, /*is_const*/ 0, I32);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), addr, UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* PATH (non-const static, not late-reopt phase): a non-const static global,
 * with its address never taken, is deferred — the pass flags the current
 * function for end-of-TU re-optimization (func_late_reopt) and makes no change
 * this pass.  This exercises the late_reopt recording branch.  We set/restore
 * tcc_state->cur_func_sym and ir_late_reopt_phase around the call so we don't
 * leave global state mutated for sibling tests. */
UT_TEST(test_globalinitprop_nonconst_static_records_late_reopt)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.a.addrtaken = 0;
  gsym.type.t = VT_INT | VT_STATIC; /* static, not const */
  gsym.type.ref = NULL;

  static Sym func_sym, func_ref;
  func_ref.f.func_late_reopt = 0;
  func_sym.type.ref = &func_ref;

  Sym *saved_func = tcc_state->cur_func_sym;
  int saved_phase = tcc_state->ir_late_reopt_phase;
  tcc_state->cur_func_sym = &func_sym;
  tcc_state->ir_late_reopt_phase = 0; /* not the late phase */

  IROperand addr = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 1, /*is_const*/ 0, I32);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), addr, UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);

  tcc_state->cur_func_sym = saved_func;
  tcc_state->ir_late_reopt_phase = saved_phase;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  /* The deferral branch must have recorded the function for late re-opt. */
  UT_ASSERT_EQ((int)func_ref.f.func_late_reopt, 1);
  utb_free(ir);
  return 0;
}

/* PATH (const global, all gates pass, no section): a const global passes every
 * linkage/attribute/type gate and reaches elfsym(), which the harness stub
 * returns NULL for — so the fold bails at the no-ELF-symbol check.  Confirms the
 * full gate chain is traversed without firing (changes==0) and the LOAD is
 * intact. */
UT_TEST(test_globalinitprop_const_reaches_elfsym_no_section)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gsym;
  gsym.a = (struct SymAttr){0};
  gsym.type.t = VT_INT | VT_CONSTANT;
  gsym.type.ref = NULL;
  IROperand addr = utb_gsymref(ir, &gsym, 0, /*is_lval*/ 1, /*is_const*/ 1, I32);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), addr, UTB_NONE);
  int changes = tcc_ir_opt_global_init_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* =========================================================== symref_const_prop
 *
 * Propagate `ASSIGN T <- &S+addend` (a symref-by-value, not is_lval) into later
 * uses of T within the same straight-line block; each use becomes a fresh symref
 * carrying the same sym+addend, preserving the use-site lval/unsigned flags.
 * Restricted to TMP defs; cleared at control-flow boundaries. */

/* NULL / empty guards. */
UT_TEST(test_symrefconstprop_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_opt_symref_const_prop(NULL), 0);
  return 0;
}

/* GUARD: no TMP destinations at all -> max_tmp_pos==0 early return. */
UT_TEST(test_symrefconstprop_no_tmp_dest)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int changes = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE: `ASSIGN T0 <- &S` then a later `ADD T1 <- T0, #4`.  The use of T0 in
 * the ADD is rewritten to a symref for S; the tracked def stays.  changes>0 and
 * the ADD's src1 becomes a sym operand referencing the same Sym. */
UT_TEST(test_symrefconstprop_propagates_into_use)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  IROperand sref = utb_gsymref(ir, &s, 0, /*is_lval*/ 0, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT(changes > 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ((int)s1.is_sym, 1);
  UT_ASSERT_EQ(irop_get_sym_ex(ir, s1), &s);
  utb_free(ir);
  return 0;
}

/* POSITIVE (addend + lval-flag preservation): `ASSIGN T0 <- &S+12`, then a
 * deref use `LOAD T1 <- T0` (src1 is_lval).  The substituted symref carries the
 * same Sym and addend, and preserves the use's is_lval flag so the result is a
 * lval-symref (a memory deref of &S+12). */
UT_TEST(test_symrefconstprop_preserves_addend_and_lval)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  IROperand sref = utb_gsymref(ir, &s, 12, /*is_lval*/ 0, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  /* Use is an lval deref of T0. */
  int iuse = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);

  int changes = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT(changes > 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ((int)s1.is_sym, 1);
  UT_ASSERT_EQ((int)s1.is_lval, 1);
  IRPoolSymref *ref = irop_get_symref_ex(ir, s1);
  UT_ASSERT(ref != NULL);
  UT_ASSERT_EQ(ref->sym, &s);
  UT_ASSERT_EQ((int)ref->addend, 12);
  utb_free(ir);
  return 0;
}

/* GUARD (lval source not tracked): `ASSIGN T0 <- *(&S)` where the source symref
 * is is_lval (a memory deref, not an address constant) must NOT be tracked — the
 * tracked value is the address, not the loaded contents.  A later use of T0 is
 * left as a plain vreg. */
UT_TEST(test_symrefconstprop_lval_source_not_tracked)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  /* is_lval=1 -> source is a deref, not an address-by-value. */
  IROperand sref = utb_gsymref(ir, &s, 0, /*is_lval*/ 1, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ((int)s1.is_sym, 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(0));
  utb_free(ir);
  return 0;
}

/* GUARD (redefinition invalidates): a tracked T0 that is overwritten by a
 * non-ASSIGN op (an arithmetic def) before its use must not propagate the stale
 * symref — the pass's def-write branch clears map[T0].
 *   T0 <- &S
 *   T0 = T9 ADD #1  (redef -> map[T0] invalidated)
 *   T1 = T0 ADD #4  -> NOT substituted with a symref
 *
 * NOTE: the first use (T0 in the redefining ADD's src1) is itself substituted
 * with the symref before T0 is overwritten — that is the pass's documented
 * forward-substitution behaviour and is independent of the invalidation we are
 * pinning here.  We therefore assert specifically that the *post-redef* use
 * (the second ADD) is NOT a symref. */
UT_TEST(test_symrefconstprop_redef_invalidates)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  IROperand sref = utb_gsymref(ir, &s, 0, /*is_lval*/ 0, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  /* Redefine T0 via an ADD whose operands do NOT read T0 (use T9), so the only
   * effect is to overwrite T0 and invalidate the tracked symref. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(9, I32), utb_imm(1, I32));
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  tcc_ir_opt_symref_const_prop(ir);
  /* The post-redef use of T0 must remain a plain vreg (no stale symref). */
  IROperand s1 = utb_src1(ir, iuse);
  UT_ASSERT_EQ((int)s1.is_sym, 0);
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(0));
  utb_free(ir);
  return 0;
}

/* GUARD (block boundary clears tracking): a JUMPIF between the def and the use
 * ends the straight-line region, so the symref must not cross the merge.
 *   T0 <- &S
 *   JUMPIF cond -> L     (clears tracked map)
 * L:T1 = T0 ADD #4       -> NOT substituted (jump target is a block start) */
UT_TEST(test_symrefconstprop_block_boundary_clears)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  IROperand sref = utb_gsymref(ir, &s, 0, /*is_lval*/ 0, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  /* JUMPIF to index 2 (the use) — makes index 2 a block start. */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(0x94, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_TMP(0));
  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE: after one pass substitutes T0's use, a second pass finds the use
 * is already a symref (is_sym) and makes no further change. */
UT_TEST(test_symrefconstprop_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym s;
  s.v = 0;
  IROperand sref = utb_gsymref(ir, &s, 0, /*is_lval*/ 0, /*is_const*/ 1, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), sref, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));

  int first = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT(first > 0);
  int second = tcc_ir_opt_symref_const_prop(ir);
  UT_ASSERT_EQ(second, 0);
  utb_free(ir);
  return 0;
}

/* ===================================================== complex_const_param_fold
 *
 * Folds the {real,imag} pair of a _Complex float local — stored to a stack slot
 * as two 4-byte float constants — directly into the FUNCPARAMVAL that passes it
 * by value, packing the two component bit patterns into a 64-bit complex-float
 * immediate and NOP-ing the two component stores. */

/* Build a complex-float lval stack operand at `off` (vreg==-1, F32, is_complex,
 * is_lval, not param) — the FUNCPARAM source shape the fold targets. */
static IROperand utb_cplx_slot(int32_t off)
{
  IROperand op = irop_make_stackoff(0, off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, F32);
  op.is_complex = 1;
  return op;
}

/* Build a plain float lval stack operand at `off` for a component STORE dest
 * (vreg==-1, F32, is_lval, NOT complex, not param). */
static IROperand utb_f32_slot(int32_t off)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, F32);
}

/* POSITIVE: the canonical 3-op pattern folds.
 *   STORE slot[-8] <- #real_bits
 *   STORE slot[-4] <- #imag_bits
 *   FUNCPARAMVAL  slot[-8] (complex lval)
 * -> param src1 becomes a packed complex-float i64 immediate; stores NOP'd. */
UT_TEST(test_cplxparamfold_packs_and_nops_stores)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  uint32_t real_bits = 0x3f800000u; /* 1.0f */
  uint32_t imag_bits = 0x40000000u; /* 2.0f */

  int isr = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-8), utb_imm((int32_t)real_bits, F32), UTB_NONE);
  int isi = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-4), utb_imm((int32_t)imag_bits, F32), UTB_NONE);
  int ip = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_cplx_slot(-8),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  int changes = tcc_ir_opt_complex_const_param_fold(ir);
  UT_ASSERT(changes > 0);

  /* Both component stores are dead. */
  UT_ASSERT_EQ(utb_op(ir, isr), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, isi), TCCIR_OP_NOP);

  /* The param source is now a complex-float immediate carrying packed bits:
   * real in the low 32 bits, imag in the high 32 bits. */
  IROperand p = utb_src1(ir, ip);
  UT_ASSERT_EQ((int)p.is_complex, 1);
  UT_ASSERT_EQ((int)p.is_lval, 0);
  UT_ASSERT_EQ(irop_is_immediate(p), 1);
  uint64_t packed = (uint64_t)real_bits | ((uint64_t)imag_bits << 32);
  UT_ASSERT_EQ((long long)irop_get_imm64_ex(ir, p), (long long)(int64_t)packed);
  utb_free(ir);
  return 0;
}

/* GUARD (non-complex param): a FUNCPARAMVAL whose source slot is NOT is_complex
 * is not a _Complex-by-value pass, so the fold never triggers. */
UT_TEST(test_cplxparamfold_non_complex_param_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int isr = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-8), utb_imm(0x3f800000, F32), UTB_NONE);
  int isi = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-4), utb_imm(0x40000000, F32), UTB_NONE);
  /* Plain (non-complex) float lval source. */
  int ip = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f32_slot(-8),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  int changes = tcc_ir_opt_complex_const_param_fold(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, isr), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, isi), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ip), TCCIR_OP_FUNCPARAMVAL);
  utb_free(ir);
  return 0;
}

/* GUARD (missing imag store): only the real component is stored, so the 8-byte
 * slot is not fully initialized by constants — fold bails (imag_store_idx<0). */
UT_TEST(test_cplxparamfold_missing_component_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int isr = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-8), utb_imm(0x3f800000, F32), UTB_NONE);
  int ip = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_cplx_slot(-8),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  int changes = tcc_ir_opt_complex_const_param_fold(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, isr), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ip), TCCIR_OP_FUNCPARAMVAL);
  utb_free(ir);
  return 0;
}

/* GUARD (extra read of the slot): a third instruction that also references the
 * 8-byte slot (an additional LOAD of slot[-8]) disqualifies the fold — the slot
 * is not touched by exactly the three expected ops. */
UT_TEST(test_cplxparamfold_extra_slot_reference_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int isr = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-8), utb_imm(0x3f800000, F32), UTB_NONE);
  int isi = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-4), utb_imm(0x40000000, F32), UTB_NONE);
  /* An extra read of the real slot through src1 of a LOAD. */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, F32), utb_f32_slot(-8), UTB_NONE);
  int ip = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_cplx_slot(-8),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  int changes = tcc_ir_opt_complex_const_param_fold(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, isr), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, isi), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ip), TCCIR_OP_FUNCPARAMVAL);
  utb_free(ir);
  return 0;
}

/* GUARD (non-constant store value): when a component store writes a non-constant
 * value (a vreg, not an immediate/float-bits) the slot can't be packed -> bail. */
UT_TEST(test_cplxparamfold_nonconst_store_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int isr = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-8), utb_temp(5, F32), UTB_NONE);
  int isi = utb_emit(ir, TCCIR_OP_STORE, utb_f32_slot(-4), utb_imm(0x40000000, F32), UTB_NONE);
  int ip = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_cplx_slot(-8),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  int changes = tcc_ir_opt_complex_const_param_fold(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, isr), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, isi), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ip), TCCIR_OP_FUNCPARAMVAL);
  utb_free(ir);
  return 0;
}

/* ============================================================== value_tracking
 *
 * Forward dataflow tracker: propagates compile-time constants through VAR
 * assignments / arithmetic / LEA+STORE and folds CMP/SETIF and a set of runtime
 * helper calls (__aeabi_lcmp etc.) when their operands are known constants.
 * Tests that build VAR destinations must allocate the live-interval table first
 * (the pass reads interval->addrtaken for every VAR position). */

/* GUARD: no instructions -> 0. */
UT_TEST(test_valuetracking_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_value_tracking(ir), 0);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 1 + 2b): a direct constant VAR assignment is tracked, and a
 * later LOAD of that VAR folds to ASSIGN of the constant.
 *   V0 <- #5
 *   T0 = LOAD V0    -> T0 = ASSIGN #5 */
UT_TEST(test_valuetracking_load_of_const_var_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iload);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 5);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 2 arithmetic fold): `V1 = V0 ADD #3` where V0 is tracked
 * constant 5 folds to `V1 = ASSIGN #8` (oracle computed independently). */
UT_TEST(test_valuetracking_arith_const_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(0, I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iadd);
  UT_ASSERT_EQ(irop_is_immediate(s1), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 5 + 3);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 2, SHL oracle): `V1 = V0 SHL #4` with V0==3 folds to 48. */
UT_TEST(test_valuetracking_shl_const_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(3, I32), UTB_NONE);
  int ish = utb_emit(ir, TCCIR_OP_SHL, utb_var(1, I32), utb_var(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ish), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ish)), 3 << 4);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 3, CMP+JUMPIF always-taken): V0==5 compared GT #3 is always
 * true, so the CMP is NOP'd and the JUMPIF becomes an unconditional JUMP.
 *   V0 <- #5
 *   CMP V0, #3
 *   JUMPIF GT -> L */
UT_TEST(test_valuetracking_cmp_jumpif_always_taken)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(0x9f /*GT*/, I32), UTB_NONE);
  /* index 3: a landing pad so the jump target is in range. */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 3, CMP+JUMPIF never-taken): V0==5 compared LT #3 is always
 * false; both CMP and JUMPIF are eliminated (NOP'd). */
UT_TEST(test_valuetracking_cmp_jumpif_never_taken)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(0x9c /*LT*/, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

/* POSITIVE (Pattern 3, CMP+SETIF): V0==5 compared EQ #5 sets the SETIF result to
 * the boolean 1; CMP is NOP'd and SETIF becomes ASSIGN #1. */
UT_TEST(test_valuetracking_cmp_setif_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));
  int iset = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0x94 /*EQ*/, I32), UTB_NONE);

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, iset)), 1);
  utb_free(ir);
  return 0;
}

/* GUARD (address-taken VAR not tracked): when V0's interval->addrtaken is set,
 * its constant assignment is not tracked, so a later LOAD does not fold.
 *   V0 <- #5  (addrtaken)
 *   T0 = LOAD V0  -> stays LOAD */
UT_TEST(test_valuetracking_addrtaken_not_tracked)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);
  ir->variables_live_intervals[0].addrtaken = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  utb_free(ir);
  return 0;
}

/* GUARD (merge point clears state): a JUMP target between the const def and the
 * use is a merge point (>1 predecessor), so the tracked constant does not cross
 * it and the LOAD does not fold.
 *   V0 <- #5
 *   JUMP -> L            (target L gets a second predecessor)
 *   ... (fallthrough also reaches L via index arithmetic)
 * Built so L (index 3) has two predecessors: the JUMP at idx1 and the
 * fall-through from idx2. */
UT_TEST(test_valuetracking_merge_point_clears)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);          /* 1 -> 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(9, I32), UTB_NONE); /* 2 (other pred path) */
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_var(0, I32), UTB_NONE); /* 3 = merge */

  int changes = tcc_ir_opt_value_tracking(ir);
  /* At the merge the two paths disagree (5 vs 9), so V0 is not a known constant
   * and the LOAD must not fold to either value. */
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD);
  (void)changes;
  utb_free(ir);
  return 0;
}

/* POSITIVE (call fold, __aeabi_lcmp): both 64-bit compare arguments are
 * immediates, so the call folds to the three-way result.  Oracle: lcmp(10,20) =
 * (10>20)-(10<20) = -1. */
UT_TEST(test_valuetracking_lcmp_const_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  /* value_tracking probes VAR pos 0 in its addrtaken pre-scan even with no
   * VARs present, so the interval table must exist. */
  utb_alloc_var_intervals(ir, 4);

  static Sym callee_sym;
  callee_sym.v = 7;
  utb_set_tok_str(7, "__aeabi_lcmp");
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &callee_sym, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(10, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(20, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  int expected = (10 > 20) - (10 < 20);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, icall)), expected);
  utb_set_tok_str(7, NULL);
  utb_free(ir);
  return 0;
}

/* POSITIVE (call fold, __aeabi_ulcmp): unsigned three-way compare.  Oracle with
 * (uint64_t)-1 vs 1 -> 1 (the huge unsigned value is greater). */
UT_TEST(test_valuetracking_ulcmp_const_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  static Sym callee_sym;
  callee_sym.v = 8;
  utb_set_tok_str(8, "__aeabi_ulcmp");
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &callee_sym, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(-1, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  uint64_t u0 = (uint64_t)-1, u1 = 1;
  int expected = (u0 > u1) - (u0 < u1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, icall)), expected);
  utb_set_tok_str(8, NULL);
  utb_free(ir);
  return 0;
}

/* GUARD (unknown callee not folded): a FUNCCALLVAL to a name the value tracker
 * does not special-case (here "?") is left intact even with constant args. */
UT_TEST(test_valuetracking_unknown_call_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 4);

  static Sym callee_sym;
  callee_sym.v = 9; /* maps to "?" (no special-case name) */
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &callee_sym, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(10, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(20, I64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_value_tracking(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_constprop)
{
  UT_COVERS("const_var_prop");
  UT_COVERS("const_prop");
  UT_COVERS("global_init_prop");
  UT_COVERS("symref_const_prop");
  UT_COVERS("complex_const_param_fold");
  UT_COVERS("value_tracking");

  /* const_var_prop */
  UT_RUN(test_constvarprop_imm_var_folds_into_use);
  UT_RUN(test_constvarprop_load_of_const_var_becomes_assign);
  UT_RUN(test_constvarprop_addrtaken_var_not_propagated);
  UT_RUN(test_constvarprop_multiply_defined_not_propagated);
  UT_RUN(test_constvarprop_nonconst_source_not_propagated);
  UT_RUN(test_constvarprop_idempotent);
  UT_RUN(test_constvarprop_large_imm_single_use);
  UT_RUN(test_constvarprop_stackoff_source_not_propagated);

  /* const_prop */
  UT_RUN(test_constprop_two_const_add_folds);
  UT_RUN(test_constprop_two_const_mul_folds);
  UT_RUN(test_constprop_var_const_propagated_and_folded);
  UT_RUN(test_constprop_add_zero_simplifies_to_copy);
  UT_RUN(test_constprop_mul_zero_simplifies_to_zero);
  UT_RUN(test_constprop_two_nonconst_not_folded);
  UT_RUN(test_constprop_sub_zero_identity);
  UT_RUN(test_constprop_or_zero_identity);
  UT_RUN(test_constprop_and_minusone_identity);
  UT_RUN(test_constprop_or_minusone_to_const);
  UT_RUN(test_constprop_and_zero_to_zero);
  UT_RUN(test_constprop_xor_zero_identity);
  UT_RUN(test_constprop_xor_same_const_to_zero);
  UT_RUN(test_constprop_mul_one_identity);
  UT_RUN(test_constprop_sub_same_const_to_zero);
  UT_RUN(test_constprop_mul_pow2_const);
  UT_RUN(test_constprop_intmax_plus_one_wraps);
  UT_RUN(test_constprop_intmin_minus_one_wraps);
  UT_RUN(test_constprop_overflow_signbit_add_wraps);
  UT_RUN(test_constprop_shl_zero_identity);
  UT_RUN(test_constprop_shl_31);
  UT_RUN(test_constprop_shl_32_bails);
  UT_RUN(test_constprop_shr_31);
  UT_RUN(test_constprop_sar_31);
  UT_RUN(test_constprop_signed_div);
  UT_RUN(test_constprop_signed_mod);
  UT_RUN(test_constprop_unsigned_div);
  UT_RUN(test_constprop_unsigned_mod);
  UT_RUN(test_constprop_div_by_zero_trap);
  UT_RUN(test_constprop_mod_by_zero_trap);
  UT_RUN(test_constprop_intmin_div_neg1_bugs);
  UT_RUN(test_constprop_int64_add);
  UT_RUN(test_constprop_int64_add_large);
  UT_RUN(test_constprop_large_const_multi_use_kept);
  UT_RUN(test_constprop_large_const_single_use_propagates);
  UT_RUN(test_constprop_complex_var_not_propagated);
  UT_RUN(test_constprop_idempotent);
  UT_RUN(test_constprop_byte_cast_shl_shr_to_and);
  UT_RUN(test_constprop_shr_and_to_ubfx);
  UT_RUN(test_constprop_xor_cancellation);
  UT_RUN(test_constprop_cmp_setif_fold_gt);
  UT_RUN(test_constprop_bool_or_one_const_no_fold);

  /* global_init_prop */
  UT_RUN(test_globalinitprop_null_ir);
  UT_RUN(test_globalinitprop_empty);
  UT_RUN(test_globalinitprop_non_sym_operand_no_fold);
  UT_RUN(test_globalinitprop_non_lval_symref_no_fold);
  UT_RUN(test_globalinitprop_weak_sym_guard);
  UT_RUN(test_globalinitprop_volatile_guard);
  UT_RUN(test_globalinitprop_nonstatic_nonconst_guard);
  UT_RUN(test_globalinitprop_nonconst_static_records_late_reopt);
  UT_RUN(test_globalinitprop_const_reaches_elfsym_no_section);

  /* symref_const_prop */
  UT_RUN(test_symrefconstprop_null_ir);
  UT_RUN(test_symrefconstprop_no_tmp_dest);
  UT_RUN(test_symrefconstprop_propagates_into_use);
  UT_RUN(test_symrefconstprop_preserves_addend_and_lval);
  UT_RUN(test_symrefconstprop_lval_source_not_tracked);
  UT_RUN(test_symrefconstprop_redef_invalidates);
  UT_RUN(test_symrefconstprop_block_boundary_clears);
  UT_RUN(test_symrefconstprop_idempotent);

  /* complex_const_param_fold */
  UT_RUN(test_cplxparamfold_packs_and_nops_stores);
  UT_RUN(test_cplxparamfold_non_complex_param_no_fold);
  UT_RUN(test_cplxparamfold_missing_component_no_fold);
  UT_RUN(test_cplxparamfold_extra_slot_reference_no_fold);
  UT_RUN(test_cplxparamfold_nonconst_store_no_fold);

  /* value_tracking */
  UT_RUN(test_valuetracking_empty);
  UT_RUN(test_valuetracking_load_of_const_var_folds);
  UT_RUN(test_valuetracking_arith_const_fold);
  UT_RUN(test_valuetracking_shl_const_fold);
  UT_RUN(test_valuetracking_cmp_jumpif_always_taken);
  UT_RUN(test_valuetracking_cmp_jumpif_never_taken);
  UT_RUN(test_valuetracking_cmp_setif_fold);
  UT_RUN(test_valuetracking_addrtaken_not_tracked);
  UT_RUN(test_valuetracking_merge_point_clears);
  UT_RUN(test_valuetracking_lcmp_const_fold);
  UT_RUN(test_valuetracking_ulcmp_const_fold);
  UT_RUN(test_valuetracking_unknown_call_no_fold);
}
