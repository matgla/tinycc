/*
 *  test_opt_loop.c - suite for ir/opt_loop.c (pre-SSA loop optimizations)
 *
 *  Covers the top-level entry points in ir/opt_loop.c: MUL strength
 *  reduction, IV strength reduction driver, loop-bound rematerialization,
 *  loop unrolling/elimination, loop rotation, and decrement-to-zero — plus
 *  ssa_opt_ptr_iv_exit_subst (ir/opt/ssa_opt_loop.c), which replaced the
 *  legacy pre-SSA driver.
 *
 *  This is a *different* file from ir/opt_loop_dead.c and ir/opt_loop_utils.c,
 *  which already have their own dedicated suites (test_opt_loop_dead.c,
 *  test_opt_loop_utils.c).  Those already exercise the shared helpers
 *  (find_induction_vars_ex, find_loop_exit_condition, compute_trip_count,
 *  try_eliminate_loop, try_unroll_loop_ex, try_rotate_loop) in isolation;
 *  here we drive the *outer* tcc_ir_opt_* entry points in ir/opt_loop.c that
 *  wrap tcc_ir_detect_loops() + those helpers, so the loop-detection glue and
 *  the opt_loop.c-local logic (strength_reduce_mul, decrement_to_zero,
 *  ptr_iv_exit_subst) gets real coverage.
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

/* Pass entry points under test (declared in ir/opt.h; forward-declared here
 * to avoid pulling in the optimizer engine headers, matching the sibling
 * suites' style). */
int tcc_ir_strength_reduce_mul(TCCIRState *ir, int instr_idx);
int ssa_opt_iv_strength_reduction(TCCIRState *ir);
int ssa_opt_loop_rotate(TCCIRState *ir);
int ssa_opt_decrement_to_zero(TCCIRState *ir);
int ssa_opt_ptr_iv_exit_subst(TCCIRState *ir);
int ssa_opt_loop_unroll(TCCIRState *ir);

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

/* ============================================ ssa_opt_iv_strength_reduction */

UT_TEST(test_iv_sr_no_loops_returns_zero)
{
  /* Straight-line code, no backward jump -> the CFG has a single block, so the
   * driver breaks out (num_blocks <= 1) before touching the engine. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(ssa_opt_iv_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_loop_with_no_derived_ivs_converges_to_zero)
{
  /* A simple counting loop with no array-indexing derived IV: the CFG front-end
   * builds the synthetic loop and iv_strength_reduction_core finds the IV but no
   * DerivedIVs to rewrite, so the driver reports 0 changes. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 5 */

  UT_ASSERT_EQ(ssa_opt_iv_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_null_or_empty_returns_zero)
{
  /* Driver guards: NULL ir and an empty instruction stream both decline. */
  UT_ASSERT_EQ(ssa_opt_iv_strength_reduction(NULL), 0);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  UT_ASSERT_EQ(ssa_opt_iv_strength_reduction(ir), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ ssa_opt_loop_rotate */

/* Build the canonical rotatable counted loop:
 *   0: V0 = 0
 *   1: CMP V0, 5          header
 *   2: JUMPIF exit=8, GE
 *   3: JUMP -> body=6
 *   4: ADD V0 = V0 + 1    latch
 *   5: JUMP -> 1          back-edge
 *   6: STORE [100] = V0   body
 *   7: JUMP -> 4          body->latch
 *   8: RETURNVOID         exit_target */
static TCCIRState *ssa_rotate_build_basic(void)
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
  return ir;
}

/* Assert the rotated (bottom-tested) shape produced from the canonical loop:
 * relocated body (STORE) is the new back-edge target at 3, followed by the
 * latch (ADD), then a tail CMP/JUMPIF pair whose condition is inverted
 * (GE -> LT) and targets the relocated body.  Returns 0 on success, -1 on a
 * failed assertion (UT_ASSERT_EQ returns -1); callers must propagate. */
static int ssa_rotate_assert_rotated(TCCIRState *ir)
{
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(ir->compact_instructions[3].is_jump_target, 1);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 6), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 6)), UT_LT);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 6)), 3);
  return 0;
}

UT_TEST(test_ssa_loop_rotate_basic)
{
  /* CFG/dominator detection must find the natural loop and drive the rewrite
   * to the same bottom-tested shape the legacy pass produces. */
  TCCIRState *ir = ssa_rotate_build_basic();
  UT_ASSERT_EQ(ssa_opt_loop_rotate(ir), 1);
  if (ssa_rotate_assert_rotated(ir)) return -1;
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_loop_rotate_no_loop_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  UT_ASSERT_EQ(ssa_opt_loop_rotate(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_loop_rotate_call_in_body_declines)
{
  /* The rewrite (try_rotate_loop) declines call-containing bodies; verify the
   * decline still reaches through the CFG-detection front-end. */
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

  UT_ASSERT_EQ(ssa_opt_loop_rotate(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP); /* untouched */
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_loop_rotate_idempotent)
{
  /* Idempotency is the property that lets this pass coexist with the still-
   * enabled legacy pass: once a loop is bottom-tested, its top-tested
   * [CMP,JUMPIF,JUMP] header is gone, so a second run rotates nothing. */
  TCCIRState *ir = ssa_rotate_build_basic();
  UT_ASSERT_EQ(ssa_opt_loop_rotate(ir), 1);
  if (ssa_rotate_assert_rotated(ir)) return -1;
  UT_ASSERT_EQ(ssa_opt_loop_rotate(ir), 0);  /* already rotated */
  if (ssa_rotate_assert_rotated(ir)) return -1; /* shape unchanged */
  utb_free(ir);
  return 0;
}

/* ============================================ ssa_opt_decrement_to_zero
 * The CFG/dominator driver ssa_opt_decrement_to_zero detects the natural loop
 * in each fixture and runs the retained engine dtz_try_region; the assertions
 * below still pin the engine's five rewrites + the bugs.md #12 guard bail. */

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
   * dtz_try_region would normally block the transform for a
   * body that reads the IV, but the loop bounds here are [start_idx=1,
   * end_idx=6] scanning backward from end_idx for the ADD; the STORE body
   * use is within [0,live_end) and DOES count as an "other use". We assert
   * accordingly: this body-reads-iv shape must NOT be transformed (documents
   * the pass's requirement that the IV be a pure counter, never read in the
   * body). */
  UT_ASSERT_EQ(ssa_opt_decrement_to_zero(ir), 0);
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

  int changes = ssa_opt_decrement_to_zero(ir);
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
   * itself), the "find pre-test guard" scan (dtz_try_region, ir/opt_loop_utils.c:
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

  int changes = ssa_opt_decrement_to_zero(ir);

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
  UT_ASSERT_EQ(ssa_opt_decrement_to_zero(ir), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ ssa_opt_ptr_iv_exit_subst */

/* VAR value-read encoding (tag=STACKOFF, vreg, is_lval=1) — the only form
 * ptr_iv_subst_uses_in_instr substitutes; tag=VREG would mean deref. */
static IROperand utb_var_value_read(int32_t vreg, int btype)
{
  IROperand op = irop_make_stackoff(vreg, 0, /*is_lval*/ 1, 0, 0, btype);
  return op;
}

/* Canonical single-entry top-tested loop: 0 counter init, 1 pointer init,
 * 2-3 CMP+JUMPIF exit->8, 4 ptr step +4, 5 counter step, 6 back-edge, 7 NOP.
 * Caller appends post-loop instructions from 8 (the exit target). */
static void utb_emit_piv_loop(TCCIRState *ir, int init, int limit, int cond,
                              IROperand p_init)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(init, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), p_init, UTB_NONE);               /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(limit, I32));     /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(cond, I32), UTB_NONE);   /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));  /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
}

UT_TEST(test_ssa_ptr_iv_subst_substitutes_post_loop_use)
{
  /* 3 trips, step 4 from StackLoc[40] -> exit value 52. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 1);
  IROperand new_s1 = utb_src1(ir, post_cmp);
  UT_ASSERT_EQ(irop_get_tag(new_s1), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_s1), 52);
  UT_ASSERT_EQ(new_s1.is_lval, 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_deref_use_not_substituted)
{
  /* A VREG-tagged deref use (`*p`) must be left untouched. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_lval(utb_var(1, I32)), utb_imm(0, I32));             /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_split_body_loop_substitutes)
{
  /* Canonical frontend layout: body AFTER the back-edge, reached by forward
   * jumps — the legacy flat range never matched this; CFG membership must. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(10, I32), utb_imm(UT_GE, I32), UTB_NONE);  /* 3 exit=10 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(7, I32), UTB_NONE, UTB_NONE);                /* 4 -> body */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 7 body: p += 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);                /* 8 -> increment */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 9 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 10 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 11 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 1);
  IROperand new_s1 = utb_src1(ir, post_cmp);
  UT_ASSERT_EQ(irop_get_tag(new_s1), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_s1), 52);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_folds_cmp_jumpif_taken)
{
  /* `p != &a[N]` idiom: both sides resolve to StackLoc[52] (one via the LEA
   * temp) -> branch proven taken: CMP -> NOP, JUMPIF -> JUMP. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(5, I32),
           utb_stackoff(52, 0, 0, 0, I32), UTB_NONE);                              /* 8 */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                     utb_var_value_read(v1, I32), utb_temp(5, I32));               /* 9 */
  int jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(13, I32), utb_imm(UT_EQ, I32),
                     UTB_NONE);                                                    /* 10 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(1, I32), UTB_NONE);       /* 11 abort path */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 12 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 13 */

  /* 1 substitution + 1 control-flow change (folded JUMPIF). */
  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 2);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, jmp)), 13);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_folds_cmp_jumpif_not_taken)
{
  /* NE branch on proven-equal operands: both CMP and JUMPIF become NOPs. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(5, I32),
           utb_stackoff(52, 0, 0, 0, I32), UTB_NONE);                              /* 8 */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                     utb_var_value_read(v1, I32), utb_temp(5, I32));               /* 9 */
  int jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(13, I32), utb_imm(UT_NE, I32),
                     UTB_NONE);                                                    /* 10 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(1, I32), UTB_NONE);       /* 11 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 12 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 13 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 2);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_fold_unequal_left_alone)
{
  /* Different offsets: substitute, but the conservative fold declines. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(5, I32),
           utb_stackoff(56, 0, 0, 0, I32), UTB_NONE);                              /* 8: 56 != 52 */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                     utb_var_value_read(v1, I32), utb_temp(5, I32));               /* 9 */
  int jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(12, I32), utb_imm(UT_EQ, I32),
                     UTB_NONE);                                                    /* 10 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 11 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 12 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 1); /* substitution only */
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_JUMPIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_no_counter_iv_declines)
{
  /* Counter without an immediate preheader init -> no anchor -> decline. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 0: no V0 init */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_lt_exit_cond_declines)
{
  /* Exit condition LT is outside compute_trip_count's model -> decline. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_LT, utb_stackoff(40, 0, 0, 0, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_zero_trip_declines)
{
  /* init 5 >= limit 3: trip count 0 -> decline. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 5, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_second_ptr_def_declines)
{
  /* Pointer IV with a second in-loop def -> not a pure linear step. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 5 second def */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 6 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 8 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 9 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 10 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_non_stack_init_declines)
{
  /* Pointer init is an immediate, not Addr[StackLoc[X]] -> decline. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_imm(40, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_final_offset_overflow_declines)
{
  /* init_off + step*trip overflows int32 -> decline. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(0x7FFFFFFC, 0, 0, 0, I32));
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_counter_only_returns_zero)
{
  /* The counter is never treated as a pointer IV; its post-loop read stays. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v0 = VR_VAR(0);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE);   /* 3 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v0, I32), utb_imm(0, I32));           /* 8 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 9 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_walk_stops_at_redef)
{
  /* A redef of V between exit_target and the use retires V. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(7, I32), UTB_NONE);       /* 8 redef */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 9 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 10 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_walk_retires_at_merge)
{
  /* An is_jump_target merge after exit_target retires all IVs. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 8 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 9 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 10 */
  ir->compact_instructions[9].is_jump_target = 1;

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_walk_retires_at_backward_jump)
{
  /* A backward JUMP in the post-loop walk retires all IVs. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(7, I32), UTB_NONE, UTB_NONE);                /* 8 backward */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 9 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 10 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_side_entry_declines)
{
  /* A side entry into the body: header no longer dominates the latch, so no
   * back-edge candidate exists (trip counts are entry-path sensitive). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(0, I32));          /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_EQ, I32), UTB_NONE);   /* 3 side entry */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 4 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(10, I32), utb_imm(UT_GE, I32), UTB_NONE);  /* 5 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 6 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 7 (side target) */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);                /* 8 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 9 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 10 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 11 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_conditional_step_declines)
{
  /* Conditional self-add: its block doesn't dominate the latch, so the step
   * isn't once-per-iteration (legacy's flat scan accepted this shape). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  int32_t v1 = VR_VAR(1);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(11, I32), utb_imm(UT_GE, I32), UTB_NONE);  /* 3 exit */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(1, I32));          /* 4 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_EQ, I32), UTB_NONE);   /* 5 skip step */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 6 conditional */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 7 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                /* 8 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 9 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 10 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(v1, I32), utb_imm(0, I32));           /* 11 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 12 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), v1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_guard_nop_rotated)
{
  /* Rotated shape: trip > 0 NOPs the guard, clears the sole-in-edge target's
   * stale is_jump_target, and the post-loop use substitutes. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  int g_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32),
                       utb_imm(3, I32));                                           /* 2 guard */
  int g_jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(10, I32), utb_imm(UT_GE, I32),
                       UTB_NONE);                                                  /* 3 skip loop */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32),
           utb_var(0, I32), UTB_NONE);                                             /* 4 body/header */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 5 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 6 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 7 tail cmp */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(UT_LT, I32), UTB_NONE);   /* 8 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 9 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(VR_VAR(1), I32), utb_imm(0, I32));    /* 10 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 11 */
  ir->compact_instructions[4].is_jump_target = 1;
  ir->compact_instructions[10].is_jump_target = 1; /* stale after guard NOP */

  /* 1 substitution + 1 control-flow change (guard NOP). */
  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 2);
  UT_ASSERT_EQ(utb_op(ir, g_cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, g_jmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->compact_instructions[10].is_jump_target, 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, post_cmp)), 52);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_guard_target_other_inedge_keeps_flag)
{
  /* Guard target with another in-edge: flag stays, walk retires at the
   * merge — guard NOP only, no substitution. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32),
           utb_stackoff(40, 0, 0, 0, I32), UTB_NONE);                              /* 1 */
  int g_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32),
                       utb_imm(3, I32));                                           /* 2 */
  int g_jmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(10, I32), utb_imm(UT_GE, I32),
                       UTB_NONE);                                                  /* 3 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32),
           utb_var(0, I32), UTB_NONE);                                             /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(4, I32));   /* 5 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));   /* 6 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));          /* 7 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(UT_LT, I32), UTB_NONE);   /* 8 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 9 */
  int post_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                          utb_var_value_read(VR_VAR(1), I32), utb_imm(0, I32));    /* 10 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 11 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(10, I32), UTB_NONE, UTB_NONE);               /* 12 other in-edge */
  ir->compact_instructions[4].is_jump_target = 1;
  ir->compact_instructions[10].is_jump_target = 1;

  /* Guard NOP only (control-flow change); substitution blocked by the kept
   * merge flag. */
  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, g_cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, g_jmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->compact_instructions[10].is_jump_target, 1);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, post_cmp)), VR_VAR(1));
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_idempotent)
{
  /* Second run on own output (incl. after the fold) changes nothing. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit_piv_loop(ir, 0, 3, UT_GE, utb_stackoff(40, 0, 0, 0, I32));
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(5, I32),
           utb_stackoff(52, 0, 0, 0, I32), UTB_NONE);                              /* 8 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
           utb_var_value_read(VR_VAR(1), I32), utb_temp(5, I32));                  /* 9 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(12, I32), utb_imm(UT_EQ, I32), UTB_NONE);  /* 10 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 11 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 12 */

  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 2);
  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_ptr_iv_subst_no_loops_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(ssa_opt_ptr_iv_exit_subst(ir), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ ssa_opt_loop_unroll */

/* A non-nested counted loop with an accumulator collapses to closed-form
 * final values through the CFG-driven driver (try_eliminate_loop path). */
UT_TEST(test_ssa_loop_unroll_eliminates_pure_counter)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);     /* 1 c=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));        /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 exit=7 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 4 c++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);    /* 7 reads c */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 */

  UT_ASSERT_EQ(ssa_opt_loop_unroll(ir), 1);
  /* closed-form c = 0 + 4*1 = 4 written into the NOP'd loop range */
  int found_final = 0;
  for (int i = 2; i <= 6; i++)
    if (utb_op(ir, i) == TCCIR_OP_ASSIGN && utb_vreg(utb_dest(ir, i)) == VR_VAR(1) &&
        irop_is_immediate(utb_src1(ir, i)) && (int)irop_get_imm64_ex(ir, utb_src1(ir, i)) == 4)
      found_final = 1;
  UT_ASSERT(found_final);
  utb_free(ir);
  return 0;
}

/* Regression for gcc.c-torture 991216-4: an inner loop whose accumulator is
 * loop-carried by the outer loop must NOT be eliminated — its "init" is the
 * outer preheader, not a per-entry init.  Outermost-only + the outer declining
 * on its internal branches means the pass leaves the whole nest intact.  A
 * driver that processed the inner loop here NOP'd the outer loop's guard and
 * hung. */
UT_TEST(test_ssa_loop_unroll_nested_carried_accum_declines)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);      /* 0 num=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(1, I32), utb_imm(5, I32));         /* 1 outer header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(12, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 outer exit=12 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);      /* 3 i=1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));        /* 4 inner header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(11, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 5 inner exit=11 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32));  /* 6 num++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 7 i++ */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));        /* 8 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_LT, I32), UTB_NONE);  /* 9 inner back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 10 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 11 outer back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 12 */

  UT_ASSERT_EQ(ssa_opt_loop_unroll(ir), 0);
  /* the outer loop's control flow must be intact (the bug NOP'd 0/1/2) */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, 11), TCCIR_OP_JUMP);
  utb_free(ir);
  return 0;
}

/* Second run on the pass's own output finds no back-edge -> 0 changes. */
UT_TEST(test_ssa_loop_unroll_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);     /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));        /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);    /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 */

  UT_ASSERT_EQ(ssa_opt_loop_unroll(ir), 1);
  UT_ASSERT_EQ(ssa_opt_loop_unroll(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ssa_loop_unroll_no_loops_returns_zero)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(ssa_opt_loop_unroll(ir), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_loop)
{
  UT_COVERS("loop_strength_reduce_mul");
  UT_COVERS("ssa_iv_strength_reduction");
  UT_COVERS("loop_rotation");
  UT_COVERS("ssa_decrement_to_zero");
  UT_COVERS("ssa_ptr_iv_exit_subst");
  UT_COVERS("ssa_loop_unroll");

  UT_RUN(test_sr_mul_power_of_2_becomes_shl);
  UT_RUN(test_sr_mul_power_of_2_immediate_on_left);
  UT_RUN(test_sr_mul_by_zero_becomes_assign_zero);
  UT_RUN(test_sr_mul_by_one_becomes_assign_passthrough);
  UT_RUN(test_sr_mul_non_power_of_2_declines);
  UT_RUN(test_sr_mul_both_operands_variable_declines);
  UT_RUN(test_sr_mul_not_a_mul_declines);
  UT_RUN(test_iv_sr_no_loops_returns_zero);
  UT_RUN(test_iv_sr_loop_with_no_derived_ivs_converges_to_zero);
  UT_RUN(test_iv_sr_null_or_empty_returns_zero);
  UT_RUN(test_ssa_loop_rotate_basic);
  UT_RUN(test_ssa_loop_rotate_no_loop_returns_zero);
  UT_RUN(test_ssa_loop_rotate_call_in_body_declines);
  UT_RUN(test_ssa_loop_rotate_idempotent);
  UT_RUN(test_decrement_to_zero_basic_countup_rewritten);
  UT_RUN(test_decrement_to_zero_pure_counter_rewritten);
  UT_RUN(test_decrement_to_zero_no_separate_pretest_guard_bails);
  UT_RUN(test_decrement_to_zero_no_loops_returns_zero);
  UT_RUN(test_ssa_ptr_iv_subst_substitutes_post_loop_use);
  UT_RUN(test_ssa_ptr_iv_subst_deref_use_not_substituted);
  UT_RUN(test_ssa_ptr_iv_subst_split_body_loop_substitutes);
  UT_RUN(test_ssa_ptr_iv_subst_folds_cmp_jumpif_taken);
  UT_RUN(test_ssa_ptr_iv_subst_folds_cmp_jumpif_not_taken);
  UT_RUN(test_ssa_ptr_iv_subst_fold_unequal_left_alone);
  UT_RUN(test_ssa_ptr_iv_subst_no_counter_iv_declines);
  UT_RUN(test_ssa_ptr_iv_subst_lt_exit_cond_declines);
  UT_RUN(test_ssa_ptr_iv_subst_zero_trip_declines);
  UT_RUN(test_ssa_ptr_iv_subst_second_ptr_def_declines);
  UT_RUN(test_ssa_ptr_iv_subst_non_stack_init_declines);
  UT_RUN(test_ssa_ptr_iv_subst_final_offset_overflow_declines);
  UT_RUN(test_ssa_ptr_iv_subst_counter_only_returns_zero);
  UT_RUN(test_ssa_ptr_iv_subst_walk_stops_at_redef);
  UT_RUN(test_ssa_ptr_iv_subst_walk_retires_at_merge);
  UT_RUN(test_ssa_ptr_iv_subst_walk_retires_at_backward_jump);
  UT_RUN(test_ssa_ptr_iv_subst_side_entry_declines);
  UT_RUN(test_ssa_ptr_iv_subst_conditional_step_declines);
  UT_RUN(test_ssa_ptr_iv_subst_guard_nop_rotated);
  UT_RUN(test_ssa_ptr_iv_subst_guard_target_other_inedge_keeps_flag);
  UT_RUN(test_ssa_ptr_iv_subst_idempotent);
  UT_RUN(test_ssa_ptr_iv_subst_no_loops_returns_zero);
  UT_RUN(test_ssa_loop_unroll_eliminates_pure_counter);
  UT_RUN(test_ssa_loop_unroll_nested_carried_accum_declines);
  UT_RUN(test_ssa_loop_unroll_idempotent);
  UT_RUN(test_ssa_loop_unroll_no_loops_returns_zero);
}
