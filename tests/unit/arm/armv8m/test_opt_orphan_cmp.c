/*
 *  test_opt_orphan_cmp.c - suite for ir/opt_dce.c orphan_cmp_elim
 *
 *  tcc_ir_opt_orphan_cmp_elim() removes CMP / TEST_ZERO / flag-setting
 *  soft-float helper calls whose condition-code result is never consumed by a
 *  SETIF, JUMPIF or SELECT before the next flag-clobbering instruction or the
 *  end of the function.  Such orphans are left behind when branch/cmp folds
 *  NOP their consumers while the flag-setting instruction itself survives
 *  ordinary DCE (it has no temp dest, only a side-effect on flags).
 *
 *  Isolated tests: hand-built IR is run through the bare pass entry point and
 *  the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_orphan_cmp_elim(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Bound large enough for encoded vreg values (type<<28 | position). */
#define UTB_VREG_BOUND 0x30000010

/* Tokens used to name symref callees via the harness get_tok_str table. */
#define TOK_FCMP 101

/* ----------------------------------------------------------------- helpers */

/* Emit an unconditional JUMP to target index `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* Emit a JUMPIF (conditional branch).  The condition-code operand value is not
 * interpreted by the pass; only the opcode matters. */
static int emit_jumpif(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_imm(0, I32), UTB_NONE);
}

/* Emit Tdst = SETIF <cc>.  The condition code is not interpreted. */
static int emit_setif(TCCIRState *ir, int dst_tmp)
{
  return utb_emit(ir, TCCIR_OP_SETIF, utb_temp(dst_tmp, I32), utb_imm(0, I32), UTB_NONE);
}

/* Build a SYMREF callee operand whose token is `tok`.  Caller must have called
 * utb_pools_init(ir) first. */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: a CMP whose flags are never consumed before the end of the function
 * is dead and becomes NOP.
 *
 *   0: CMP T0, #1
 *   1: RETURNVOID
 */
UT_TEST(test_orphan_cmp_removed_at_end_of_function)
{
  TCCIRState *ir = utb_new();

  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a CMP followed immediately by a SETIF consumer stays alive.
 *
 *   0: CMP T0, #1
 *   1: T1 = SETIF
 *   2: RETURNVALUE T1
 */
UT_TEST(test_orphan_cmp_kept_by_setif_consumer)
{
  TCCIRState *ir = utb_new();

  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  emit_setif(ir, 1);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a CMP followed by a JUMPIF consumer stays alive.
 *
 *   0: CMP T0, #1
 *   1: JUMPIF -> 3
 *   2: RETURNVOID
 *   3: RETURNVOID
 */
UT_TEST(test_orphan_cmp_kept_by_jumpif_consumer)
{
  TCCIRState *ir = utb_new();

  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  emit_jumpif(ir, 3);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: flags propagate across an unconditional JUMP; if no consumer is
 * reached, the original CMP is orphan.
 *
 *   0: CMP T0, #1
 *   1: JUMP -> 3
 *   2: RETURNVOID
 *   3: RETURNVOID
 */
UT_TEST(test_orphan_cmp_removed_across_uncond_jump)
{
  TCCIRState *ir = utb_new();

  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a CMP that is a jump target is never eliminated, because alternate
 * predecessors may rely on reaching this instruction.
 *
 *   0: JUMP -> 2
 *   1: RETURNVOID
 *   2: CMP T0, #1   (jump target)
 *   3: RETURNVOID
 */
UT_TEST(test_orphan_cmp_kept_at_jump_target)
{
  TCCIRState *ir = utb_new();

  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  ir->compact_instructions[ic].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: TEST_ZERO with no downstream consumer is orphan.
 *
 *   0: TEST_ZERO T0
 *   1: RETURNVOID
 */
UT_TEST(test_orphan_test_zero_removed)
{
  TCCIRState *ir = utb_new();

  int iz = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, iz), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: TEST_ZERO with a SETIF consumer stays.
 *
 *   0: TEST_ZERO T0
 *   1: T1 = SETIF
 *   2: RETURNVALUE T1
 */
UT_TEST(test_orphan_test_zero_kept_by_setif)
{
  TCCIRState *ir = utb_new();

  int iz = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  emit_setif(ir, 1);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, iz), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: an __aeabi_cfcmple FUNCCALLVOID whose flag result is not consumed is
 * eliminated, and its PARAM instructions are NOP'd too.
 *
 *   0: PARAM T0, (call=1, idx=0)
 *   1: PARAM T1, (call=1, idx=1)
 *   2: FUNCCALLVOID __aeabi_cfcmple, (call=1, argc=2)
 *   3: RETURNVOID
 */
UT_TEST(test_orphan_flag_helper_call_removed)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  int p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, p0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, p1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: an __aeabi_cfcmple FUNCCALLVOID followed by a SETIF consumer stays,
 * and so do its parameters.
 *
 *   0: PARAM T0, (call=1, idx=0)
 *   1: PARAM T1, (call=1, idx=1)
 *   2: FUNCCALLVOID __aeabi_cfcmple, (call=1, argc=2)
 *   3: T2 = SETIF
 *   4: RETURNVALUE T2
 */
UT_TEST(test_orphan_flag_helper_call_kept_by_setif)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym helper;
  IROperand callee = utb_callee_named(ir, &helper, TOK_FCMP);
  utb_set_tok_str(TOK_FCMP, "__aeabi_cfcmple");

  int p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
                    utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));
  emit_setif(ir, 2);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, p0), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_op(ir, p1), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: two consecutive CMPs with no consumer leave both orphans — the
 * first is clobbered by the second, and the second reaches RETURNVOID.
 *
 *   0: CMP T0, #1
 *   1: CMP T1, #2
 *   2: RETURNVOID
 */
UT_TEST(test_orphan_cmp_two_cmps_no_consumer_removed)
{
  TCCIRState *ir = utb_new();

  int ic1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  int ic2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, ic1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ic2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: RETURNVALUE clobbers flags before any consumer, so the preceding CMP
 * is orphan.
 *
 *   0: CMP T0, #1
 *   1: RETURNVALUE T0
 */
UT_TEST(test_orphan_cmp_before_returnvalue_removed)
{
  TCCIRState *ir = utb_new();

  int ic = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ic), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* CONVERGENCE / IDEMPOTENCE: a single pass removes the orphan; a second pass
 * reports zero additional changes. */
UT_TEST(test_orphan_cmp_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_orphan_cmp_elim, 5);

  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(tcc_ir_opt_orphan_cmp_elim(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, UTB_VREG_BOUND), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------- suite */

UT_SUITE(opt_orphan_cmp)
{
  UT_COVERS("orphan_cmp");

  UT_RUN(test_orphan_cmp_removed_at_end_of_function);
  UT_RUN(test_orphan_cmp_kept_by_setif_consumer);
  UT_RUN(test_orphan_cmp_kept_by_jumpif_consumer);
  UT_RUN(test_orphan_cmp_removed_across_uncond_jump);
  UT_RUN(test_orphan_cmp_kept_at_jump_target);
  UT_RUN(test_orphan_test_zero_removed);
  UT_RUN(test_orphan_test_zero_kept_by_setif);
  UT_RUN(test_orphan_flag_helper_call_removed);
  UT_RUN(test_orphan_flag_helper_call_kept_by_setif);
  UT_RUN(test_orphan_cmp_two_cmps_no_consumer_removed);
  UT_RUN(test_orphan_cmp_before_returnvalue_removed);
  UT_RUN(test_orphan_cmp_idempotent);
}
