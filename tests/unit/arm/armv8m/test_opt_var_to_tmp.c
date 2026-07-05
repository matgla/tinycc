/*
 *  test_opt_var_to_tmp.c - suite for ir/opt_promote.c (var_to_tmp promotion)
 *
 *  tcc_ir_opt_var_to_tmp() promotes a single-definition INT32 local VAR to a
 *  TEMP when every lval read of that VAR is either:
 *    - ASSIGN T <- V[lval]   (a reload into a TEMP), or
 *    - FUNCPARAMVAL / FUNCPARAMVOID V[lval]   (passing the value to a call).
 *  The pass rewrites the defining instruction's destination to a freshly
 *  allocated TEMP and redirects each qualifying read to copy from that TEMP.
 *
 *  The promotion is blocked by: multiple definitions, non-lval or src2 uses,
 *  non-INT32 btype, address-taken / complex / llong / float interval flags,
 *  an unsupported defining opcode, control-flow boundaries, calls, or a
 *  redefinition of the VAR between the def and a use.
 *
 *  Isolated tests: a hand-built IR sequence is run through the bare pass entry
 *  point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here to avoid pulling
 * in the optimizer engine headers). */
int tcc_ir_opt_var_to_tmp(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I8  IROP_BTYPE_INT8

#define PROMO_TMP 4

#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))

/* var_to_tmp dereferences ir->variables_live_intervals[pos] for every VAR
 * destination it considers.  utb_new() zeroes that pointer/size, which would
 * make tcc_ir_get_live_interval() exit(1).  Allocate a zeroed interval table
 * large enough for all VAR positions a test uses. */
static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* The pass allocates a fresh TEMP via tcc_ir_vreg_alloc_temp(), which needs a
 * non-empty temporary-variables interval table. */
static void utb_alloc_temp_intervals(TCCIRState *ir, int count)
{
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->temporary_variables_live_intervals_size = count;
}

/* ========================================================= positive cases */

/* POSITIVE: two reloads of a single-def VAR are rewritten to copy from the
 * promoted temp.
 *   V1 = ASSIGN #5
 *   T0 = ASSIGN V1[lval]
 *   T1 = ASSIGN V1[lval]
 * After promotion the def targets the fresh temp and both uses read it. */
UT_TEST(test_var_to_tmp_two_reloads_promoted)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  int idef = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  int iuse0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);
  int iuse1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, idef)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse0)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse1)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: the defining opcode may be any arithmetic op in the allowlist.
 *   V1 = ADD #2, #1
 *   T1 = ASSIGN V1[lval]
 * The def's destination is rewritten to the promoted temp. */
UT_TEST(test_var_to_tmp_arith_def_promoted)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  int idef = utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_imm(2, I32), utb_imm(1, I32));
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, idef), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, idef)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: FUNCPARAMVAL reads are also rewritten to use the promoted temp.
 *   V1 = ASSIGN #7
 *   FUNCPARAMVAL V1[lval]
 * The parameter source becomes the promoted temp. */
UT_TEST(test_var_to_tmp_funcparamval_promoted)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(7, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_TMP(PROMO_TMP));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ========================================================= guard cases */

/* GUARD: a VAR with more than one definition is not promoted. */
UT_TEST(test_var_to_tmp_multiple_defs_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(2, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: a non-lval use of the VAR disqualifies promotion. */
UT_TEST(test_var_to_tmp_nonlval_use_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_var(1, I32), utb_imm(1, I32));
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: a VAR appearing in src2 disqualifies promotion. */
UT_TEST(test_var_to_tmp_src2_use_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(1, I32), utb_var(1, I32));
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: only INT32 scalars are promoted; narrower types are rejected. */
UT_TEST(test_var_to_tmp_non_int32_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I8), utb_imm(5, I8), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I8), utb_lval(utb_var(1, I8)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: address-taken, complex, long-long, and float interval flags each
 * prevent promotion. */
UT_TEST(test_var_to_tmp_interval_flags_blocked)
{
  for (int f = 0; f < 4; ++f)
  {
    TCCIRState *ir = utb_new();
    utb_alloc_var_intervals(ir, 4);
    utb_alloc_temp_intervals(ir, 16);
    ir->next_temporary_variable = PROMO_TMP;

    ir->variables_live_intervals[1].addrtaken = (f == 0);
    ir->variables_live_intervals[1].is_complex = (f == 1);
    ir->variables_live_intervals[1].is_llong   = (f == 2);
    ir->variables_live_intervals[1].is_float   = (f == 3);

    utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
    int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

    int changes = tcc_ir_opt_var_to_tmp(ir);

    UT_ASSERT_EQ(changes, 0);
    UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

    utb_free(ir);
  }
  return 0;
}

/* GUARD: defining opcodes outside the allowlist (e.g. STORE) prevent
 * promotion. */
UT_TEST(test_var_to_tmp_store_def_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(1, I32)), utb_imm(7, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: a use that is the target of a jump (BB boundary) is not promoted. */
UT_TEST(test_var_to_tmp_use_across_jump_target_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: a call between the def and the use aborts the scan. */
UT_TEST(test_var_to_tmp_call_between_def_use_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_imm(0, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* GUARD: a redefinition of the VAR between the def and the use aborts
 * promotion. */
UT_TEST(test_var_to_tmp_redef_between_def_use_blocked)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 4);
  utb_alloc_temp_intervals(ir, 16);
  ir->next_temporary_variable = PROMO_TMP;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);
  int iuse = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_lval(utb_var(1, I32)), UTB_NONE);

  int changes = tcc_ir_opt_var_to_tmp(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, iuse)), VR_VAR(1));

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_var_to_tmp)
{
  UT_COVERS("var_to_tmp");

  UT_RUN(test_var_to_tmp_two_reloads_promoted);
  UT_RUN(test_var_to_tmp_arith_def_promoted);
  UT_RUN(test_var_to_tmp_funcparamval_promoted);
  UT_RUN(test_var_to_tmp_multiple_defs_blocked);
  UT_RUN(test_var_to_tmp_nonlval_use_blocked);
  UT_RUN(test_var_to_tmp_src2_use_blocked);
  UT_RUN(test_var_to_tmp_non_int32_blocked);
  UT_RUN(test_var_to_tmp_interval_flags_blocked);
  UT_RUN(test_var_to_tmp_store_def_blocked);
  UT_RUN(test_var_to_tmp_use_across_jump_target_blocked);
  UT_RUN(test_var_to_tmp_call_between_def_use_blocked);
  UT_RUN(test_var_to_tmp_redef_between_def_use_blocked);
}
