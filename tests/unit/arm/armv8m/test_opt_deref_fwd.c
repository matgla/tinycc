/*
 *  test_opt_deref_fwd.c - suite for ir/opt_memory.c (deref forwarding)
 *
 *  tcc_ir_opt_deref_fwd() forwards a LOAD/ASSIGN/STORE dereference result into
 *  an immediately following CMP operand when both instructions are adjacent
 *  (or separated only by NOPs) and live in the same basic block.
 *
 *      i:   T0 = LOAD V0[lval]          (deref of V0)
 *      j:   CMP T1, V0[lval]            => CMP T1, T0
 *
 *  The pass also handles the symmetric case where the deref appears in src1 of
 *  the CMP, and the STORE case where the value just written is forwarded.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here to avoid pulling
 * in the optimizer engine headers). */
int tcc_ir_opt_deref_fwd(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Build a dereferenced vreg operand (the operand's is_lval flag is set). */
static inline IROperand utb_deref_vreg(IROperand op)
{
  return utb_lval(op);
}

/* ========================================================== positive cases */

/* POSITIVE: LOAD result forwards into src2 of the next CMP.
 *   T0 = LOAD V0[lval]
 *   CMP T1, V0[lval]   ->  CMP T1, T0
 */
UT_TEST(test_deref_fwd_load_to_cmp_src2)
{
  TCCIRState *ir = utb_new();

  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32),
                       utb_deref_vreg(utb_var(0, I32)), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32),
                      utb_deref_vreg(utb_var(0, I32)));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_LOAD); /* unchanged */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(ir, icmp).is_lval, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, icmp)), utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: ASSIGN result forwards into src1 of the next CMP.
 *   T0 = ASSIGN V0[lval]
 *   CMP V0[lval], T1   ->  CMP T0, T1
 */
UT_TEST(test_deref_fwd_assign_to_cmp_src1)
{
  TCCIRState *ir = utb_new();

  int iassign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                         utb_deref_vreg(utb_var(0, I32)), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                      utb_deref_vreg(utb_var(0, I32)), utb_temp(1, I32));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, iassign), TCCIR_OP_ASSIGN); /* unchanged */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, icmp)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src1(ir, icmp).is_lval, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: STORE value forwards into src2 of the next CMP.
 *   STORE V0[lval] <- T2
 *   CMP T1, V0[lval]   ->  CMP T1, T2
 */
UT_TEST(test_deref_fwd_store_to_cmp_src2)
{
  TCCIRState *ir = utb_new();

  int istore = utb_emit(ir, TCCIR_OP_STORE, utb_deref_vreg(utb_var(0, I32)),
                        utb_temp(2, I32), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32),
                      utb_deref_vreg(utb_var(0, I32)));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, istore), TCCIR_OP_STORE); /* unchanged */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_temp(2, I32)));
  UT_ASSERT_EQ(utb_src2(ir, icmp).is_lval, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, icmp)), utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ============================================================ guard cases */

/* GUARD: a jump target between the load and the CMP blocks forwarding.
 * The CMP itself is the target of an earlier JUMP, so a path exists that
 * reaches the CMP without executing the load.
 *
 *   ADD T3, #0, #0
 *   JUMP -> L1
 *   T0 = LOAD V0[lval]
 *   NOP
 * L1:
 *   CMP T1, V0[lval]   (unchanged)
 */
UT_TEST(test_deref_fwd_jump_target_blocks)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_imm(0, I32), utb_imm(0, I32));
  int ijump = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32),
                       utb_deref_vreg(utb_var(0, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32),
                      utb_deref_vreg(utb_var(0, I32)));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_var(0, I32)));
  UT_ASSERT_EQ(utb_src2(ir, icmp).is_lval, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a CMP deref that uses a different vreg is left untouched.
 *   T0 = LOAD V0[lval]
 *   CMP T1, V1[lval]   (unchanged)
 */
UT_TEST(test_deref_fwd_nonmatching_vreg)
{
  TCCIRState *ir = utb_new();

  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32),
                       utb_deref_vreg(utb_var(0, I32)), UTB_NONE);
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32),
                      utb_deref_vreg(utb_var(1, I32)));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_var(1, I32)));
  UT_ASSERT_EQ(utb_src2(ir, icmp).is_lval, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a real (non-NOP) instruction between the load and the CMP blocks
 * forwarding.  The pass only skips over NOPs.
 *   T0 = LOAD V0[lval]
 *   T3 = ADD #1, #2
 *   CMP T1, V0[lval]   (unchanged)
 */
UT_TEST(test_deref_fwd_real_instruction_blocks)
{
  TCCIRState *ir = utb_new();

  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32),
                       utb_deref_vreg(utb_var(0, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_imm(1, I32), utb_imm(2, I32));
  int icmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32),
                      utb_deref_vreg(utb_var(0, I32)));

  int changes = tcc_ir_opt_deref_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, icmp)), utb_vreg(utb_var(0, I32)));
  UT_ASSERT_EQ(utb_src2(ir, icmp).is_lval, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_deref_fwd)
{
  UT_COVERS("deref_fwd");

  UT_RUN(test_deref_fwd_load_to_cmp_src2);
  UT_RUN(test_deref_fwd_assign_to_cmp_src1);
  UT_RUN(test_deref_fwd_store_to_cmp_src2);
  UT_RUN(test_deref_fwd_jump_target_blocks);
  UT_RUN(test_deref_fwd_nonmatching_vreg);
  UT_RUN(test_deref_fwd_real_instruction_blocks);
}
