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

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_const_var_prop(TCCIRState *ir);
int tcc_ir_opt_const_prop(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

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

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_constprop)
{
  UT_COVERS("const_var_prop");
  UT_COVERS("const_prop");

  /* const_var_prop */
  UT_RUN(test_constvarprop_imm_var_folds_into_use);
  UT_RUN(test_constvarprop_load_of_const_var_becomes_assign);
  UT_RUN(test_constvarprop_addrtaken_var_not_propagated);
  UT_RUN(test_constvarprop_multiply_defined_not_propagated);
  UT_RUN(test_constvarprop_nonconst_source_not_propagated);

  /* const_prop */
  UT_RUN(test_constprop_two_const_add_folds);
  UT_RUN(test_constprop_two_const_mul_folds);
  UT_RUN(test_constprop_var_const_propagated_and_folded);
  UT_RUN(test_constprop_add_zero_simplifies_to_copy);
  UT_RUN(test_constprop_mul_zero_simplifies_to_zero);
  UT_RUN(test_constprop_two_nonconst_not_folded);
}
