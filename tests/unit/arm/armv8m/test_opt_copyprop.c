/*
 *  test_opt_copyprop.c - suite for ir/opt_copyprop.c (copy propagation)
 *
 *  tcc_ir_opt_copy_prop tracks ASSIGN "copies" of the form
 *      TMP:X <- VAR:Y | PARAM:Y | TMP:Y      (src not constant, not lval)
 *  and rewrites later uses of TMP:X with the recorded source operand, as long
 *  as the source has not been redefined between the copy and the use and no
 *  basic-block boundary / terminator / FUNCCALL has cleared the copy table.
 *
 *  Key guards verified here:
 *    - lval (DEREF) uses keep their is_lval / load-width bits when the source is
 *      substituted in, and a VAR/PARAM source is NOT propagated into an lval use
 *      (only a TMP source is).
 *    - ASSIGN with an lval source is a LOAD, not a copy, so it is NOT recorded.
 *    - a constant source is not a copy and is NOT recorded.
 *    - redefining the source before the use invalidates the copy.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_copy_prop(TCCIRState *ir);

/* The CSE sub-passes living in the same TU (ir/opt_copyprop.c). */
int tcc_ir_opt_cse_global_load(TCCIRState *ir);
int tcc_ir_opt_globalsym_cse(TCCIRState *ir);
int tcc_ir_opt_cse_param_add(TCCIRState *ir);
int tcc_ir_opt_local_load_cse(TCCIRState *ir);
int tcc_ir_opt_local_alu_cse(TCCIRState *ir);
int tcc_ir_opt_bool_cse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I16 IROP_BTYPE_INT16
#define I64 IROP_BTYPE_INT64
#define I8  IROP_BTYPE_INT8

/* Encoded vreg helpers for assertions. */
#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))
#define VR_PARAM(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, (p))

/* Return one-past-the-largest encoded vreg used in the IR built so far.
 * This is the appropriate max_vreg bound for utb_assert_wellformed(). */
static inline int32_t utb_max_vreg_bound(TCCIRState *ir)
{
  int32_t max = 0;
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    int32_t v;
    if (irop_config[q->op].has_dest && (v = irop_get_vreg(dest)) > max)
      max = v;
    if (irop_config[q->op].has_src1 && (v = irop_get_vreg(s1)) > max)
      max = v;
    if (irop_config[q->op].has_src2 && (v = irop_get_vreg(s2)) > max)
      max = v;
  }
  return max + 1;
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: a plain VAR copy propagates into an arithmetic use.
 *   T1 <- V0            [ASSIGN copy]
 *   T2 = T1 ADD #1      -> src1 rewritten to V0
 * changes > 0, and T2.src1 becomes V0. */
UT_TEST(test_copyprop_var_copy_propagates_to_add)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  /* The ADD's src1 must now reference the copy source V0, not T1. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_VAR(0));
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a TMP->TMP copy propagates into BOTH src1 and src2 of one use.
 *   T1 <- T0
 *   T3 = T1 ADD T1      -> both operands rewritten to T0 (two changes) */
UT_TEST(test_copyprop_tmp_copy_propagates_both_operands)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  /* src1 and src2 are each rewritten -> at least two propagations. */
  UT_ASSERT(changes >= 2);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, iadd)), VR_TMP(0));

  utb_free(ir);
  return 0;
}

/* is_lval PRESERVATION: a TMP->TMP copy of an address propagates into an lval
 * (DEREF) use while keeping the deref + load-width bits taken from the use site.
 *   T1 <- T0                         (register-to-register address copy)
 *   T2 = LOAD T1***DEREF*** (INT16)  -> src1 becomes T0 but stays lval, INT16 */
UT_TEST(test_copyprop_lval_use_preserves_deref_and_width)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  /* Build the LOAD's lval src1 by hand (DEREF, narrow INT16 load). */
  IROperand load_src = utb_temp(1, I16);
  load_src.is_lval = 1;
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), load_src, UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand s1 = utb_src1(ir, iload);
  /* Substituted to the copy source T0... */
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(0));
  /* ...but the DEREF semantics and the use-site load width are preserved. */
  UT_ASSERT_EQ((int)s1.is_lval, 1);
  UT_ASSERT_EQ(irop_get_btype(s1), I16);

  utb_free(ir);
  return 0;
}

/* GUARD (lval source NOT propagated for VAR): an lval use whose copy source is a
 * VAR must NOT be rewritten, because propagating a VAR into a DEREF would extend
 * its live range and can corrupt register allocation. Only TMP sources qualify.
 *   T1 <- V0
 *   T2 = LOAD T1***DEREF***   -> NOT rewritten (still T1, still lval) */
UT_TEST(test_copyprop_lval_use_var_source_not_propagated)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);

  IROperand load_src = utb_temp(1, I32);
  load_src.is_lval = 1;
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), load_src, UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  /* The lval use is left untouched; the only possible change would have been
   * this propagation, so the pass must report no changes. */
  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, iload);
  UT_ASSERT_EQ(utb_vreg(s1), VR_TMP(1));
  UT_ASSERT_EQ((int)s1.is_lval, 1);

  utb_free(ir);
  return 0;
}

/* GUARD (ASSIGN with lval source is a LOAD, not a copy): must NOT be recorded,
 * so a later use of the destination is NOT rewritten.
 *   T1 <- V0***DEREF***   (this is a LOAD-shaped ASSIGN)
 *   T2 = T1 ADD #1        -> NOT rewritten */
UT_TEST(test_copyprop_lval_source_assign_not_recorded)
{
  TCCIRState *ir = utb_new();

  IROperand lval_src = utb_var(0, I32);
  lval_src.is_lval = 1;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), lval_src, UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD (constant source): T1 <- #5 is not a copy; no propagation.
 *   T1 <- #5
 *   T2 = T1 ADD #1        -> NOT rewritten */
UT_TEST(test_copyprop_const_source_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(5, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (source redefined before use): the copy is invalidated when its
 * source VAR is reassigned between the copy and the use, so it must NOT
 * propagate past the redefinition.
 *   T1 <- V0
 *   V0 <- #9          (redefines the source)
 *   T2 = T1 ADD #1    -> NOT rewritten (still T1) */
UT_TEST(test_copyprop_source_redef_blocks_propagation)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(9, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (btype mismatch on the copy): T9 is a 64-bit value and T10 <- T9
 * truncates to 32-bit; that ASSIGN is NOT a copy (different register width
 * class), so a later use of T10 must NOT be rewritten.
 *   T10(INT32) <- T9(INT64)
 *   T11 = T10 ADD #1      -> NOT rewritten */
UT_TEST(test_copyprop_btype_mismatch_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(10, I32), utb_temp(9, I64), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(11, I32), utb_temp(10, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(10));

  utb_free(ir);
  return 0;
}

/* POSITIVE (STORE dest propagation): copy of an address propagates into the
 * STORE destination pointer while preserving the DEREF + store width.
 *   T1 <- T0
 *   STORE T1***DEREF*** <- V5   -> dest pointer rewritten to T0 (still lval) */
UT_TEST(test_copyprop_store_dest_tmp_source_propagates)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  /* STORE: dest = address (lval pointer), src1 = value. */
  IROperand store_addr = utb_temp(1, I32);
  store_addr.is_lval = 1;
  int istore = utb_emit(ir, TCCIR_OP_STORE, store_addr, utb_var(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand d = utb_dest(ir, istore);
  UT_ASSERT_EQ(utb_vreg(d), VR_TMP(0));
  UT_ASSERT_EQ((int)d.is_lval, 1);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a PARAM copy propagates into an arithmetic use. */
UT_TEST(test_copyprop_param_copy_propagates_to_add)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_param(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_PARAM(0));
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: copy-of-copy chain (VAR -> TMP -> TMP) collapses to the original
 * source after running to a fixpoint. */
UT_TEST(test_copyprop_copy_chain_var_through_tmp)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_imm(1, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_copy_prop, 8);

  (void)total;
  UT_ASSERT(utb_vreg(utb_src1(ir, iadd)) == VR_VAR(0));
  UT_ASSERT_EQ(utb_op(ir, iadd), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a three-link copy chain collapses to the original source. */
UT_TEST(test_copyprop_copy_chain_three_links)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32), utb_imm(1, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_copy_prop, 8);

  (void)total;
  UT_ASSERT(utb_vreg(utb_src1(ir, iadd)) == VR_PARAM(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* is_lval PRESERVATION (src2): a TMP copy propagates into an lval use in the
 * second operand slot, preserving DEREF and the use-site load width. */
UT_TEST(test_copyprop_lval_src2_preserves_deref_and_width)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(0, I32), utb_lval(utb_temp(1, I16)));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand s2 = utb_src2(ir, iadd);
  UT_ASSERT_EQ(utb_vreg(s2), VR_TMP(0));
  UT_ASSERT_EQ((int)s2.is_lval, 1);
  UT_ASSERT_EQ(irop_get_btype(s2), I16);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE (STORE dest with PARAM source): a PARAM copy propagates into a STORE
 * destination pointer when the PARAM is not a stack-local/llocal address. */
UT_TEST(test_copyprop_store_dest_param_source_propagates)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_param(0, I32), UTB_NONE);
  int istore = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I16)), utb_var(5, I16), UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(changes > 0);
  IROperand d = utb_dest(ir, istore);
  UT_ASSERT_EQ(utb_vreg(d), VR_PARAM(0));
  UT_ASSERT_EQ((int)d.is_lval, 1);
  UT_ASSERT_EQ(irop_get_btype(d), I16);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD (STORE dest with llocal PARAM source): a PARAM that carries is_llocal
 * represents a stack-relative address; propagating it into a DEREF would turn
 * a register-resident store into a stack-relative one, so the pass must bail. */
UT_TEST(test_copyprop_store_dest_param_llocal_source_not_propagated)
{
  TCCIRState *ir = utb_new();

  IROperand llocal_param = utb_llocal(utb_param(0, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), llocal_param, UTB_NONE);
  int istore = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_var(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand d = utb_dest(ir, istore);
  UT_ASSERT_EQ(utb_vreg(d), VR_TMP(1));
  UT_ASSERT_EQ((int)d.is_lval, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: copies are invalidated at function calls, so a use after a CALL must
 * not be rewritten. */
UT_TEST(test_copyprop_copy_cleared_across_func_call)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, utb_imm(0, I32), utb_var(99, I32), utb_imm(0, I32));
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: copies do not survive across basic-block boundaries. A use at a merge
 * point (target of jumps from multiple predecessors) must see a cleared table. */
UT_TEST(test_copyprop_copy_cleared_at_merge_point)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_var(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: redefining a TMP source between the copy and the use invalidates
 * the copy, even though the source is a TEMP rather than a VAR/PARAM. */
UT_TEST(test_copyprop_tmp_source_redef_blocks_propagation)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD (narrow source width): an INT8 source assigned into an INT32 dest is not
 * a register-width-compatible copy and must not be recorded. */
UT_TEST(test_copyprop_int8_btype_mismatch_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(10, I32), utb_temp(9, I8), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(11, I32), utb_temp(10, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(10));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* GUARD (stack-offset source): a source operand that is a stack offset is not a
 * register-resident VAR/PARAM/TMP, so the ASSIGN must not be treated as a copy. */
UT_TEST(test_copyprop_stackoff_source_not_recorded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_stackoff(0, 0, 0, 0, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* FIXED: a self-copy (T1 <- T1) is no longer recorded as a copy, so the pass
 * does not "propagate" T1 onto itself.  It reports no change and converges. */
UT_TEST(test_copyprop_self_copy)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(1, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));

  int c1 = tcc_ir_opt_copy_prop(ir);
  int c2 = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(c1, 0);
  UT_ASSERT_EQ(c2, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_TMP(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* DEGENERATE: an empty function should return 0 without crashing. */
UT_TEST(test_copyprop_empty_ir_returns_zero)
{
  TCCIRState *ir = utb_new();

  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* DEGENERATE: a single non-copy instruction with a TEMP dest returns 0. */
UT_TEST(test_copyprop_single_instruction_no_copy_returns_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  int changes = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* FIXPOINT: after a chain collapses, a second run reports no changes.
 * FIXED: the copy-recording step now sees the propagated source, so the
 * VAR->TMP->TMP chain fully collapses in one pass (T3 <- V0 directly) and the
 * second run converges with no spurious change. */
UT_TEST(test_copyprop_idempotent_after_chain)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_imm(1, I32));

  int c1 = tcc_ir_opt_copy_prop(ir);
  int c2 = tcc_ir_opt_copy_prop(ir);

  UT_ASSERT(c1 > 0);
  UT_ASSERT_EQ(c2, 0);
  /* The ADD source collapses all the way to the original VAR in one pass. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iadd)), VR_VAR(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, utb_max_vreg_bound(ir)), 0);

  utb_free(ir);
  return 0;
}

/* ================================================================== *
 *  CSE sub-passes that share ir/opt_copyprop.c
 *
 *  These exercise the load/ALU/boolean CSE passes alongside copy_prop.
 *  Several of them gate on register-allocator metadata (interval sizes,
 *  symref pool, compact_instructions_size), so they build their IR with
 *  the helpers below rather than bare utb_new().
 * ================================================================== */

/* Mark vregs of all types valid up to `n` positions, so passes that call
 * tcc_ir_vreg_is_valid() (e.g. cse_param_add) see hand-built vregs as real.
 * Only the *_size fields are read by those passes; the interval arrays stay
 * NULL (utb_free tolerates NULL). */
static inline void utb_set_vreg_validity(TCCIRState *ir, int n)
{
  ir->variables_live_intervals_size = n;
  ir->parameters_live_intervals_size = n;
  ir->temporary_variables_live_intervals_size = n;
}

/* Prepare an IR that a temp-allocating pass (globalsym_cse) can mutate:
 *   - symref/operand pools initialized
 *   - compact_instructions_size set so the insert/shift realloc loop has a
 *     non-zero starting capacity
 *   - a real temporary_variables_live_intervals array so
 *     tcc_ir_vreg_alloc_temp() can hand out fresh TEMP vregs. */
static inline TCCIRState *utb_new_sym(void)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = 32; /* leave hand-built T0..T31 below the bump */
  ir->variables_live_intervals_size = 64;
  ir->parameters_live_intervals_size = 64;
  return ir;
}

/* Build a fake static/extern global symbol with a token + type flags. */
static inline void utb_init_sym(Sym *s, int tok, int vt_flags)
{
  memset(s, 0, sizeof(*s));
  s->v = tok;
  s->type.t = VT_INT | vt_flags;
}

/* Build an lval SYMREF operand (a *(GlobalSym)*** address) for a LOAD/STORE,
 * with an explicit addend so two reads of distinct fields differ. */
static inline IROperand utb_symref_lval(TCCIRState *ir, Sym *sym, int32_t addend, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
  return irop_make_symref(0, sidx, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, btype);
}

/* Build a non-lval SYMREF operand (the address value GlobalSym+off) for an
 * ADD src1 — the shape globalsym_cse hoists. */
static inline IROperand utb_symref_addr(TCCIRState *ir, Sym *sym, int32_t addend, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
  return irop_make_symref(0, sidx, /*is_lval*/ 0, /*is_local*/ 0, /*is_const*/ 0, btype);
}

/* ---------------------------------------------------- cse_global_load */

/* POSITIVE: two LOADs of the same (non-written, non-volatile) global in the
 * same straight-line block — the second becomes ASSIGN from the first's dest. */
UT_TEST(test_cse_global_load_dedups_second_load)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 40, VT_STATIC);

  int l0 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, l0), TCCIR_OP_LOAD);
  /* Second load rewritten to ASSIGN T2 <- T1 (the first load's dest vreg). */
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, l1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD: different addends (different struct members of the same base) are NOT
 * the same value, so the second load is preserved. */
UT_TEST(test_cse_global_load_distinct_addend_not_deduped)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 41, VT_STATIC);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 4, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* GUARD (intervening store to same global): a STORE to the global is collected
 * into written_globals, so loads of it are excluded from CSE entirely. */
UT_TEST(test_cse_global_load_store_to_same_global_blocks)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 42, VT_STATIC);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_symref_lval(ir, &g, 0, I32), utb_temp(9, I32), UTB_NONE);
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* GUARD (volatile global): each volatile read must be re-emitted; no CSE. */
UT_TEST(test_cse_global_load_volatile_not_deduped)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 43, VT_STATIC | VT_VOLATILE);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* CROSS-BB: a non-static (extern-visible) global is only tracked within a BB.
 * After a jump target (new BB), the cached load is cleared, so the second
 * load in the new block is preserved. */
UT_TEST(test_cse_global_load_extern_not_tracked_across_bb)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 44, 0 /* extern-visible: no VT_STATIC */);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int mid = utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(1, I32), utb_imm(1, I32));
  ir->compact_instructions[mid].is_jump_target = 1; /* start of a new BB */
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* CROSS-BB STATIC: a static global survives across a BB boundary (no STORE,
 * no call), so the load in the next block still CSEs against the first. */
UT_TEST(test_cse_global_load_static_tracked_across_bb)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 45, VT_STATIC);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int mid = utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(1, I32), utb_imm(1, I32));
  ir->compact_instructions[mid].is_jump_target = 1;
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);

  int changes = tcc_ir_opt_cse_global_load(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, l1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- globalsym_cse */

/* POSITIVE: 3 ADDs use the same GlobalSym+off as src1 -> the base is hoisted
 * into a leading ASSIGN T_base <- GlobalSym and the 3 ADDs' src1 become T_base.
 * (count threshold for hoisting is 3.) */
UT_TEST(test_globalsym_cse_hoists_repeated_base)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 50, 0);

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(0, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(4, I32));
  int a2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_globalsym_cse(ir);

  /* One hoisted ASSIGN inserted at index 0 shifts every original ADD by one. */
  UT_ASSERT(changes >= 3);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  int32_t base_vr = utb_vreg(utb_dest(ir, 0));
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(base_vr), TCCIR_VREG_TYPE_TEMP);
  /* All three ADD src1 operands now reference the hoisted base, not a SYMREF. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a0 + 1)), base_vr);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a1 + 1)), base_vr);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a2 + 1)), base_vr);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, a0 + 1)), IROP_TAG_VREG);

  utb_free(ir);
  return 0;
}

/* GUARD (below threshold): only 2 uses of the base -> count < 3, nothing is
 * hoisted, and the SYMREF src1 operands are left in place. */
UT_TEST(test_globalsym_cse_below_threshold_no_hoist)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 51, 0);

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(0, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(4, I32));

  int n_before = ir->next_instruction_index;
  int changes = tcc_ir_opt_globalsym_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, a0)), IROP_TAG_SYMREF);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, a1)), IROP_TAG_SYMREF);

  utb_free(ir);
  return 0;
}

/* GUARD (lval use disqualifies hoist): if the symbol also appears as an lval
 * (LOAD src1) the entry is flagged has_lval and is never hoisted, so the ADD
 * src1 SYMREFs stay even with 3 ADD uses. */
UT_TEST(test_globalsym_cse_lval_use_blocks_hoist)
{
  TCCIRState *ir = utb_new_sym();
  static Sym g;
  utb_init_sym(&g, 52, 0);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(7, I32), utb_symref_lval(ir, &g, 0, I32), UTB_NONE);
  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_symref_addr(ir, &g, 0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_globalsym_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, a0)), IROP_TAG_SYMREF);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- cse_param_add */

/* POSITIVE: two `P0 ADD #8` in the same block -> the second is rewritten to an
 * ASSIGN of the first's result. */
UT_TEST(test_cse_param_add_dedups_repeated_offset)
{
  TCCIRState *ir = utb_new();
  utb_set_vreg_validity(ir, 64);

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(8, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_param(0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_cse_param_add(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* ADD/SUB canonicalization: `P0 ADD #8` then `P0 SUB #-8` are the same value
 * (key encodes SUB as negated imm), so the SUB folds to an ASSIGN of the ADD. */
UT_TEST(test_cse_param_add_sub_negation_matches)
{
  TCCIRState *ir = utb_new();
  utb_set_vreg_validity(ir, 64);

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(8, I32));
  int a1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_param(0, I32), utb_imm(-8, I32));

  int changes = tcc_ir_opt_cse_param_add(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD (non-PARAM src): the pass only CSEs PARAM (or stackoff-lval) bases;
 * a VAR base must NOT be deduped. */
UT_TEST(test_cse_param_add_var_base_not_deduped)
{
  TCCIRState *ir = utb_new();
  utb_set_vreg_validity(ir, 64);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(8, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_cse_param_add(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* GUARD (PARAM redefined between uses): writing P0 invalidates the cached
 * `P0 ADD #8`, so the later identical ADD is not deduped. */
UT_TEST(test_cse_param_add_invalidated_by_param_write)
{
  TCCIRState *ir = utb_new();
  utb_set_vreg_validity(ir, 64);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_param(0, I32), utb_temp(9, I32), UTB_NONE);
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_param(0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_cse_param_add(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* GUARD (BB boundary): a jump between the two uses clears the CSE table, so the
 * second `P0 ADD #8` after the merge point is not deduped. */
UT_TEST(test_cse_param_add_cleared_across_bb)
{
  TCCIRState *ir = utb_new();
  utb_set_vreg_validity(ir, 64);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_param(0, I32), utb_imm(8, I32));
  ir->compact_instructions[a1].is_jump_target = 1;

  int changes = tcc_ir_opt_cse_param_add(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- local_load_cse */

/* POSITIVE: two lval ASSIGN loads of the same VAR in the same block -> the
 * second load is NOP'd and downstream uses of its dest are redirected to the
 * first load's TEMP. */
UT_TEST(test_local_load_cse_dedups_reload)
{
  TCCIRState *ir = utb_new();

  /* T1 <- V0***DEREF*** (load) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(0, I32)), UTB_NONE);
  /* T2 <- V0***DEREF*** (redundant reload) */
  int l2 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_lval(utb_var(0, I32)), UTB_NONE);
  /* T3 = T2 ADD #1  (use of the reloaded value) */
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_local_load_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  /* Redundant reload turned into a NOP. */
  UT_ASSERT_EQ(utb_op(ir, l2), TCCIR_OP_NOP);
  /* Downstream use of T2 rewritten to the first load's T1. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, use)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD (VAR written between loads): a store to the VAR's slot invalidates the
 * cached load, so the reload is preserved. */
UT_TEST(test_local_load_cse_invalidated_by_var_write)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(0, I32)), UTB_NONE);
  /* V0 <- T9 : direct write to V0 invalidates the cached load of V0. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(9, I32), UTB_NONE);
  int l2 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_lval(utb_var(0, I32)), UTB_NONE);

  int changes = tcc_ir_opt_local_load_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l2), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* GUARD (width mismatch): an INT8 lval load of the same VAR is a different
 * access width than an INT32 one, so it is not a CSE match. */
UT_TEST(test_local_load_cse_width_mismatch_not_deduped)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(0, I32)), UTB_NONE);
  int l2 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I8), utb_lval(utb_var(0, I8)), UTB_NONE);

  int changes = tcc_ir_opt_local_load_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l2), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* GUARD (function call between loads): a call may clobber the stack slot, so
 * the cache is flushed and the reload is preserved. */
UT_TEST(test_local_load_cse_cleared_by_call)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(0, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, utb_imm(0, I32), utb_var(99, I32), utb_imm(0, I32));
  int l2 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_lval(utb_var(0, I32)), UTB_NONE);

  int changes = tcc_ir_opt_local_load_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, l2), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- local_alu_cse */

/* POSITIVE: two identical `V0 ADD V1` in a block -> the second becomes an
 * ASSIGN of the first's dest. */
UT_TEST(test_local_alu_cse_dedups_identical_add)
{
  TCCIRState *ir = utb_new();

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_var(1, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(0, I32), utb_var(1, I32));

  int changes = tcc_ir_opt_local_alu_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* COMMUTATIVITY: `V0 ADD V1` then `V1 ADD V0` are equal (ADD commutes), so the
 * swapped second occurrence still CSEs. */
UT_TEST(test_local_alu_cse_commutative_match)
{
  TCCIRState *ir = utb_new();

  int a0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_var(1, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(1, I32), utb_var(0, I32));

  int changes = tcc_ir_opt_local_alu_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, a1)), VR_TMP(1));

  utb_free(ir);
  return 0;
}

/* GUARD (non-commutative SUB swap): `V0 SUB V1` and `V1 SUB V0` are different
 * values, so the second must NOT be deduped. */
UT_TEST(test_local_alu_cse_sub_not_commutative)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_var(0, I32), utb_var(1, I32));
  int a1 = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_var(1, I32), utb_var(0, I32));

  int changes = tcc_ir_opt_local_alu_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_SUB);

  utb_free(ir);
  return 0;
}

/* GUARD (operand redefined): redefining V0 between the two `V0 ADD V1` ops
 * changes its value, so the cached entry is killed and no CSE happens. */
UT_TEST(test_local_alu_cse_invalidated_by_operand_redef)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_var(1, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(9, I32), UTB_NONE);
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(0, I32), utb_var(1, I32));

  int changes = tcc_ir_opt_local_alu_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* GUARD (BB boundary): a jump target between the two ALU ops resets the cache
 * (entries don't survive across BBs), so no CSE. */
UT_TEST(test_local_alu_cse_cleared_across_bb)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_var(1, I32));
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(0, I32), utb_var(1, I32));
  ir->compact_instructions[a1].is_jump_target = 1;

  int changes = tcc_ir_opt_local_alu_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- bool_cse */

/* POSITIVE: `T0 && T1` computed twice -> the second BOOL_AND becomes an ASSIGN
 * of the first's result. */
UT_TEST(test_bool_cse_dedups_repeated_and)
{
  TCCIRState *ir = utb_new();

  int b0 = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int b1 = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_bool_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, b0), TCCIR_OP_BOOL_AND);
  UT_ASSERT_EQ(utb_op(ir, b1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, b1)), VR_TMP(2));

  utb_free(ir);
  return 0;
}

/* COMMUTATIVITY: `T0 && T1` then `T1 && T0` hash to the same key (operands
 * sorted), so the swapped AND still CSEs. */
UT_TEST(test_bool_cse_commutative_operands)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int b1 = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(3, I32), utb_temp(1, I32), utb_temp(0, I32));

  int changes = tcc_ir_opt_bool_cse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, b1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, b1)), VR_TMP(2));

  utb_free(ir);
  return 0;
}

/* GUARD (different operator): a BOOL_OR over the same operands is a distinct
 * key from a BOOL_AND, so it is not deduped against it. */
UT_TEST(test_bool_cse_and_or_distinct_ops)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int b1 = utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_bool_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, b1), TCCIR_OP_BOOL_OR);

  utb_free(ir);
  return 0;
}

/* GUARD (cleared at call): a function call between the two ANDs clears the bool
 * CSE table, so the second is preserved. */
UT_TEST(test_bool_cse_cleared_by_call)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, utb_imm(0, I32), utb_var(99, I32), utb_imm(0, I32));
  int b1 = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_bool_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, b1), TCCIR_OP_BOOL_AND);

  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE: after deduping, a second bool_cse pass reports no changes. */
UT_TEST(test_bool_cse_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));

  int c1 = tcc_ir_opt_bool_cse(ir);
  int c2 = tcc_ir_opt_bool_cse(ir);

  UT_ASSERT_EQ(c1, 1);
  UT_ASSERT_EQ(c2, 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_copyprop)
{
  UT_COVERS("copy_prop");
  UT_COVERS("cse_global_load");
  UT_COVERS("globalsym_cse");
  UT_COVERS("cse_param_add");
  UT_COVERS("local_load_cse");
  UT_COVERS("local_alu_cse");
  UT_COVERS("bool_cse");
  UT_RUN(test_copyprop_var_copy_propagates_to_add);
  UT_RUN(test_copyprop_tmp_copy_propagates_both_operands);
  UT_RUN(test_copyprop_lval_use_preserves_deref_and_width);
  UT_RUN(test_copyprop_lval_use_var_source_not_propagated);
  UT_RUN(test_copyprop_lval_source_assign_not_recorded);
  UT_RUN(test_copyprop_const_source_not_recorded);
  UT_RUN(test_copyprop_source_redef_blocks_propagation);
  UT_RUN(test_copyprop_btype_mismatch_not_recorded);
  UT_RUN(test_copyprop_store_dest_tmp_source_propagates);
  UT_RUN(test_copyprop_param_copy_propagates_to_add);
  UT_RUN(test_copyprop_copy_chain_var_through_tmp);
  UT_RUN(test_copyprop_copy_chain_three_links);
  UT_RUN(test_copyprop_lval_src2_preserves_deref_and_width);
  UT_RUN(test_copyprop_store_dest_param_source_propagates);
  UT_RUN(test_copyprop_store_dest_param_llocal_source_not_propagated);
  UT_RUN(test_copyprop_copy_cleared_across_func_call);
  UT_RUN(test_copyprop_copy_cleared_at_merge_point);
  UT_RUN(test_copyprop_tmp_source_redef_blocks_propagation);
  UT_RUN(test_copyprop_int8_btype_mismatch_not_recorded);
  UT_RUN(test_copyprop_stackoff_source_not_recorded);
  UT_RUN(test_copyprop_self_copy);
  UT_RUN(test_copyprop_empty_ir_returns_zero);
  UT_RUN(test_copyprop_single_instruction_no_copy_returns_zero);
  UT_RUN(test_copyprop_idempotent_after_chain);

  /* cse_global_load */
  UT_RUN(test_cse_global_load_dedups_second_load);
  UT_RUN(test_cse_global_load_distinct_addend_not_deduped);
  UT_RUN(test_cse_global_load_store_to_same_global_blocks);
  UT_RUN(test_cse_global_load_volatile_not_deduped);
  UT_RUN(test_cse_global_load_extern_not_tracked_across_bb);
  UT_RUN(test_cse_global_load_static_tracked_across_bb);

  /* globalsym_cse */
  UT_RUN(test_globalsym_cse_hoists_repeated_base);
  UT_RUN(test_globalsym_cse_below_threshold_no_hoist);
  UT_RUN(test_globalsym_cse_lval_use_blocks_hoist);

  /* cse_param_add */
  UT_RUN(test_cse_param_add_dedups_repeated_offset);
  UT_RUN(test_cse_param_add_sub_negation_matches);
  UT_RUN(test_cse_param_add_var_base_not_deduped);
  UT_RUN(test_cse_param_add_invalidated_by_param_write);
  UT_RUN(test_cse_param_add_cleared_across_bb);

  /* local_load_cse */
  UT_RUN(test_local_load_cse_dedups_reload);
  UT_RUN(test_local_load_cse_invalidated_by_var_write);
  UT_RUN(test_local_load_cse_width_mismatch_not_deduped);
  UT_RUN(test_local_load_cse_cleared_by_call);

  /* local_alu_cse */
  UT_RUN(test_local_alu_cse_dedups_identical_add);
  UT_RUN(test_local_alu_cse_commutative_match);
  UT_RUN(test_local_alu_cse_sub_not_commutative);
  UT_RUN(test_local_alu_cse_invalidated_by_operand_redef);
  UT_RUN(test_local_alu_cse_cleared_across_bb);

  /* bool_cse */
  UT_RUN(test_bool_cse_dedups_repeated_and);
  UT_RUN(test_bool_cse_commutative_operands);
  UT_RUN(test_bool_cse_and_or_distinct_ops);
  UT_RUN(test_bool_cse_cleared_by_call);
  UT_RUN(test_bool_cse_idempotent);
}
