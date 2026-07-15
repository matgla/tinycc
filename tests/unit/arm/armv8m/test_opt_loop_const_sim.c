/*
 *  test_opt_loop_const_sim.c - suite for ssa_opt_loop_const_sim
 *                               (ir/opt/ssa_opt_loop.c, loop constant simulation)
 *
 *  ssa_opt_loop_const_sim symbolically executes a small-trip-count loop body
 *  at compile time when every address/value the body touches is statically
 *  derivable, then rewrites the whole loop range to NOPs plus a handful of
 *  residual ASSIGN/STORE instructions carrying the loop's final state.  It is
 *  the SSA/CFG-era driver over the shared fold engine (lcs_fold_region in
 *  ir/opt_loop_const_sim.c); the legacy pre-SSA driver tcc_ir_opt_loop_const_sim
 *  this suite used to drive has been retired (see
 *  docs/plan_legacy_loop_const_sim_ssa.md).  Real bug history (project memory):
 *  238_fuzz_loop_const_sim_unsigned_char_residual.c fixed a dropped is_unsigned
 *  flag on narrow VAR residuals; 241_fuzz_loop_const_sim_indexed_store.c fixed a
 *  pre-loop-scan STORE_INDEXED blind spot.  Tests assert on CURRENT behavior.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point (ssa_opt_loop_const_sim) and the resulting instructions are
 *  inspected directly, following the ir_build.h / utb_* pattern used by
 *  test_opt_loop_dead.c / test_opt_loop_utils.c.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt/ssa_opt.h; forward-declared here to
 * avoid pulling in the SSA optimizer engine headers). */
int ssa_opt_loop_const_sim(TCCIRState *ir);

#define I8  IROP_BTYPE_INT8
#define I16 IROP_BTYPE_INT16
#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* JUMPIF condition tokens (match evaluate_compare_condition / opt_loop_utils.c). */
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

#define VR_VAR(n)  irop_get_vreg(utb_var(n, I32))
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

/* utb_new() leaves iroperand_pool_capacity / compact_instructions_size at 0
 * (it pre-fills the buffers but not the capacity bookkeeping). The pass under
 * test rewrites the loop to residual ASSIGN/STORE instructions via
 * tcc_ir_pool_add() / insert_instruction_before(), both of which grow via
 * those fields -- with capacity stuck at 0, the very first residual write
 * hits pool.c's realloc-to-0 dead end and aborts the whole test binary
 * ("tcc_ir_pool_add: out of memory"). Set them to the real allocated sizes so
 * the existing UTB_MAX_* buffers are used in place (our sequences are tiny,
 * well under the limits, so no reallocation is triggered). Mirrors
 * test_opt_licm.c's utb_loop_new() (ir/licm.c has the identical hazard). */
static TCCIRState *utb_loop_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  return ir;
}

/* lcs_scan_body calls tcc_ir_get_live_interval() for every VAR vreg it sees
 * (dest or source), which exit(1)s when ir->variables_live_intervals is
 * NULL/zero-sized (utb_new() leaves it so). Allocate a zeroed interval table
 * large enough for all VAR positions a test uses — mirrors
 * test_opt_constprop.c's utb_alloc_var_intervals. */
static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* Build a direct StackLoc[off] lvalue operand (is_lval=1, no vreg). */
static IROperand lcs_stack_lval(int32_t off, int btype)
{
  return irop_make_stackoff(-1, off, /*is_lval*/ 1, /*is_llocal*/ 0,
                            /*is_param*/ 0, btype);
}

/* Find the (single) residual STORE writing stack offset `off`; returns its
 * instruction index or -1 if none found. Used to locate the pass's rewritten
 * output regardless of which NOP slot it landed in. */
static int find_store_to_offset(TCCIRState *ir, int32_t off)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_STORE)
      continue;
    IROperand d = utb_dest(ir, i);
    if (irop_get_tag(d) == IROP_TAG_STACKOFF && irop_get_stack_offset(d) == off)
      return i;
  }
  return -1;
}

/* Find the residual ASSIGN whose dest is VAR `pos`; returns its index or -1.
 * Searches from the END of the instruction stream: a VAR that already had a
 * pre-loop preheader initializer (e.g. `acc = 0` before the loop) keeps that
 * original ASSIGN in place (it sits outside the folded loop range, so the
 * pass never touches it), and the residual carrying the loop's *final* value
 * is appended after it. In straight-line, non-SSA IR the last write to a
 * given VAR is the one that determines its runtime value, so callers that
 * want "the value V ends up holding" must find the last match, not the
 * first. */
static int find_assign_to_var(TCCIRState *ir, int pos)
{
  int32_t target = VR_VAR(pos);
  for (int i = ir->next_instruction_index - 1; i >= 0; i--)
  {
    if (utb_op(ir, i) != TCCIR_OP_ASSIGN)
      continue;
    IROperand d = utb_dest(ir, i);
    if (utb_vreg(d) == target)
      return i;
  }
  return -1;
}

/* ======================================================================
 * Headline: a canonical top-tested counting loop with a memory-only body
 * that copies the induction variable into a fixed stack slot.
 *
 *   0: ASSIGN V0 = #0            (preheader init)
 *   1: CMP V0, #5                (header, start_idx)
 *   2: JUMPIF GE -> 7            (exit)
 *   3: STORE [100] = V0          (body)
 *   4: ADD V0 = V0 + #1          (iv inc)
 *   5: JUMP -> 1                 (back-edge, end_idx)
 *   6: NOP                       (spacer / not part of loop range)
 *   7: RETURNVOID                (exit target)
 *
 * Final V0 after 5 iterations (0,1,2,3,4) is 5; the last body store writes
 * V0==4 (the value on the 5th and final executed iteration) into [100].
 * ====================================================================== */

static int emit_counting_store_loop(TCCIRState *ir, int init, int limit, int step,
                                    int32_t store_off, int store_btype)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(init, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(limit, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit=7 */
  utb_emit(ir, TCCIR_OP_STORE, lcs_stack_lval(store_off, store_btype),
           utb_var(0, I32), UTB_NONE);                                          /* 3 body */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(step, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 6 spacer */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 7 exit target */
  return 8;
}

/* A counting loop whose body writes memory (STORE [100]=V0) is now left intact
 * for the normal IR pipeline.  LCS bails on any memory-carrying loop: its
 * partial stack-memory modeling let stale aggregate values escape through
 * indexed stores / packed RMW chains, so the has_memory guard in
 * ssa_opt_loop_const_sim skips such loops.  The loop must be reported
 * unchanged (changes == 0), with its control flow and store untouched. */
UT_TEST(test_lcs_counting_store_in_body_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 5, 1, 100, I32);

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);

  /* Loop control and the memory body are untouched (deferred to the pipeline). */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  /* The store still holds the runtime IV, not a folded immediate. */
  int s = find_store_to_offset(ir, 100);
  UT_ASSERT(s >= 0);
  UT_ASSERT_EQ(irop_is_immediate(utb_src1(ir, s)), 0);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Same store loop with a post-loop reader of V0.  Still blocked by the memory
 * guard: no residual fold and no synthesized final-IV ASSIGN -- the only ASSIGN
 * to V0 remains the preheader init (#0). */
UT_TEST(test_lcs_counting_store_used_after_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 5, 1, 100, I32); /* emits indices 0..7, exit_target=7 */
  /* Append a reader of V0 after the exit target. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE); /* 8 */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);

  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  /* No folded final-value ASSIGN V0=#5 appended: the last (and only) ASSIGN to
   * V0 is still the preheader init carrying #0. */
  int a = find_assign_to_var(ir, 0);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 0);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);
  utb_free(ir);
  return 0;
}

/* Non-unit step, non-zero init: for (V0=2; V0<11; V0+=3).  The memory guard
 * blocks the fold regardless of step/init shape -- the loop is left intact. */
UT_TEST(test_lcs_counting_store_nonunit_step_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 2, 11, 3, 200, I32);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE); /* 8: read V0 after */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);

  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  /* Stored value is still the runtime IV, not a folded immediate. */
  int s = find_store_to_offset(ir, 200);
  UT_ASSERT(s >= 0);
  UT_ASSERT_EQ(irop_is_immediate(utb_src1(ir, s)), 0);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);
  utb_free(ir);
  return 0;
}

/* A second VAR accumulator updated in the body (not just the IV) also gets
 * folded to its final constant value and, if used after, gets a residual
 * ASSIGN.  for(i=0;i<4;i++) acc += 10;  acc final = 40. */
UT_TEST(test_lcs_accumulator_var_folds_to_final_value)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);     /* 9 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 40);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);
  utb_free(ir);
  return 0;
}

/* ======================================================================
 * is_unsigned residual preservation (regression class of test 238):
 * a narrow (INT8) VAR loop-invariant value must keep its sign flag on the
 * residual ASSIGN so downstream narrowing doesn't sign-extend an unsigned
 * byte.  Body: V1 (unsigned char) <- V0 (declared unsigned, byte width) each
 * iteration, where V0 never changes (loop-invariant store, folds via the
 * same residual-VAR path).
 * ====================================================================== */

UT_TEST(test_lcs_narrow_unsigned_var_residual_preserves_is_unsigned)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  /* V0 = 254 as an unsigned INT8 (0xFE). i is V2 (the loop counter). */
  IROperand v0_u8 = utb_unsigned(utb_var(0, I8));
  utb_emit(ir, TCCIR_OP_ASSIGN, v0_u8, utb_imm(254, I8), UTB_NONE);            /* 0 V0=254 (u8) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);   /* 1 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(3, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  /* Loop-invariant copy: V1 (u8) <- V0 (u8), every iteration (same value). */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_unsigned(utb_var(1, I8)), v0_u8, UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I8), UTB_NONE);     /* 9 read V1 after */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  IROperand dest = utb_dest(ir, a);
  /* The regression this pins: the residual dest operand must carry
   * is_unsigned so a later narrowing pass zero- (not sign-) extends 254. */
  UT_ASSERT_EQ(dest.is_unsigned, 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 254);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);
  utb_free(ir);
  return 0;
}

/* Signed-char sibling: a residual dest with is_unsigned==0 for a value whose
 * bit pattern (200 truncated to a signed byte -> -56) round-trips through
 * lcs_truncate/lcs_write_operand unchanged as the raw int64, but the operand
 * flag itself must read back 0 (not accidentally set). */
UT_TEST(test_lcs_narrow_signed_var_residual_is_unsigned_zero)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  IROperand v0_s8 = utb_var(0, I8); /* signed (no utb_unsigned) */
  utb_emit(ir, TCCIR_OP_ASSIGN, v0_s8, utb_imm(200, I8), UTB_NONE);           /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);  /* 1 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(3, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I8), v0_s8, UTB_NONE);             /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I8), UTB_NONE);     /* 9 */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ(utb_dest(ir, a).is_unsigned, 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 200);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * lcs_truncate width behavior, observed via a residual store: a 32-bit ADD
 * result that overflows int32 must wrap (residual value equals the wrapped
 * 32-bit result), while an INT64-typed accumulation must NOT wrap at 32
 * bits. Exercises the lcs_truncate table (INT8/16/32 fold to 32-bit; INT64
 * passes through) indirectly through observable IR.
 * ====================================================================== */

UT_TEST(test_lcs_int32_overflow_wraps_in_residual)
{
  /* acc starts at 0x7FFFFFFF and the loop adds 1 exactly once -> wraps to
   * INT32_MIN. Independent oracle: (int32_t)(0x7FFFFFFFu + 1u) == INT32_MIN. */
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  UT_ASSERT_EQ((int32_t)(int64_t)((uint64_t)(uint32_t)0x7FFFFFFF + 1u), (int32_t)0x80000000);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);         /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0x7FFFFFFF, I32), UTB_NONE); /* 1 acc */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(1, I32));            /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE);    /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32));     /* 4 acc+=1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));     /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                  /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                          /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                          /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);        /* 9 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), (int)0x80000000);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: opcode not in lcs_op_supported() blocks folding.
 *
 * LOAD_INDEXED is a common real-world loop-body op (array indexing) that is
 * conspicuously ABSENT from lcs_op_supported's switch (ir/opt_loop_const_
 * sim.c:149) — every other memory op used by simple counting loops (LOAD,
 * STORE, ASSIGN, LEA) is listed there, but LOAD_INDEXED / STORE_INDEXED /
 * LOAD_POSTINC / STORE_POSTINC are not, so lcs_scan_body's
 * `!lcs_op_supported(q->op)` check rejects any loop body containing one.
 * This is almost certainly intentional (the whole point of this simulator is
 * a *statically fully resolvable* body — a real array index load has no
 * defined "the address is a compile-time constant" shortcut the way a direct
 * StackLoc[off] does), so this test pins the CURRENT bail behavior rather
 * than asserting it is a bug. See ground-rule note in the suite header.
 * ====================================================================== */

UT_TEST(test_lcs_load_indexed_in_body_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit */
  /* LOAD_INDEXED T0 <- [base=stackoff(0), index=V0, scale=4] (4th operand
   * scale via utb_emit4; exact addressing semantics don't matter here, only
   * that the opcode itself is rejected by lcs_op_supported). */
  utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
           lcs_stack_lval(0, I32), utb_var(0, I32), utb_imm(4, I32));         /* 3 body */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 exit target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: a runtime (non-constant) value flowing into the body blocks the
 * fold. The IV step itself is fine, but the body ADDs a PARAM (unknown at
 * compile time) into an accumulator -> lcs_read_operand bails (PARAM has no
 * tracked slot), so lcs_exec returns action=0 and the whole loop is left
 * untouched.
 * ====================================================================== */

UT_TEST(test_lcs_runtime_param_value_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_param(0, I32)); /* 4 acc += param0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 exit target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: an unresolvable function call (unknown callee symbol name, not a
 * recognised soft-float helper) blocks the fold. lcs_classify_softcall
 * returns 0 for any name it doesn't recognise, and lcs_exec bails on that.
 * ====================================================================== */

UT_TEST(test_lcs_unknown_call_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);
  utb_pools_init(ir);

  /* Sym.v == 0 -> get_tok_str's stub table has no entry -> returns "?", which
   * lcs_classify_softcall does not recognise as any softfloat helper name
   * (mirrors test_unroll_body_with_call_skips's unregistered-symbol pattern). */
  static Sym unknown_fn;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &unknown_fn, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));                   /* 3 body call() */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 exit target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: trip count exceeds LCS_MAX_TRIP_COUNT (16) -> the IV-trip path
 * gives up (have_iv_trip stays 0), and the generic bounded-simulation
 * fallback requires a single exit target reachable purely from
 * stack/local/immediate state; a plain over-large counting loop like this
 * still qualifies for the generic path UNLESS it hits LCS_MAX_ITER_STEPS.
 * With trip=17 and a tiny body the generic path's max_total_steps budget
 * (LCS_MAX_TRIP_COUNT+1)*(body_size)+32 = 17*5+32 = 117 comfortably covers
 * 17 real iterations (~4 steps each = ~68 steps), so this loop DOES still
 * fold via the generic bounded simulator -- pinning that "trip > 16" alone
 * does not block folding, only the have_iv_trip fast path is skipped. */
UT_TEST(test_lcs_trip_over_max_blocks_both_paths)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 17, 1, 300, I32); /* trip_count = 17 > 16 */

  int changes = ssa_opt_loop_const_sim(ir);

  /* Independent oracle: the have_iv_trip fast path requires
   * trip_count <= LCS_MAX_TRIP_COUNT(16), so 17 disqualifies it; the generic
   * bounded-simulation fallback's own back-edge counter also starts at
   * LCS_MAX_TRIP_COUNT(16) and is decremented once per back-edge taken (see
   * lcs_try_fold's `step_trip_bound--` on step.action==2 with
   * !have_iv_trip). A 17-trip loop takes the back-edge 17 times before its
   * 18th CMP/JUMPIF finally exits, so step_trip_bound underflows to -1 and
   * the generic path bails too -- this trip count is un-foldable by EITHER
   * path, not just the fast one. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  /* The loop is left completely untouched, so the body's own original STORE
   * (emitted at index 3 by emit_counting_store_loop, unconditionally, not a
   * residual) is still exactly there -- unlike the positive-fold tests, an
   * unfoldable loop does NOT get its body NOPed, so a plain
   * "no store to this offset" check would be wrong: the pre-existing body
   * STORE was never removed. */
  UT_ASSERT_EQ(find_store_to_offset(ir, 300), 3);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: DIV by zero in the body (statically detectable once operands are
 * constant) is not foldable -- lcs_exec's TCCIR_OP_DIV case explicitly bails
 * (`if (v2 == 0) { action = 0; }`) rather than emitting undefined behavior
 * into the residual, so the loop is left completely untouched.
 * ====================================================================== */

UT_TEST(test_lcs_div_by_zero_in_body_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), utb_imm(10, I32), utb_imm(0, I32)); /* 3 10/0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 5 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 7 exit target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_DIV);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: address-taken VAR in the loop body blocks folding.  lcs_scan_body
 * rejects any VAR whose live interval has addrtaken (or is_complex) set --
 * the value could be mutated through an alias the simulator cannot see.
 * ====================================================================== */

UT_TEST(test_lcs_addrtaken_var_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);
  ir->variables_live_intervals[1].addrtaken = 1; /* V1's address is taken */

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);  /* 1 acc=0 (addrtaken) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 4 acc++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 exit target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: an internal conditional branch that exits to a THIRD location
 * (neither back-edge nor the CMP/JUMPIF's own exit_target) makes
 * lcs_find_single_exit_target / the "all branches land inside or at
 * exit_target" check fail for the generic path, and is irrelevant to the
 * have_iv_trip fast path only if that internal jump ever fires -- here it's
 * a compile-time-unreachable arm (condition always false), but the pass does
 * NOT execute the body ahead of time to prove that; it purely inspects
 * static jump targets in lcs_try_fold's "verify all branches ... land inside
 * OR at exit_target" loop BEFORE simulating, so a differing internal target
 * unconditionally blocks the fold regardless of runtime reachability. */
UT_TEST(test_lcs_internal_branch_to_third_target_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit=8 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(999, I32));   /* 3 never true */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 4 -> 9 (third target!) */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 exit_target */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 9 third target */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Negative: no loop at all (no backward jump) -> tcc_ir_detect_loops finds
 * nothing, loops->num_loops == 0, changes == 0, IR untouched.
 * ====================================================================== */

UT_TEST(test_lcs_no_loop_no_fire)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* forward only */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Idempotence: a memory-carrying loop is skipped by the guard, so the pass is
 * a no-op from the very first application and stays at a fixpoint (0 changes).
 * ====================================================================== */

UT_TEST(test_lcs_memory_loop_idempotent_noop)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 5, 1, 100, I32);

  int total = utb_run_to_fixpoint(ir, ssa_opt_loop_const_sim, 10);
  UT_ASSERT_EQ(total, 0);
  UT_ASSERT_EQ(ssa_opt_loop_const_sim(ir), 0);
  /* Loop left intact for the normal pipeline. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Pointer/address-of-local pattern: `T0 = &V0; *T0 = V0_value` inside the
 * loop is the LEA + indirect-STORE path (lcs_write_addr_operand /
 * lcs_resolve_stack_addr).  This address-tracking fold was the source of the
 * combo_num-872 miscompile class, so the memory guard now blocks it: a loop
 * carrying an indirect store is left for the normal pipeline rather than
 * simulated here.
 * ====================================================================== */

UT_TEST(test_lcs_lea_indirect_store_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  /* T0 = &StackLoc[64] (a LEA-style stack address, preheader). */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32),
           irop_make_stackoff(0, 64, /*is_lval*/ 0, 0, 0, I32), UTB_NONE);    /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  /* *T0 = i  (indirect STORE through the tracked address temp) */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_var(0, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 exit target */

  int changes = ssa_opt_loop_const_sim(ir);
  /* The indirect STORE makes this a memory-carrying loop -- the address-tracking
   * fold path (the source of the combo_num-872 miscompile class) is deferred to
   * the normal pipeline. */
  UT_ASSERT_EQ(changes, 0);

  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_STORE);
  /* The indirect store still holds the runtime IV, not a folded immediate. */
  UT_ASSERT_EQ(irop_is_immediate(utb_src1(ir, 4)), 0);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);
  utb_free(ir);
  return 0;
}

/* ======================================================================
 * Zero-trip loop (init already satisfies the exit condition, 0 >= 0): even
 * though the body never executes at runtime, the loop still *carries* a STORE,
 * so the memory guard bails before analyzing the trip count.  The loop is left
 * intact for the normal pipeline (which can prove the zero-trip statically).
 * This pins that the guard is a purely structural memory check -- it does not
 * peek at trip counts to make an exception for provably-dead bodies. */
UT_TEST(test_lcs_zero_trip_store_loop_blocks_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 0, 1, 400, I32); /* init==limit -> 0 trips */

  int changes = ssa_opt_loop_const_sim(ir);

  UT_ASSERT_EQ(changes, 0);
  /* Loop control and the store body are untouched. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ======================================================================
 * ssa_opt_loop_const_sim: the CFG/dominator-driven front-end for the shared
 * engine (lcs_fold_region).  The engine is exercised above through the legacy
 * entry; these tests drive the SSA driver, so they assert the *detection*
 * facts (dominance-verified candidates, contiguity, single-entry, outermost)
 * on top of the shared fold semantics.  Loops here must form a valid CFG:
 * tcc_ir_cfg_build parses the flat jump targets into blocks.
 * ====================================================================== */

/* Top-tested accumulator loop folds via the SSA driver, identical to the
 * legacy path: for(i=0;i<4;i++) acc+=10 -> residual acc=40. */
UT_TEST(test_lcs_ssa_accumulator_top_tested_folds)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);     /* 9 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  /* Back-edge collapsed; residuals fill the freed slots (residual acc=40). */
  UT_ASSERT_EQ(utb_op(ir, 6), TCCIR_OP_NOP);
  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 40);
  /* Non-member read of acc survives. */
  UT_ASSERT_EQ(utb_op(ir, 9), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Bottom-tested (rotated) loop — the orientation the legacy tccgen driver
 * currently never sees but the SSA pass will (it runs after ssa:loop_rotate).
 * do { acc+=10; i++; } while(i<4) -> acc=40. */
UT_TEST(test_lcs_ssa_bottom_tested_folds)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 2 header/body acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 i++ */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 4 bottom test */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 5 back-edge (i<4) */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 6 fall-through exit */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);     /* 7 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 40);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* int32 wrap fold via the SSA driver: acc=0x7FFFFFFF, +1 once -> INT32_MIN. */
UT_TEST(test_lcs_ssa_int32_overflow_wraps)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);          /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0x7FFFFFFF, I32), UTB_NONE); /* 1 acc */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(1, I32));             /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE);     /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32));      /* 4 acc+=1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));      /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                   /* 6 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                           /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                           /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);         /* 9 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), (int)0x80000000);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* is_unsigned residual preservation via the SSA driver (seed-4791 class). */
UT_TEST(test_lcs_ssa_narrow_unsigned_residual)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  IROperand v0_u8 = utb_unsigned(utb_var(0, I8));
  utb_emit(ir, TCCIR_OP_ASSIGN, v0_u8, utb_imm(254, I8), UTB_NONE);            /* 0 V0=254 (u8) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);   /* 1 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(3, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_unsigned(utb_var(1, I8)), v0_u8, UTB_NONE); /* 4 V1=V0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I8), UTB_NONE);     /* 9 read V1 */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ(utb_dest(ir, a).is_unsigned, 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 254);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Register-only decline: a STORE in the body makes it memory-carrying, so the
 * driver's has_memory scan over the member span declines (same narrowing as
 * legacy). */
UT_TEST(test_lcs_ssa_store_in_body_declines)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  emit_counting_store_loop(ir, 0, 5, 1, 100, I32);

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Address-taken VAR declines (engine's lcs_scan_body live-interval guard,
 * reached through the SSA driver). */
UT_TEST(test_lcs_ssa_addrtaken_var_declines)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);
  ir->variables_live_intervals[1].addrtaken = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);  /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 4 acc++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);           /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 8 exit target */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Trip count over LCS_MAX_TRIP_COUNT declines by both engine paths. */
UT_TEST(test_lcs_ssa_trip_over_max_declines)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  /* Register-only counting loop trip=17 (store loop would decline on memory
   * first; use a plain accumulator so the trip cap is what declines). */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(17, I32));     /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 4 acc++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);    /* 9 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* seed-589 shape: a backward JUMP that is NOT a dominance-verified back-edge
 * (a switch case body laid out before its dispatch; the dispatch jumps
 * backward into the case, which does not dominate the dispatch).  The legacy
 * range-scan detector flagged this as a loop; the SSA driver produces no
 * dominance-verified candidate, so nothing is folded. */
UT_TEST(test_lcs_ssa_seed589_switch_backjump_no_candidate)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);  /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);           /* 1 skip to dispatch */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(100, I32), UTB_NONE); /* 2 case body */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);           /* 3 case done */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(0, I32));     /* 4 dispatch */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 5 back into case */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);            /* 6 */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  /* Nothing NOPed: the case body and dispatch are untouched. */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* seed-2426 shape: a guard's else arm laid out between the header and the
 * rotated body sits inside the flat member span but is not a member block.
 * The legacy rotated-range extension absorbed and NOPed it; the SSA driver's
 * contiguity fact declines the whole loop structurally. */
UT_TEST(test_lcs_ssa_seed2426_nonmember_in_span_declines)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 2 exit->else arm */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);            /* 3 then -> body */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(99, I32), UTB_NONE);  /* 4 ELSE arm (non-member) */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(8, I32), UTB_NONE, UTB_NONE);            /* 5 else -> end */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 6 body i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);            /* 7 back-edge */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 8 end */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  /* The else arm survives (not NOPed by an absorbed-tail extension). */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 4)), 99);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Jump from outside into a non-header member block violates single-entry (the
 * header no longer dominates the body) -> declines. */
UT_TEST(test_lcs_ssa_side_entry_into_body_declines)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);            /* 1 side-entry into body */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ (side-entry target) */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);            /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);             /* 8 exit target */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Nested loop: the outer candidate simulates through the inner back-edge as
 * internal control flow; the inner loop is not separately processed (outermost
 * filter).  for(i=0;i<3;i++) for(j=0;j<2;j++) acc++ -> acc=6. */
UT_TEST(test_lcs_ssa_nested_outer_folds_inner_not_separate)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));      /* 2 outer header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(13, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 outer exit=13 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);   /* 4 j=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(2, I32));      /* 5 inner header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(10, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 6 inner exit=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(1, I32)); /* 7 acc++ */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32)); /* 8 j++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);            /* 9 inner back-edge */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 10 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);            /* 11 outer back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 12 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 13 outer exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);  /* 14 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 1);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 6);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 24), 0);

  utb_free(ir);
  return 0;
}

/* Two-loop cascade: loop B consumes loop A's residual across driver rounds
 * with no interleaved cleanup.  A: acc=40; B seeds acc=40, adds 3*100 -> 340. */
UT_TEST(test_lcs_ssa_cascade_two_loops)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 A header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 A exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 A back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);   /* 8 k=0 (A exit target) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(3, I32));      /* 9 B header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(15, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 10 B exit=15 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(100, I32)); /* 11 acc+=100 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32));   /* 12 k++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(9, I32), UTB_NONE, UTB_NONE);               /* 13 B back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 14 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);     /* 15 read acc */

  int changes = ssa_opt_loop_const_sim(ir);
  /* One fold per round: A in round 1, B in round 2 -> 2 total. */
  UT_ASSERT_EQ(changes, 2);

  int a = find_assign_to_var(ir, 1);
  UT_ASSERT(a >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a)), 340);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 24), 0);

  utb_free(ir);
  return 0;
}

/* Quality: three independent (disjoint) loops all fold in a single driver
 * call.  Outermost natural loops have disjoint spans, so the driver folds every
 * one per CFG build rather than one-per-round — no artificial cap on how many
 * loops a function may collapse.  acc1=40, acc2=300, acc3=5. */
UT_TEST(test_lcs_ssa_three_independent_loops_fold)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 8);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc1=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 L1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit=8 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc1+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 L1 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(0, I32), UTB_NONE);   /* 8 j=0 (L1 exit) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(3, I32), utb_imm(0, I32), UTB_NONE);   /* 9 acc2=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(2, I32), utb_imm(3, I32));      /* 10 L2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(16, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 11 exit=16 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(3, I32), utb_var(3, I32), utb_imm(100, I32)); /* 12 acc2+=100 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(2, I32), utb_imm(1, I32));   /* 13 j++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(10, I32), UTB_NONE, UTB_NONE);              /* 14 L2 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 15 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(4, I32), utb_imm(0, I32), UTB_NONE);   /* 16 k=0 (L2 exit) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(5, I32), utb_imm(0, I32), UTB_NONE);   /* 17 acc3=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(4, I32), utb_imm(5, I32));      /* 18 L3 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(24, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 19 exit=24 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(5, I32), utb_var(5, I32), utb_imm(1, I32));  /* 20 acc3+=1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(4, I32), utb_var(4, I32), utb_imm(1, I32));  /* 21 k++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(18, I32), UTB_NONE, UTB_NONE);              /* 22 L3 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 23 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);  /* 24 read acc1 (L3 exit) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(3, I32), UTB_NONE);  /* 25 read acc2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_var(5, I32), UTB_NONE);  /* 26 read acc3 */

  int changes = ssa_opt_loop_const_sim(ir);
  UT_ASSERT_EQ(changes, 3);

  int a1 = find_assign_to_var(ir, 1), a2 = find_assign_to_var(ir, 3),
      a3 = find_assign_to_var(ir, 5);
  UT_ASSERT(a1 >= 0 && a2 >= 0 && a3 >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a1)), 40);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a2)), 300);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, a3)), 5);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 24), 0);

  utb_free(ir);
  return 0;
}

/* Idempotency: the driver's own output holds no back-edge, so a second run
 * finds nothing. */
UT_TEST(test_lcs_ssa_idempotent)
{
  TCCIRState *ir = utb_loop_new();
  utb_alloc_var_intervals(ir, 4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE);   /* 1 acc=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(4, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(10, I32)); /* 4 acc+=10 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);               /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 7 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                       /* 8 exit target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(1, I32), UTB_NONE);     /* 9 read acc */

  UT_ASSERT_EQ(ssa_opt_loop_const_sim(ir), 1);
  UT_ASSERT_EQ(ssa_opt_loop_const_sim(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("loop_const_sim");
