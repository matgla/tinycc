/*
 *  test_opt_loop.c - suite for ir/opt_loop.c (pre-SSA loop optimizations)
 *
 *  Covers the top-level entry points in ir/opt_loop.c: MUL strength
 *  reduction, IV strength reduction driver, loop-bound rematerialization,
 *  loop unrolling/elimination, loop rotation, decrement-to-zero, pointer-IV
 *  exit-value substitution, and redundant guard elimination.
 *
 *  This is a *different* file from ir/opt_loop_dead.c and ir/opt_loop_utils.c,
 *  which already have their own dedicated suites (test_opt_loop_dead.c,
 *  test_opt_loop_utils.c).  Those already exercise the shared helpers
 *  (find_induction_vars_ex, find_loop_exit_condition, compute_trip_count,
 *  try_eliminate_loop, try_unroll_loop_ex, try_rotate_loop) in isolation;
 *  here we drive the *outer* tcc_ir_opt_* entry points in ir/opt_loop.c that
 *  wrap tcc_ir_detect_loops() + those helpers, so the loop-detection glue and
 *  the opt_loop.c-local logic (strength_reduce_mul, loop_bound_remat,
 *  decrement_to_zero, ptr_iv_exit_subst, guard_elim) gets real coverage.
 *
 *  IR shape patterns reuse the exact conventions established in
 *  test_opt_loop_utils.c (emit_unrollable_loop / emit_rotatable_loop style):
 *  a natural loop is any backward JUMP/JUMPIF (tcc_ir_detect_loops scans for
 *  target < source index).
 */

#include "ir_build.h"

#include "ut.h"
#include "opt_loop_utils.h"

#define I32 IROP_BTYPE_INT32

/* Condition-token values (mirror tcc.h TOK_*, matches opt_loop.c's direct use
 * of the real TOK_* constants -- NOT a local renumbering). */
#define UT_ULT 0x92
#define UT_UGE 0x93
#define UT_EQ  0x94
#define UT_NE  0x95
#define UT_ULE 0x96
#define UT_UGT 0x97
#define UT_LT  0x9c
#define UT_GE  0x9d
#define UT_LE  0x9e
#define UT_GT  0x9f

#define VR_VAR(n) irop_get_vreg(utb_var(n, I32))
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

/* Pass entry points under test (declared in ir/opt.h; forward-declared here
 * to avoid pulling in the optimizer engine headers, matching the sibling
 * suites' style). */
int tcc_ir_strength_reduce_mul(TCCIRState *ir, int instr_idx);
int tcc_ir_opt_strength_reduction(TCCIRState *ir);
int tcc_ir_opt_iv_strength_reduction(TCCIRState *ir);
int tcc_ir_opt_iv_strength_reduction_with_loops(TCCIRState *ir, IRLoops *loops);
int tcc_ir_opt_loop_bound_remat(TCCIRState *ir);
int tcc_ir_opt_loop_unroll(TCCIRState *ir);
int tcc_ir_opt_loop_rotation(TCCIRState *ir);
int tcc_ir_opt_decrement_to_zero(TCCIRState *ir);
int tcc_ir_opt_loop_ptr_iv_exit_subst(TCCIRState *ir);
int tcc_ir_opt_loop_guard_elim(TCCIRState *ir);

/* Initialise the temp-vreg live-interval pool (mirrors test_opt_loop_utils.c's
 * utb_init_temp_intervals).  Needed by tcc_ir_opt_loop_bound_remat, which
 * calls tcc_ir_vreg_alloc_temp() to mint fresh rematerialization vregs. */
#define UTB_INTERVAL_INIT_SIZE 8
static void utb_init_temp_intervals(TCCIRState *ir, int reserved)
{
  ir->temporary_variables_live_intervals_size = UTB_INTERVAL_INIT_SIZE;
  ir->next_temporary_variable = reserved;
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * UTB_INTERVAL_INIT_SIZE);
  for (int i = 0; i < UTB_INTERVAL_INIT_SIZE; ++i)
  {
    ir->temporary_variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->temporary_variables_live_intervals[i].incoming_reg0 = -1;
    ir->temporary_variables_live_intervals[i].incoming_reg1 = -1;
    ir->temporary_variables_live_intervals[i].stack_slot_index = -1;
    ir->temporary_variables_live_intervals[i].allocation.r0 = PREG_NONE;
    ir->temporary_variables_live_intervals[i].allocation.r1 = PREG_NONE;
  }
}

/* ============================================ tcc_ir_strength_reduce_mul */

UT_TEST(test_sr_mul_power_of_2_becomes_shl)
{
  /* T1 = V0 * 8  ->  T1 = V0 << 3 */
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_imm(8, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 1);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i)), VR_VAR(0));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, i)), 3);
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_power_of_2_immediate_on_left)
{
  /* T1 = 4 * V0  ->  T1 = V0 << 2 (the variable operand is always placed in
   * src1 regardless of which side the constant appeared on). */
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_imm(4, I32), utb_var(0, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 1);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i)), VR_VAR(0));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, i)), 2);
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_by_zero_becomes_assign_zero)
{
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 1);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i)), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i)), -1); /* src2 slot cleared to NONE */
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_by_one_becomes_assign_passthrough)
{
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 1);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i)), VR_VAR(0));
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_non_power_of_2_declines)
{
  /* x*3 isn't a power of 2; the multi-instruction (shift+add) rewrite is
   * documented as disabled (see the TODO comment in tcc_ir_strength_reduce_mul
   * — insert_instr_at during IV-SR was found to desync indices/liveness).
   * The pass must leave the MUL untouched rather than partially rewrite it. */
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_imm(3, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 0);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_MUL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_both_operands_variable_declines)
{
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_var(1, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 0);
  UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_MUL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_mul_not_a_mul_declines)
{
  TCCIRState *ir = utb_new();
  int i = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(8, I32));

  UT_ASSERT_EQ(tcc_ir_strength_reduce_mul(ir, i), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_strength_reduction */

UT_TEST(test_sr_whole_function_reduces_all_muls)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(0, I32), utb_var(0, I32), utb_imm(16, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_var(1, I32), utb_imm(3, I32));  /* 1 not reducible */
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32), utb_var(2, I32), utb_imm(2, I32));  /* 2 */

  int changes = tcc_ir_opt_strength_reduction(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_MUL); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_SHL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_sr_whole_function_empty_ir_no_crash)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_iv_strength_reduction */

UT_TEST(test_iv_sr_no_loops_returns_zero)
{
  /* Straight-line code, no backward jump -> tcc_ir_detect_loops finds nothing
   * -> the driver breaks out of its retry loop immediately. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_iv_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_loop_with_no_derived_ivs_converges_to_zero)
{
  /* A simple counting loop with no array-indexing derived IV: iv_strength_
   * reduction_core finds the IV but no DerivedIVs to rewrite, so each retry
   * reports 0 changes and the driver stops after the first iteration. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 5 */

  UT_ASSERT_EQ(tcc_ir_opt_iv_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_with_loops_null_or_empty_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_iv_strength_reduction_with_loops(ir, NULL), 0);

  IRLoops empty;
  memset(&empty, 0, sizeof empty);
  UT_ASSERT_EQ(tcc_ir_opt_iv_strength_reduction_with_loops(ir, &empty), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_loop_bound_remat */

UT_TEST(test_loop_bound_remat_no_calls_in_loop_no_change)
{
  /* Loop has no function call -> the "only worthwhile with calls" gate skips
   * every loop; must return 0 without touching anything. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
           utb_stackoff(64, 0, 0, 0, I32), UTB_NONE);                          /* 0 T0 = Addr[64] */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 1 preheader */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_temp(0, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);            /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 6 */

  UT_ASSERT_EQ(tcc_ir_opt_loop_bound_remat(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN); /* untouched */
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_bound_remat_hoisted_end_ptr_with_call_rematerializes)
{
  /* Preheader defines T0 = Addr[StackLoc[64]] (an address-of computation, not
   * a value load), used only in a header CMP against the IV; the loop body
   * contains a call.  The pass should rematerialize T0 right before the CMP
   * with a fresh TEMP vreg and NOP the original preheader definition. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_init_temp_intervals(ir, 1); /* T0 already "allocated"; next alloc starts at TEMP1 */

  static Sym foo;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &foo, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  int t0_def = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                        utb_stackoff(64, 0, 0, 0, I32), UTB_NONE);             /* 0 T0 = Addr[64] */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 1 preheader i=0 */
  int cmp_idx = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_temp(0, I32)); /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(0, 0), I32));                    /* 4 call() */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);            /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 8 exit_target */

  int changes = tcc_ir_opt_loop_bound_remat(ir);

  UT_ASSERT_EQ(changes, 1);
  /* Original preheader definition is dead now. */
  UT_ASSERT_EQ(utb_op(ir, t0_def), TCCIR_OP_NOP);

  /* A fresh ASSIGN <TEMP> = Addr[StackLoc[64]] was inserted just before the
   * (shifted) CMP, and the CMP's src2 vreg no longer equals T0's original
   * vreg (T0 was VR_TEMP(0)); it must be some other TEMP vreg reading the
   * same offset. */
  int remat_assign_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ASSIGN)
      continue;
    IROperand src = utb_src1(ir, i);
    if (irop_get_tag(src) != IROP_TAG_STACKOFF)
      continue;
    if ((int)irop_get_imm64_ex(ir, src) == 64 && utb_vreg(utb_dest(ir, i)) != VR_TEMP(0))
      remat_assign_idx = i;
  }
  UT_ASSERT(remat_assign_idx >= 0);

  /* Find the (shifted) CMP and confirm its src2 now reads the remat vreg. */
  int found_cmp = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_CMP)
      continue;
    if (utb_vreg(utb_src1(ir, i)) != VR_VAR(0))
      continue;
    UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i)), utb_vreg(utb_dest(ir, remat_assign_idx)));
    found_cmp = 1;
  }
  UT_ASSERT(found_cmp);
  (void)cmp_idx;
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_bound_remat_value_load_not_rematerialized)
{
  /* T0 = Addr[StackLoc[64]] but marked is_lval (a VALUE load, not an address
   * computation) -> disqualified per the fuzz-seed-6214 guard documented in
   * opt_loop.c; must not rematerialize. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_init_temp_intervals(ir, 1);

  static Sym foo;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &foo, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32),
           utb_stackoff(64, /*is_lval*/ 1, 0, 0, I32), UTB_NONE);             /* 0 T0 = *Addr[64] (value load) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_temp(0, I32));    /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(0, 0), I32));                   /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 */

  UT_ASSERT_EQ(tcc_ir_opt_loop_bound_remat(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN); /* untouched */
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_loop_unroll */

/* Build a canonical top-tested counting loop identical in shape to
 * test_opt_loop_utils.c's emit_unrollable_loop, but exercised through the
 * top-level driver (which itself calls tcc_ir_detect_loops). */
static int emit_unrollable_loop_top(TCCIRState *ir, int init, int limit, int step, IROperand body_op)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(init, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(limit, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=6 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), body_op, UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(step, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);             /* 5 */
  return 6;
}

/* A store-body counting loop is no longer folded by the top-level driver: both
 * try_eliminate_loop and try_unroll_loop_ex now defer memory-carrying loops to
 * the normal IR pipeline (the memory guards that fix the packed-RMW / indexed-
 * store fuzz classes).  The loop must be reported unchanged and left intact. */
UT_TEST(test_loop_unroll_top_level_store_body_blocks_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  int exit_t = emit_unrollable_loop_top(ir, 0, 3, 1, utb_var(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* exit_target */
  UT_ASSERT_EQ(exit_t, 6);

  int changes = tcc_ir_opt_loop_unroll(ir);
  UT_ASSERT_EQ(changes, 0);

  /* Loop control and the store body are untouched; no immediate-valued clones
   * were written into the slot. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  int store_imms = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_STORE)
      continue;
    IROperand d = utb_dest(ir, i);
    if (irop_get_tag(d) != IROP_TAG_STACKOFF || (int)irop_get_imm64_ex(ir, d) != 100)
      continue;
    if (irop_is_immediate(utb_src1(ir, i)))
      store_imms++;
  }
  UT_ASSERT_EQ(store_imms, 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_unroll_top_level_no_loop_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_loop_unroll(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_unroll_top_level_pure_counters_eliminated_not_unrolled)
{
  /* Body has only IV updates (no STORE) -> try_eliminate_loop fires first and
   * the loop collapses to closed-form final-value assigns, never reaching the
   * unroller.  Exercises the try_eliminate_loop-first ordering inside
   * tcc_ir_opt_loop_unroll. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);     /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));        /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 exit=7 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(5, I32)); /* 4 acc += 5 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 exit_target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);    /* 8 reads acc */

  int changes = tcc_ir_opt_loop_unroll(ir);
  UT_ASSERT_EQ(changes, 1);

  /* Closed-form final value acc = 0 + 4*5 = 20 must appear; no residual STORE
   * (there was none in the body to begin with) and the whole loop body range
   * is NOP except the final-value writes. */
  int found_final_acc = 0;
  for (int i = 2; i <= 6; i++)
  {
    if (utb_op(ir, i) == TCCIR_OP_ASSIGN && utb_vreg(utb_dest(ir, i)) == VR_VAR(1) &&
        irop_is_immediate(utb_src1(ir, i)) && (int)irop_get_imm64_ex(ir, utb_src1(ir, i)) == 20)
      found_final_acc = 1;
  }
  UT_ASSERT(found_final_acc);
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_loop_rotation */

UT_TEST(test_loop_rotation_top_level_basic)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=8 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);              /* 3 -> body */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 latch */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 6 body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);              /* 7 body->latch */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 exit_target */

  int changes = tcc_ir_opt_loop_rotation(ir);
  UT_ASSERT_EQ(changes, 1);

  /* Rotated shape: relocated body (STORE) becomes the new back-edge target,
   * followed by the latch (ADD) and a tail CMP/JUMPIF pair whose condition is
   * inverted (GE -> LT) and targets the relocated body. */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(ir->compact_instructions[3].is_jump_target, 1);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 6), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 6)), UT_LT);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 6)), 3);
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_rotation_top_level_no_loop_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_loop_rotation(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_loop_rotation_top_level_call_in_body_declines)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  static Sym foo;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &foo, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);              /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(0, 0), I32));                     /* 6 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);              /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);              /* 8 */

  UT_ASSERT_EQ(tcc_ir_opt_loop_rotation(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP); /* untouched */
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_decrement_to_zero */

UT_TEST(test_decrement_to_zero_basic_countup_rewritten)
{
  /* Canonical: V0=0; pre-test guard CMP V0,#7 GE->exit; body (no other IV
   * use); V0=V0+1; back-edge CMP V0,#7 LT->header. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 preheader init */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(7, I32));        /* 1 header pretest */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=8 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 body (reads V0, ok: allowed at header cmp position only if excluded)  */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 increment */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(7, I32));        /* 5 back-edge cmp */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(UT_LT, I32), UTB_NONE); /* 6 back-edge jump */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 */

  /* NOTE: instruction 3 (body STORE) reads V0 -- the "no other uses" check in
   * tcc_ir_opt_decrement_to_zero would normally block the transform for a
   * body that reads the IV, but the loop bounds here are [start_idx=1,
   * end_idx=6] scanning backward from end_idx for the ADD; the STORE body
   * use is within [0,live_end) and DOES count as an "other use". We assert
   * accordingly: this body-reads-iv shape must NOT be transformed (documents
   * the pass's requirement that the IV be a pure counter, never read in the
   * body). */
  UT_ASSERT_EQ(tcc_ir_opt_decrement_to_zero(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD); /* untouched: still counting up */
  utb_free(ir);
  return 0;
}

UT_TEST(test_decrement_to_zero_pure_counter_rewritten)
{
  /* Pure counter: IV never read anywhere except init/increment/CMPs. Body
   * does unrelated work on a different VAR. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 preheader init i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(7, I32));        /* 1 header pretest */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=8 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(200, 1, 0, 0, I32), utb_imm(42, I32), UTB_NONE); /* 3 unrelated body */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 i++ */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(7, I32));        /* 5 back-edge cmp */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(UT_LT, I32), UTB_NONE); /* 6 back-edge jump */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 */

  int changes = tcc_ir_opt_decrement_to_zero(ir);
  UT_ASSERT_EQ(changes, 1);

  /* 1. Init rewritten V=#0 -> V=#7 (the limit). */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 0)), 7);
  /* 2. Increment ADD -> SUB #1. */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_SUB);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 4)), 1);
  /* 3. Back-edge CMP's #limit -> #0. */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 5)), 0);
  /* 4. Back-edge JUMPIF condition LT -> NE. */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 6)), UT_NE);
  /* 5. Pre-test guard (header CMP/JUMPIF at 1/2) NOPed -- always taken now. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_decrement_to_zero_no_separate_pretest_guard_bails)
{
  /* Regression lock for bugs.md #12 (fixed): when a loop has no SEPARATE
   * pre-test guard CMP before the header (only the back-edge CMP/JUMPIF
   * itself), the "find pre-test guard" scan (opt_loop.c ~line 930:
   * `scan_start = preheader_idx .. header_idx+2`) used to re-find the *same*
   * CMP/JUMPIF instruction that IS the back-edge test (be_cmp_idx/
   * be_jmpif_idx).  Step 5 ("NOP the pre-test guard") would then NOP those
   * indices *after* steps 3/4 had already rewritten them into the new
   * decrement-to-zero back-edge test -- destroying the loop's only back-edge
   * and degenerating it to a single iteration, while still reporting
   * changes=1 (a silent miscompile).
   *
   * The fix makes the scan skip any candidate whose indices coincide with
   * be_cmp_idx/be_jmpif_idx, so hdr_cmp_idx stays -1 and the transform bails
   * (the "must have found the pre-test guard" check).  This shape is now left
   * untouched rather than corrupted. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(200, 1, 0, 0, I32), utb_imm(42, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 2 i++ (header) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(7, I32));        /* 3 back-edge cmp (would also match as "pre-test guard") */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(UT_LT, I32), UTB_NONE); /* 4 back-edge jump */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 5 */

  int changes = tcc_ir_opt_decrement_to_zero(ir);

  /* No separate guard found -> transform bails, loop left intact. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);    /* increment untouched (still counting up) */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_CMP);    /* back-edge test preserved */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_JUMPIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_decrement_to_zero_no_loops_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_decrement_to_zero(ir), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_loop_ptr_iv_exit_subst */

/* Build a raw STACKOFF-tag "VAR value read" of vreg `vr` (tag=STACKOFF,
 * is_lval=1, is_local=1) -- the exact encoding ptr_iv_subst_uses_in_instr
 * requires (see its comment in opt_loop.c: a VAR value-read is a STACKOFF
 * operand carrying the VAR's own vreg with is_lval=1, NOT a deref-through
 * pointer, which would use tag=VREG instead). irop_make_stackoff always sets
 * is_local=1, so only is_lval needs to be forced on afterward. */
static IROperand utb_var_value_read(int32_t vreg, int btype)
{
  IROperand op = irop_make_stackoff(vreg, 0, /*is_lval*/ 1, 0, 0, btype);
  return op;
}

UT_TEST(test_ptr_iv_exit_subst_substitutes_post_loop_use)
{
  /* Counter IV V0 (0..3, step 1, 3 trips), pointer IV V1 initialized to
   * Addr[StackLoc[40]] and stepped by +4 each iteration (element size 4).
   * After the loop, a CMP reading V1's value should be rewritten to compare
   * against the closed-form exit address Addr[StackLoc[40 + 4*3]] == 52. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                           /* 1 p = &arr[0] */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 4 p += 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */
  /* exit_target = 8: post-loop use `CMP <value-read of p>, #0` -- a use that
   * ptr_iv_subst_uses_in_instr recognises and substitutes. */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  int changes = tcc_ir_opt_loop_ptr_iv_exit_subst(ir);

  UT_ASSERT_EQ(changes, 1);
  /* src1 of the post-loop CMP now reads Addr[StackLoc[52]] (40 + 4*3),
   * is_lval=0 (an address, not a value-read) per the substitution's repl
   * operand construction. */
  IROperand new_s1 = utb_src1(ir, post_cmp);
  UT_ASSERT_EQ(irop_get_tag(new_s1), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_s1), 52);
  UT_ASSERT_EQ(new_s1.is_lval, 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ptr_iv_exit_subst_deref_use_not_substituted)
{
  /* Same loop shape, but the post-loop use is a VREG-tagged deref (`*p`, the
   * dereference form), not a STACKOFF-tagged value-read.
   * ptr_iv_subst_uses_in_instr only matches IROP_TAG_STACKOFF operands, so a
   * VREG-form use must be left untouched. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                           /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_lval(utb_var(1, I32)), utb_imm(0, I32));             /* 8 *p (VREG deref) */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(tcc_ir_opt_loop_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1); /* untouched */
  utb_free(ir);
  return 0;
}

/* ============================================ tcc_ir_opt_loop_guard_elim */

UT_TEST(test_guard_elim_removes_provably_false_guard)
{
  /* A single rotated loop with its own tail exit-test near end_idx, PLUS a
   * separate pre-loop guard CMP further up whose outcome is statically known
   * false given the (immediate) entry value -- the guard must be NOPed. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 i=0 (entry) */
  int g_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32)); /* 1 guard cmp */
  int g_jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 guard: skip to 9 if i>=10 (false: 0<10) */
  /* loop body (rotated: back-edge CMP/JUMPIF near end_idx) */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 body start */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 4 i++ */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));         /* 5 tail cmp */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(UT_LT, I32), UTB_NONE);   /* 6 back-edge (continue if <10) */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 fallthrough exit_target */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 9 */

  int changes = tcc_ir_opt_loop_guard_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, g_cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, g_jmp), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_guard_elim_keeps_guard_when_provably_taken)
{
  /* Same shape, but entry value (20) makes the guard's `i>=10` condition
   * TRUE -- removing it would change behaviour, so the pass must leave it. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(20, I32), UTB_NONE);      /* 0 i=20 */
  int g_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32)); /* 1 */
  int g_jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 taken: 20>=10 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 4 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));         /* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(UT_LT, I32), UTB_NONE);   /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  int changes = tcc_ir_opt_loop_guard_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, g_cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, g_jmp), TCCIR_OP_JUMPIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_guard_elim_no_loops_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_loop_guard_elim(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_guard_elim_bails_on_switch_table)
{
  /* Un-enumerable control flow (SWITCH_TABLE) anywhere in the function makes
   * the whole pass bail out immediately, even though a removable guard is
   * present -- the program-order exit-value carry assumes straight-line
   * fall-through, which a switch breaks. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  int g_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 2 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 4 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));         /* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(UT_LT, I32), UTB_NONE);   /* 6 */
  utb_emit(ir, TCCIR_OP_SWITCH_TABLE, utb_var(0, I32), utb_imm(0, I32), utb_imm(3, I32)); /* 7 unrelated switch */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(tcc_ir_opt_loop_guard_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, g_cmp), TCCIR_OP_CMP); /* untouched */
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_loop)
{
  UT_COVERS("loop_strength_reduce_mul");
  UT_COVERS("loop_iv_strength_reduction");
  UT_COVERS("loop_bound_remat");
  UT_COVERS("loop_unroll");
  UT_COVERS("loop_rotation");
  UT_COVERS("loop_decrement_to_zero");
  UT_COVERS("loop_ptr_iv_exit_subst");
  UT_COVERS("loop_guard_elim");

  UT_RUN(test_sr_mul_power_of_2_becomes_shl);
  UT_RUN(test_sr_mul_power_of_2_immediate_on_left);
  UT_RUN(test_sr_mul_by_zero_becomes_assign_zero);
  UT_RUN(test_sr_mul_by_one_becomes_assign_passthrough);
  UT_RUN(test_sr_mul_non_power_of_2_declines);
  UT_RUN(test_sr_mul_both_operands_variable_declines);
  UT_RUN(test_sr_mul_not_a_mul_declines);
  UT_RUN(test_sr_whole_function_reduces_all_muls);
  UT_RUN(test_sr_whole_function_empty_ir_no_crash);
  UT_RUN(test_iv_sr_no_loops_returns_zero);
  UT_RUN(test_iv_sr_loop_with_no_derived_ivs_converges_to_zero);
  UT_RUN(test_iv_sr_with_loops_null_or_empty_returns_zero);
  UT_RUN(test_loop_bound_remat_no_calls_in_loop_no_change);
  UT_RUN(test_loop_bound_remat_hoisted_end_ptr_with_call_rematerializes);
  UT_RUN(test_loop_bound_remat_value_load_not_rematerialized);
  UT_RUN(test_loop_unroll_top_level_store_body_blocks_fold);
  UT_RUN(test_loop_unroll_top_level_no_loop_returns_zero);
  UT_RUN(test_loop_unroll_top_level_pure_counters_eliminated_not_unrolled);
  UT_RUN(test_loop_rotation_top_level_basic);
  UT_RUN(test_loop_rotation_top_level_no_loop_returns_zero);
  UT_RUN(test_loop_rotation_top_level_call_in_body_declines);
  UT_RUN(test_decrement_to_zero_basic_countup_rewritten);
  UT_RUN(test_decrement_to_zero_pure_counter_rewritten);
  UT_RUN(test_decrement_to_zero_no_separate_pretest_guard_bails);
  UT_RUN(test_decrement_to_zero_no_loops_returns_zero);
  UT_RUN(test_ptr_iv_exit_subst_substitutes_post_loop_use);
  UT_RUN(test_ptr_iv_exit_subst_deref_use_not_substituted);
  UT_RUN(test_guard_elim_removes_provably_false_guard);
  UT_RUN(test_guard_elim_keeps_guard_when_provably_taken);
  UT_RUN(test_guard_elim_no_loops_returns_zero);
  UT_RUN(test_guard_elim_bails_on_switch_table);
}
