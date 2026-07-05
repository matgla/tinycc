/*
 *  test_opt_loop_utils.c - suite for ir/opt_loop_utils.c
 *
 *  Covers the two pure helpers exhaustively (signed_to_unsigned_cond,
 *  compute_trip_count — where arithmetic/overflow bugs hide) plus the
 *  IR-coupled loop-analysis entry points that the unroll/SR passes build on
 *  (find_induction_vars_ex, find_loop_exit_condition).
 *
 *  The pure-function tests are invariant/bug-hunt style: they assert the
 *  mathematically correct trip count for each condition shape and the
 *  overflow behaviour the int cast produces.
 */

#include "ir_build.h"

#include "ut.h"
#include "opt_loop_utils.h"

#define I32 IROP_BTYPE_INT32

/* Condition-token values used by the IR (see tcc.h TOK_*; mirrored from
 * signed_to_unsigned_cond in opt_loop_utils.c). */
#define UT_LT 0x9c
#define UT_GE 0x9d
#define UT_LE 0x9e
#define UT_GT 0x9f
#define UT_EQ 0x94
#define UT_NE 0x95
#define UT_ULT 0x92
#define UT_UGE 0x93
#define UT_ULE 0x96
#define UT_UGT 0x97

#define VR_VAR(n) irop_get_vreg(utb_var(n, I32))
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

/* Build a minimal IRLoop over [start,end] with the given preheader. */
static IRLoop utb_loop(int header, int start, int end, int preheader)
{
  IRLoop L;
  memset(&L, 0, sizeof L);
  L.header_idx = header;
  L.start_idx = start;
  L.end_idx = end;
  L.preheader_idx = preheader;
  L.body_instrs = NULL;
  L.num_body_instrs = 0;
  L.body_instrs_capacity = 0;
  L.depth = 1;
  return L;
}

/* Initialise the temp-vreg live-interval pool (mirrors test_ir_vreg.c's
 * ut_init_intervals).  Needed by any test that reaches tcc_ir_vreg_alloc_temp
 * (try_eliminate_iv_counter's end_vreg allocation) — without it,
 * temporary_variables_live_intervals_size stays 0 and the growth arithmetic
 * in tcc_ir_vreg_alloc_temp (size <<= 1) never actually grows the buffer. */
#define UTB_INTERVAL_INIT_SIZE 8
/* `reserved` pre-bumps next_temporary_variable so that any TEMP vregs the
 * test hand-constructs at positions [0, reserved) are treated as already
 * allocated — a subsequent tcc_ir_vreg_alloc_temp() call (e.g. inside
 * try_eliminate_iv_counter's end_vreg allocation) then returns a FRESH
 * position starting at `reserved`, instead of colliding with a hand-picked
 * TEMP the test is using to simulate a pre-existing pointer vreg. */
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

/* ============================================ signed_to_unsigned_cond */

UT_TEST(test_s2u_signed_mappings)
{
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_LT), UT_ULT);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_GE), UT_UGE);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_LE), UT_ULE);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_GT), UT_UGT);
  return 0;
}

UT_TEST(test_s2u_eq_ne_unchanged)
{
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_EQ), UT_EQ);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_NE), UT_NE);
  return 0;
}

UT_TEST(test_s2u_already_unsigned_passthrough)
{
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_ULT), UT_ULT);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_UGE), UT_UGE);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_ULE), UT_ULE);
  UT_ASSERT_EQ(signed_to_unsigned_cond(UT_UGT), UT_UGT);
  return 0;
}

UT_TEST(test_s2u_unknown_token_returns_minus_one)
{
  UT_ASSERT_EQ(signed_to_unsigned_cond(0x00), -1);
  UT_ASSERT_EQ(signed_to_unsigned_cond(0xFF), -1);
  UT_ASSERT_EQ(signed_to_unsigned_cond(0x40), -1);
  return 0;
}

/* ============================================ compute_trip_count (invariants) */

UT_TEST(test_trip_count_invalid_step)
{
  UT_ASSERT_EQ(compute_trip_count(0, 10, 0, UT_GE), -1);
  UT_ASSERT_EQ(compute_trip_count(0, 10, -1, UT_GE), -1);
  return 0;
}

UT_TEST(test_trip_count_ge_divisible)
{
  /* for(i=0; i<10; i+=2): trips = ceil(10/2) = 5 */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 2, UT_GE), 5);
  UT_ASSERT_EQ(compute_trip_count(0, 10, 2, UT_UGE), 5); /* unsigned same */
  return 0;
}

UT_TEST(test_trip_count_ge_nondivisible_rounds_up)
{
  /* ceil(10/3) = 4 : i = 0,3,6,9 (exit when i>=10) */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 3, UT_GE), 4);
  return 0;
}

UT_TEST(test_trip_count_ge_range_zero_and_negative)
{
  UT_ASSERT_EQ(compute_trip_count(5, 5, 1, UT_GE), 0);  /* i>=limit at entry */
  UT_ASSERT_EQ(compute_trip_count(10, 5, 1, UT_GE), 0); /* range<0 */
  return 0;
}

UT_TEST(test_trip_count_gt_divisible_and_nondivisible)
{
  /* for(i=0; i<=10; i+=2): i=0,2,4,6,8,10 -> 6 = 10/2+1 */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 2, UT_GT), 6);
  /* i=0,3,6,9 (<=10) -> 4 = 10/3+1 */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 3, UT_GT), 4);
  return 0;
}

UT_TEST(test_trip_count_gt_range_zero_and_negative)
{
  UT_ASSERT_EQ(compute_trip_count(5, 5, 1, UT_GT), 1);  /* i=5<=5 once */
  UT_ASSERT_EQ(compute_trip_count(10, 5, 1, UT_GT), 0); /* range<0 */
  return 0;
}

UT_TEST(test_trip_count_ne_exact)
{
  /* exit when i==limit: i=0,2,4,6,8 (exit at 10) -> 5 = 10/2 */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 2, UT_NE), 5);
  return 0;
}

UT_TEST(test_trip_count_ne_zero_range_exits_immediately)
{
  UT_ASSERT_EQ(compute_trip_count(5, 5, 1, UT_NE), 0);
  return 0;
}

UT_TEST(test_trip_count_ne_negative_range_infinite)
{
  /* step>0 but limit below init: never reaches limit -> -1 (infinite) */
  UT_ASSERT_EQ(compute_trip_count(10, 5, 1, UT_NE), -1);
  return 0;
}

UT_TEST(test_trip_count_ne_not_divisible_infinite)
{
  /* 10/3 has remainder -> would step over limit forever -> -1 */
  UT_ASSERT_EQ(compute_trip_count(0, 10, 3, UT_NE), -1);
  return 0;
}

UT_TEST(test_trip_count_unsupported_cond)
{
  UT_ASSERT_EQ(compute_trip_count(0, 10, 1, UT_EQ), -1);
  UT_ASSERT_EQ(compute_trip_count(0, 10, 1, UT_LT), -1);
  UT_ASSERT_EQ(compute_trip_count(0, 10, 1, 0x00), -1);
  return 0;
}

UT_TEST(test_trip_count_huge_range_overflows_int_to_minus_one)
{
  /* init=INT_MIN, limit=INT_MAX, step=1: range = 2^32-1 (fits int64), but the
   * (int) cast of 4294967295 yields -1.  The function thus reports "cannot
   * compute" for a ~4-billion-iteration loop — conservative-safe (the caller
   * will not attempt to unroll it).  This pins the documented behaviour. */
  int r = compute_trip_count((int)0x80000000, (int)0x7fffffff, 1, UT_GE);
  UT_ASSERT_EQ(r, -1);
  return 0;
}

/* ============================================ find_induction_vars_ex */

UT_TEST(test_find_iv_basic_counting_loop)
{
  /* preheader: V0 = #0 ; body: V0 = V0 + #1 ; back-edge */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 1 header */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(UT_NE, I32), UTB_NONE); /* 3 */
  IRLoop L = utb_loop(1, 1, 3, 0);

  InductionVar ivs[4];
  int n = find_induction_vars_ex(ir, &L, ivs, 4, 0);

  UT_ASSERT_EQ(n, 1);
  UT_ASSERT_EQ(ivs[0].vreg, VR_VAR(0));
  UT_ASSERT_EQ(ivs[0].init_val, 0);
  UT_ASSERT_EQ(ivs[0].step, 1);
  UT_ASSERT_EQ(ivs[0].def_idx, 2);
  UT_ASSERT_EQ(ivs[0].init_idx, 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_iv_multiple_defs_not_iv)
{
  /* Two definitions of V0 inside the loop -> not a simple IV. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(2, I32)); /* 2 */
  IRLoop L = utb_loop(1, 1, 2, 0);

  InductionVar ivs[4];
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 0), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_iv_no_init_in_preheader_not_iv)
{
  /* No `V0 = #const` within the preheader window -> init_idx stays -1. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                   /* 0 preheader */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 1 */
  IRLoop L = utb_loop(1, 1, 1, 0);

  InductionVar ivs[4];
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 0), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_iv_non_var_dest_not_iv)
{
  /* IV must be a VAR vreg; a TEMP dest is rejected. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));
  IRLoop L = utb_loop(1, 1, 1, 0);

  InductionVar ivs[4];
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 0), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_iv_copy_through_allowed)
{
  /* `T1 = V0; V0 = T1 + #1` — with allow_copy_through=1 the temp is traced
   * back to V0 and the IV is recognised; with =0 it is not. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);    /* 0 preheader */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);   /* 1 T1=V0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_temp(1, I32), utb_imm(1, I32)); /* 2 */
  IRLoop L = utb_loop(1, 1, 2, 0);

  InductionVar ivs[4];
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 1), 1);
  UT_ASSERT_EQ(ivs[0].step, 1);
  UT_ASSERT_EQ(ivs[0].init_val, 0);

  /* Without copy-through, src1 (T1) != dest (V0) -> not an IV. */
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 0), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ find_loop_exit_condition */

UT_TEST(test_find_exit_top_tested)
{
  /* Header CMP V0,#10; JUMPIF GE -> 99 (exit outside loop). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 back-edge */
  IRLoop L = utb_loop(1, 1, 4, 0);

  int cmp = -1, jmp = -1, limit = -1, cond = -1, exit_t = -1;
  int found = find_loop_exit_condition(ir, &L, VR_VAR(0), &cmp, &jmp, &limit, &cond, &exit_t);

  UT_ASSERT_EQ(found, 1);
  UT_ASSERT_EQ(cmp, 1);
  UT_ASSERT_EQ(jmp, 2);
  UT_ASSERT_EQ(limit, 10);
  UT_ASSERT_EQ(cond, UT_GE);
  UT_ASSERT_EQ(exit_t, 99);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_exit_bottom_tested_rotated)
{
  /* Rotated loop: body, then `CMP V0,#10; JUMPIF LT -> header` (back-edge).
   * The condition is inverted (LT continue -> GE exit). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 preheader */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 1 header/body */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(1, I32), utb_imm(UT_LT, I32), UTB_NONE); /* 3 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 4 fall-through exit */
  IRLoop L = utb_loop(1, 1, 3, 0);

  int cmp = -1, jmp = -1, limit = -1, cond = -1, exit_t = -1;
  int found = find_loop_exit_condition(ir, &L, VR_VAR(0), &cmp, &jmp, &limit, &cond, &exit_t);

  UT_ASSERT_EQ(found, 1);
  UT_ASSERT_EQ(cmp, 2);
  UT_ASSERT_EQ(limit, 10);
  UT_ASSERT_EQ(cond, UT_GE); /* inverted from LT */
  UT_ASSERT_EQ(exit_t, 4);   /* fall-through past JUMPIF */
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_exit_cmp_not_on_iv_not_found)
{
  /* CMP is on a different vreg than the IV -> not the exit condition. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(1, I32), utb_imm(10, I32)); /* not V0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE);
  IRLoop L = utb_loop(1, 1, 2, 0);

  int cmp, jmp, limit, cond, exit_t;
  UT_ASSERT_EQ(find_loop_exit_condition(ir, &L, VR_VAR(0), &cmp, &jmp, &limit, &cond, &exit_t), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_exit_no_jumpif_after_cmp_not_found)
{
  /* CMP not immediately followed by JUMPIF -> not matched. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* not JUMPIF */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE);
  IRLoop L = utb_loop(1, 1, 3, 0);

  int cmp, jmp, limit, cond, exit_t;
  UT_ASSERT_EQ(find_loop_exit_condition(ir, &L, VR_VAR(0), &cmp, &jmp, &limit, &cond, &exit_t), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ integration: find IV + exit + trip */

UT_TEST(test_loop_iv_and_exit_yield_trip_count)
{
  /* A canonical `for(V0=0; V0<10; V0++)` loop: find the IV, find the exit,
   * and confirm compute_trip_count gives 10. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  IRLoop L = utb_loop(1, 1, 4, 0);

  InductionVar ivs[4];
  UT_ASSERT_EQ(find_induction_vars_ex(ir, &L, ivs, 4, 0), 1);

  int cmp, jmp, limit, cond, exit_t;
  UT_ASSERT_EQ(find_loop_exit_condition(ir, &L, ivs[0].vreg, &cmp, &jmp, &limit, &cond, &exit_t), 1);

  UT_ASSERT_EQ(compute_trip_count(ivs[0].init_val, limit, ivs[0].step, cond), 10);
  utb_free(ir);
  return 0;
}

/* ============================================ try_unroll_loop_ex (invariants) */

/* Build a canonical top-tested counting loop:
 *   0: ASSIGN V0 = #init        (preheader, preheader_idx=0)
 *   1: CMP V0, #limit           (header/start_idx=1)
 *   2: JUMPIF GE -> exit        (exit outside loop)
 *   3: <body>                   (one STORE [100]=V0 by default)
 *   4: ADD V0 = V0 + #step
 *   5: JUMP -> 1                (back-edge, end_idx=5)
 * Returns the exit_target index (6 by default; caller may append readers after). */
static int emit_unrollable_loop(TCCIRState *ir, int init, int limit, int step, IROperand body_op)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(init, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(limit, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=6 */
  if (irop_get_vreg(body_op) >= 0)
  {
    /* body is a STORE [100] = <vreg-or-imm> */
    utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), body_op, UTB_NONE); /* 3 */
  }
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(step, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);                 /* 5 */
  return 6;
}

/* Count ADDs "V1 = V1 + #imm" whose src2 is an immediate; fill vals[].
 * Returns the count.  Used to verify per-iteration IV substitution in a
 * register-only unrolled body. */
static int collect_add_iv_values(TCCIRState *ir, int *vals, int max)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index && n < max; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    if (utb_vreg(tcc_ir_op_get_dest(ir, q)) != VR_VAR(1))
      continue;
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(s))
      continue;
    vals[n++] = (int)irop_get_imm64_ex(ir, s);
  }
  return n;
}

/* Happy path: a register-only body is still unrolled.  The memory guard added
 * to collect_body_instructions rejects only memory-carrying bodies, so a purely
 * arithmetic accumulator loop -- for(V0=0; V0<3; V0++) V1 += V0 -- unrolls into
 * three copies of "V1 = V1 + #k" with the IV substituted per iteration. */
UT_TEST(test_unroll_register_body_three_iters)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 i=0 (preheader) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));        /* 1 header/start */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=6 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_var(0, I32)); /* 3 acc += i (body) */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 back-edge/end */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 6 exit target */
  IRLoop L = utb_loop(1, 1, 5, 0);

  int ret = try_unroll_loop_ex(ir, &L, NULL, 0);
  UT_ASSERT_EQ(ret, 1);

  /* IV init, the IV increment, and the back-edge are NOP'd (not replicated). */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP); /* init  */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP); /* iv inc */
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_NOP); /* back-edge */

  /* Three body copies "V1 = V1 + #k" with IV substituted to 0, 1, 2. */
  int vals[8];
  int n = collect_add_iv_values(ir, vals, 8);
  UT_ASSERT_EQ(n, 3);
  UT_ASSERT_EQ(vals[0], 0);
  UT_ASSERT_EQ(vals[1], 1);
  UT_ASSERT_EQ(vals[2], 2);
  utb_free(ir);
  return 0;
}

/* A memory-carrying body (STORE [100]=V0) is no longer unrolled: cloning
 * stack/aggregate accesses could expose stale initializer values through the
 * later forwarding passes (bitfield packed-RMW fuzz class, seed 163176), so
 * collect_body_instructions rejects any memory op and the loop is left intact. */
UT_TEST(test_unroll_store_body_blocks_unroll)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 3, 1, utb_var(0, I32));
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);

  /* Loop control and the store body are untouched (no clones written). */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN); /* IV init */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_JUMP);   /* back-edge intact */
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_store_body_used_after_blocks_unroll)
{
  /* Even with a post-loop reader of V0, the memory body blocks the unroll --
   * no clones, and no synthesized final-value ASSIGN V0=#3. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 3, 1, utb_var(0, I32));
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);          /* 6 (exit_target) spacer */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE); /* 7 reads V0 */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);

  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  int found_final = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ASSIGN)
      continue;
    if (utb_vreg(utb_dest(ir, i)) != VR_VAR(0))
      continue;
    if (irop_is_immediate(utb_src1(ir, i)) &&
        (int)irop_get_imm64_ex(ir, utb_src1(ir, i)) == 3)
      found_final = 1;
  }
  UT_ASSERT(!found_final);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_store_body_single_iter_blocks_unroll)
{
  /* trip_count = 1 is irrelevant once the body carries memory -- still blocked. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 7, 8, 1, utb_var(0, I32)); /* init=7, limit=8 -> 1 trip */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_trip_over_max_skips)
{
  /* limit=20 -> trip_count=20 > UNROLL_MAX_TRIP_COUNT(16) -> skip. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 20, 1, utb_var(0, I32));
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  /* Loop control is untouched. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_trip_zero_skips)
{
  /* init=5, limit=5 -> range=0, GE -> 0 trips -> skip. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 5, 5, 1, utb_var(0, I32));
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_no_unroll_flag_skips)
{
  /* back-edge marked no_unroll (e.g. by the reroll pass) -> skip. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 3, 1, utb_var(0, I32));
  ir->compact_instructions[5].no_unroll = 1; /* back-edge */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_body_with_call_skips)
{
  /* A function call in the body is a side effect -> collect_body rejects. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym foo;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &foo, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));                  /* 3 body call */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);         /* 5 */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_no_iv_skips)
{
  /* No IV increment in the loop -> find_induction_vars returns 0 -> skip. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);         /* 4 (no IV inc) */
  IRLoop L = utb_loop(1, 1, 4, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_body_internal_jumpif_skips)
{
  /* An internal JUMPIF in the body -> collect_body rejects (too complex). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(3, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(UT_NE, I32), UTB_NONE); /* 3 internal */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);         /* 5 */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ loop_size_cmp */

UT_TEST(test_loop_size_cmp_orders_ascending_by_span)
{
  /* Comparator used by qsort(loops->loops, ...) in opt_loop_dead.c / opt_loop.c:
   * span = end_idx - start_idx.  Ascending order -> smaller loops first. */
  IRLoop small = utb_loop(1, 1, 3, 0);  /* span=2 */
  IRLoop big = utb_loop(10, 10, 40, 9); /* span=30 */
  IRLoop equal_a = utb_loop(1, 1, 5, 0);
  IRLoop equal_b = utb_loop(20, 20, 24, 19); /* same span=4, different position */

  UT_ASSERT(loop_size_cmp(&small, &big) < 0);
  UT_ASSERT(loop_size_cmp(&big, &small) > 0);
  UT_ASSERT_EQ(loop_size_cmp(&equal_a, &equal_b), 0);

  /* qsort() end-to-end: array sorts ascending by span. */
  IRLoop arr[3];
  arr[0] = big;
  arr[1] = small;
  arr[2] = equal_a;
  qsort(arr, 3, sizeof(IRLoop), loop_size_cmp);
  UT_ASSERT_EQ(arr[0].header_idx, small.header_idx);
  UT_ASSERT_EQ(arr[2].header_idx, big.header_idx);
  return 0;
}

/* ============================================ transform_derived_iv */

UT_TEST(test_transform_derived_iv_skips_memory_feeding_div)
{
  /* A DIV whose computed address is dereferenced inside the loop must be
   * SKIPPED (docs/bugs.md #2): the escape scan in transform_derived_iv
   * (sr_div_value_stays_in_regs) sees the lval read of the address temp and
   * disqualifies the DIV.  The function is a no-op and all out-params keep
   * their "nothing happened" sentinels. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_init_temp_intervals(ir, /*reserved=*/4); /* TEMP0..3 hand-used below */

  /* Counted loop V0 with a derived pointer `T2 = base + V0*4` used by a LOAD. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 preheader init */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));         /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 3 shl */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_stackoff(200, 0, 0, 0, I32),
           utb_temp(1, I32));                                                     /* 4 addr = base+shl */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_lval(utb_temp(2, I32)), UTB_NONE); /* 5 load *addr */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 6 iv inc */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 7 back-edge */

  IRLoop L = utb_loop(1, 1, 7, 0);
  InductionVar iv;
  iv.vreg = VR_VAR(0);
  iv.init_val = 0;
  iv.step = 1;
  iv.def_idx = 6;
  iv.init_idx = 0;

  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.iv_idx = 0;
  div.base_vreg = -1; /* stackoff base has no vreg */
  div.base_op = utb_stackoff(200, 0, 0, 0, I32);
  div.stride = 4;
  div.use_idx = 4;
  div.shl_idx = 3;
  div.share_with = -1;

  int out_ptr_vreg = 12345, out_idx_shift = 12345, out_postnop = 12345, out_stride_pos = 12345;
  int n_before = ir->next_instruction_index;

  int ret = transform_derived_iv(ir, &L, &iv, &div, &out_ptr_vreg, &out_idx_shift, &out_postnop, &out_stride_pos, -1);

  UT_ASSERT_EQ(ret, 0);
  /* Out-params reset to "nothing happened" sentinels. */
  UT_ASSERT_EQ(out_ptr_vreg, -1);
  UT_ASSERT_EQ(out_idx_shift, 0);
  UT_ASSERT_EQ(out_postnop, -1);
  UT_ASSERT_EQ(out_stride_pos, -1);
  /* No instructions inserted, nothing rewritten. */
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

UT_TEST(test_transform_derived_iv_reduces_register_only_div)
{
  /* POSITIVE case: a derived IV whose address value stays in registers
   * (accumulated into V1 — never dereferenced, stored, or passed to a call)
   * IS strength-reduced:
   *   - `ptr = base` inserted in the preheader (init_val == 0 → 1 instr),
   *   - the SHL is NOPed,
   *   - the ADD use site becomes `ASSIGN T2, ptr`,
   *   - `ptr += stride` is inserted after the IV increment. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  utb_init_temp_intervals(ir, /*reserved=*/4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);       /* 0 preheader init */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));         /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 3 shl */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_stackoff(200, 0, 0, 0, I32),
           utb_temp(1, I32));                                                     /* 4 addr = base+shl */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_temp(2, I32)); /* 5 acc += addr (reg-only) */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 6 iv inc */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 7 back-edge */

  IRLoop L = utb_loop(1, 1, 7, 0);
  InductionVar iv;
  iv.vreg = VR_VAR(0);
  iv.init_val = 0;
  iv.step = 1;
  iv.def_idx = 6;
  iv.init_idx = 0;

  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.iv_idx = 0;
  div.base_vreg = -1;
  div.base_op = utb_stackoff(200, 0, 0, 0, I32);
  div.stride = 4;
  div.use_idx = 4;
  div.shl_idx = 3;
  div.share_with = -1;

  int out_ptr_vreg = 12345, out_idx_shift = 12345, out_postnop = 12345, out_stride_pos = 12345;

  int ret = transform_derived_iv(ir, &L, &iv, &div, &out_ptr_vreg, &out_idx_shift, &out_postnop, &out_stride_pos, -1);

  UT_ASSERT_EQ(ret, 3); /* full success: init + replace + stride */
  UT_ASSERT(out_ptr_vreg >= 0);
  UT_ASSERT_EQ(out_idx_shift, 1); /* init_val == 0 → single `ptr = base` ASSIGN */
  UT_ASSERT_EQ(out_postnop, -1);  /* no INDEXED rewrite → no postnop slot */

  /* Layout after the two insertions (init at 1, stride at 8):
   *   0: ASSIGN V0, #0
   *   1: ASSIGN ptr, StackOff(200)   <- inserted init
   *   2: CMP V0, #5
   *   3: JUMPIF ->101 (99 shifted by both inserts)
   *   4: NOP                          <- was SHL
   *   5: ASSIGN T2, ptr               <- was ADD (use site)
   *   6: ADD V1, V1, T2
   *   7: ADD V0, V0, #1               <- iv inc
   *   8: ADD ptr, ptr, #4             <- inserted stride
   *   9: JUMP ->2                     (back-edge, retargeted 1->2)
   */
  UT_ASSERT_EQ(out_stride_pos, 8);
  UT_ASSERT_EQ(ir->next_instruction_index, 10);

  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 1)), out_ptr_vreg);

  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP); /* SHL removed */

  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_ASSIGN); /* use site: T2 <- ptr */
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 5)), utb_vreg(utb_temp(2, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 5)), out_ptr_vreg);

  UT_ASSERT_EQ(utb_op(ir, 8), TCCIR_OP_ADD); /* ptr += 4 after iv inc */
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 8)), out_ptr_vreg);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 8)), 4);

  UT_ASSERT_EQ(utb_op(ir, 9), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 9)), 2);
  /* Exit target 99 shifted by BOTH insertions (init at 1, stride at 8). */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 3)), 101);

  utb_free(ir);
  return 0;
}

UT_TEST(test_transform_derived_iv_shared_path_refused)
{
  /* Shared-pointer rewrites (shared_ptr_vreg >= 0) are refused: the rewrite
   * ran no escape analysis and could not prove the shared use executes before
   * the primary's `ptr += stride` bump (docs/bugs.md #2).  Duplicates are
   * instead re-detected as independent primaries by the driver loop. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_stackoff(200, 0, 0, 0, I32), utb_temp(1, I32)); /* 0 */
  IRLoop L = utb_loop(0, 0, 0, -1);
  InductionVar iv = {0};
  iv.vreg = VR_VAR(0);
  iv.step = 1;
  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.use_idx = 0;
  div.shl_idx = -1;
  div.share_with = -1;

  int ret = transform_derived_iv(ir, &L, &iv, &div, NULL, NULL, NULL, NULL, /*shared_ptr_vreg=*/5);
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD); /* untouched: not rewritten to ASSIGN */
  utb_free(ir);
  return 0;
}

/* ============================================ insert_instr_at */

UT_TEST(test_insert_instr_at_shifts_and_retargets_jumps)
{
  /*   0: ASSIGN V0 = #0
   *   1: JUMP -> 0        (target BEFORE pos=2 -> must NOT shift)
   *   2: JUMP -> 3        (target AT/AFTER pos=2 -> must shift to 4)
   *   3: NOP
   * insert_instr_at(ir, pos=2, ADD, ...) inserts a new instruction at index 2,
   * pushing the old [2,3] down to [3,4], and retargets any JUMP/JUMPIF whose
   * destination was >= pos. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE, UTB_NONE);          /* 1 target=0 (< pos) */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);          /* 2 target=3 (>= pos) */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 3 */

  int max_orig_before = ir->max_orig_index;
  int n_before = ir->next_instruction_index;

  IROperand dest = utb_temp(5, I32);
  IROperand src1 = utb_imm(7, I32);
  IROperand src2 = utb_imm(9, I32);
  int pos = insert_instr_at(ir, 2, TCCIR_OP_ADD, dest, src1, src2);

  UT_ASSERT_EQ(pos, 2);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1);

  /* New instruction at index 2. */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 2)), utb_vreg(dest));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 2)), 7);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 2)), 9);
  /* Fresh orig_index, strictly greater than anything used before. */
  UT_ASSERT(ir->compact_instructions[2].orig_index > max_orig_before);
  UT_ASSERT_EQ(ir->max_orig_index, max_orig_before + 1);

  /* Old index1 (JUMP->0, target < pos) is unchanged (still at index1, target
   * still 0). */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 1)), 0);

  /* Old index2 (JUMP->3) shifted to index3; its target (3 >= pos) retargeted
   * to 4. */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 3)), 4);

  /* Old index3 (NOP) shifted to index4. */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NOTE: insert_instr_at's pool-exhaustion path (iroperand_pool_count + 3 >
 * iroperand_pool_capacity even after tcc_ir_pool_ensure) is NOT covered here.
 * tcc_ir_pool_ensure (ir/pool.c) doubles iroperand_pool_capacity until it
 * fits the request; called with iroperand_pool_capacity==0 (i.e. without
 * utb_pools_init/tcc_ir_pools_init first), `capacity *= 2` never leaves 0,
 * looping forever.  That is only reachable in a test harness that skips the
 * normal pool-init call, so it isn't a production-reachable path (every real
 * caller runs through tcc_ir_pools_init, which seeds capacity to 64) — but it
 * is a latent hang hazard in tcc_ir_pool_ensure if capacity is ever 0. Not
 * fixed here (production code, out of scope for this test-only change); see
 * summary for the flagged bug. Exercising the true out-of-memory branch
 * would require exhausting the address space, so this path is left
 * untested. */

/* ============================================ try_eliminate_loop */

UT_TEST(test_eliminate_loop_pure_counter_and_accumulator)
{
  /* for (i=0; i<5; i++) count = count + 3;
   * Body has only IV updates (i and count are both simple IVs) -> eliminable.
   * Final values: i=5, count=init_count+15.  Both used after the loop. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 i=0 (preheader) */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(100, I32), UTB_NONE); /* 1 count=100 (preheader) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));      /* 2 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 3 exit=7 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(1, I32), utb_var(1, I32), utb_imm(3, I32)); /* 4 count += 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 7 exit_target */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);    /* 8 reads i */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(1, I32), UTB_NONE);    /* 9 reads count */

  IRLoop L = utb_loop(2, 2, 6, 1);
  int ret = try_eliminate_loop(ir, &L);
  UT_ASSERT_EQ(ret, 1);

  /* Whole [start_idx..end_idx] region NOP'd except the two final-value
   * ASSIGN writes.  Find them and check the values. */
  int found_i = 0, found_count = 0;
  for (int i = L.start_idx; i <= L.end_idx; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ASSIGN)
      continue;
    int32_t dvr = utb_vreg(utb_dest(ir, i));
    int32_t val = (int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i));
    if (dvr == VR_VAR(0))
    {
      UT_ASSERT_EQ(val, 5); /* i final = 0 + 5*1 */
      found_i = 1;
    }
    else if (dvr == VR_VAR(1))
    {
      UT_ASSERT_EQ(val, 115); /* count final = 100 + 5*3 */
      found_count = 1;
    }
  }
  UT_ASSERT(found_i);
  UT_ASSERT(found_count);

  /* Preheader inits are NOP'd too. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_loop_unused_iv_gets_no_final_assign)
{
  /* for (i=0; i<5; i++) ;  i never read after the loop -> no ASSIGN written,
   * the whole region collapses to NOPs only. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=5 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 5 exit_target */

  IRLoop L = utb_loop(1, 1, 4, 0);
  UT_ASSERT_EQ(try_eliminate_loop(ir, &L), 1);

  for (int i = L.start_idx; i <= L.end_idx; i++)
    UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_loop_side_effect_body_blocked)
{
  /* Body contains a STORE (side effect, not an IV update) -> not eliminable. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 */

  IRLoop L = utb_loop(1, 1, 5, 0);
  UT_ASSERT_EQ(try_eliminate_loop(ir, &L), 0);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE); /* untouched */
  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_loop_no_iv_gives_up)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE); /* 0 preheader, no init */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE); /* 1 */
  IRLoop L = utb_loop(1, 1, 1, 0);
  UT_ASSERT_EQ(try_eliminate_loop(ir, &L), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_loop_zero_trip_gives_up)
{
  /* init==limit with GE -> trip_count=0 -> try_eliminate_loop requires >0. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 4 */
  IRLoop L = utb_loop(1, 1, 4, 0);
  UT_ASSERT_EQ(try_eliminate_loop(ir, &L), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ find_derived_ivs */

UT_TEST(test_find_derived_ivs_shl_add_pattern)
{
  /* Classic address-computation DIV: T1 = V0 << 2; T2 = base + T1; STORE [T2].
   * base is an immediate (no vreg) so it's trivially loop-invariant.
   * find_derived_ivs requires loop->body_instrs[] populated explicitly. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);      /* 0 preheader */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 1 shl */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(200, I32), utb_temp(1, I32)); /* 2 addr=base+shl */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_var(0, I32), UTB_NONE); /* 3 STORE *addr=V0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 4 iv inc */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 5 back-edge */

  IRLoop L = utb_loop(1, 1, 5, 0);
  int body[] = {1, 2, 3, 4};
  L.body_instrs = body;
  L.num_body_instrs = 4;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  int num_divs = find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4);
  UT_ASSERT_EQ(num_divs, 1);
  UT_ASSERT_EQ(divs[0].iv_idx, 0);
  UT_ASSERT_EQ(divs[0].stride, 4); /* step(1) * (1<<shift(2)) */
  UT_ASSERT_EQ(divs[0].use_idx, 2);
  UT_ASSERT_EQ(divs[0].shl_idx, 1);
  UT_ASSERT_EQ(divs[0].share_with, -1);

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_mul_variant_and_operand_order)
{
  /* T1 = V0 * 8 (MUL, not SHL); T2 = T1 + base (SHL/MUL result in src1, base
   * in src2 — the "check src1" fallback path). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);      /* 0 */
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_var(0, I32), utb_imm(8, I32)); /* 1 mul */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(200, I32)); /* 2 addr=mul+base */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 5 */

  IRLoop L = utb_loop(1, 1, 5, 0);
  int body[] = {1, 2, 3, 4};
  L.body_instrs = body;
  L.num_body_instrs = 4;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  int num_divs = find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4);
  UT_ASSERT_EQ(num_divs, 1);
  UT_ASSERT_EQ(divs[0].stride, 8); /* step(1) * mul_const(8) */
  UT_ASSERT_EQ(divs[0].use_idx, 2);
  UT_ASSERT_EQ(divs[0].shl_idx, 1);

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_shl_multi_use_not_nopable_skipped)
{
  /* Same SHL/ADD shape, but the SHL result T1 is ALSO used by a second
   * instruction -> shl_vr_uses != 1 -> the DIV must be rejected (can't NOP
   * the SHL out from under the other use). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);      /* 0 */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 1 shl */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(200, I32), utb_temp(1, I32)); /* 2 addr */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(300, 1, 0, 0, I32), utb_temp(1, I32), UTB_NONE); /* 4 2nd use of T1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 6 */

  IRLoop L = utb_loop(1, 1, 6, 0);
  int body[] = {1, 2, 3, 4, 5};
  L.body_instrs = body;
  L.num_body_instrs = 5;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  UT_ASSERT_EQ(find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_dead_add_result_skipped)
{
  /* T1 = V0 << 2; T2 = base + T1 — but T2 is never used anywhere (dead) ->
   * use_count < 1 -> rejected. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);      /* 0 */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(200, I32), utb_temp(1, I32)); /* 2 dead */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 4 */

  IRLoop L = utb_loop(1, 1, 4, 0);
  int body[] = {1, 2, 3};
  L.body_instrs = body;
  L.num_body_instrs = 3;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  UT_ASSERT_EQ(find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_mla_fused_pattern)
{
  /* Second pass: MLA-fused DIV.  dest = V0 * 4 + accum, accum loop-invariant
   * (a STACKOFF base, never redefined in the loop). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit4(ir, TCCIR_OP_MLA, utb_temp(1, I32), utb_var(0, I32), utb_imm(4, I32),
            utb_stackoff(200, 0, 0, 0, I32));                                /* 1 mla */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_var(0, I32), UTB_NONE); /* 2 use */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 4 */

  IRLoop L = utb_loop(1, 1, 4, 0);
  int body[] = {1, 2, 3};
  L.body_instrs = body;
  L.num_body_instrs = 3;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  int num_divs = find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4);
  UT_ASSERT_EQ(num_divs, 1);
  UT_ASSERT_EQ(divs[0].iv_idx, 0);
  UT_ASSERT_EQ(divs[0].stride, 4);
  UT_ASSERT_EQ(divs[0].use_idx, 1);
  UT_ASSERT_EQ(divs[0].shl_idx, -1); /* fused — nothing to NOP */

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_mla_base_redefined_in_loop_skipped)
{
  /* Same MLA shape, but the accum base vreg is redefined inside the loop ->
   * not loop-invariant -> rejected.  The redefinition is a plain ASSIGN
   * (not `V2 = V2 + const`) so it does NOT itself look like a second IV to
   * find_induction_vars_ex — keeping num_ivs==1 and isolating exactly the
   * "base redefined" gate in find_derived_ivs's MLA pass. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);   /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(200, I32), UTB_NONE); /* 1 base init (preheader-ish) */
  utb_emit4(ir, TCCIR_OP_MLA, utb_temp(1, I32), utb_var(0, I32), utb_imm(4, I32), utb_var(2, I32)); /* 2 mla */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(2, I32), utb_imm(999, I32), UTB_NONE);   /* 4 base redefined! */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 6 */

  IRLoop L = utb_loop(2, 2, 6, 1);
  int body[] = {2, 3, 4, 5};
  L.body_instrs = body;
  L.num_body_instrs = 4;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  UT_ASSERT_EQ(find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_indexed_load_eliminable_pattern)
{
  /* Third pass: LOAD_INDEXED whose index is the IV, with the IV eliminable
   * (its only other uses are the self-increment and one CMP against an
   * immediate).  base is a STACKOFF (no vreg -> trivially invariant). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 1 header cmp */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit */
  utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32), utb_stackoff(200, 0, 0, 0, I32),
            utb_var(0, I32), utb_imm(2, I32));                               /* 3 val = base[V0<<2] */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(300, 1, 0, 0, I32), utb_temp(1, I32), UTB_NONE); /* 4 use val */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 5 iv inc */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 6 back-edge */

  IRLoop L = utb_loop(1, 1, 6, 0);
  int body[] = {3, 4, 5};
  L.body_instrs = body;
  L.num_body_instrs = 3;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  int num_divs = find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4);
  UT_ASSERT_EQ(num_divs, 1);
  UT_ASSERT_EQ(divs[0].stride, 4); /* step(1) * (1<<scale(2)) */
  UT_ASSERT_EQ(divs[0].use_idx, 3);
  UT_ASSERT_EQ(divs[0].shl_idx, -1); /* shift encoded in scale field */

  utb_free(ir);
  return 0;
}

UT_TEST(test_find_derived_ivs_indexed_load_not_eliminable_skipped)
{
  /* Same shape, but V0 has an EXTRA use beyond the CMP/increment/indexed
   * access (here, a second STORE reading V0 directly) -> not eliminable ->
   * the eliminability gate rejects the DIV (cost without compensating
   * saving). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32), utb_stackoff(200, 0, 0, 0, I32),
            utb_var(0, I32), utb_imm(2, I32));                               /* 3 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(300, 1, 0, 0, I32), utb_temp(1, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(400, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE);  /* 5 extra use of V0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 6 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 7 */

  IRLoop L = utb_loop(1, 1, 7, 0);
  int body[] = {3, 4, 5, 6};
  L.body_instrs = body;
  L.num_body_instrs = 4;

  InductionVar ivs[4];
  int num_ivs = find_induction_vars_ex(ir, &L, ivs, 4, 0);
  UT_ASSERT_EQ(num_ivs, 1);

  DerivedIV divs[4];
  UT_ASSERT_EQ(find_derived_ivs(ir, &L, ivs, num_ivs, divs, 4), 0);

  utb_free(ir);
  return 0;
}

/* ============================================ find_loop_exit_condition_op */

UT_TEST(test_find_exit_op_immediate_limit_matches_int_version)
{
  /* Same shape as test_find_exit_top_tested: the _op variant must find the
   * same CMP/JUMPIF/cond/exit and report the limit as an immediate operand. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(10, I32));   /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  IRLoop L = utb_loop(1, 1, 4, 0);

  int cmp = -1, jmp = -1, cond = -1, exit_t = -1;
  IROperand limit_op;
  int found = find_loop_exit_condition_op(ir, &L, VR_VAR(0), &cmp, &jmp, &limit_op, &cond, &exit_t);

  UT_ASSERT_EQ(found, 1);
  UT_ASSERT_EQ(cmp, 1);
  UT_ASSERT_EQ(jmp, 2);
  UT_ASSERT(irop_is_immediate(limit_op));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, limit_op), 10);
  UT_ASSERT_EQ(cond, UT_GE);
  UT_ASSERT_EQ(exit_t, 99);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_exit_op_symbolic_vreg_limit)
{
  /* `for (i=0; i<n; i++)` where n is a plain vreg (symbolic, non-immediate
   * limit) — the feature find_loop_exit_condition (int-only) cannot handle. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 i=0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_var(1, I32));    /* 1 CMP i, n */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);           /* 4 */
  IRLoop L = utb_loop(1, 1, 4, 0);

  int cmp = -1, jmp = -1, cond = -1, exit_t = -1;
  IROperand limit_op;
  int found = find_loop_exit_condition_op(ir, &L, VR_VAR(0), &cmp, &jmp, &limit_op, &cond, &exit_t);

  UT_ASSERT_EQ(found, 1);
  UT_ASSERT_EQ(cmp, 1);
  UT_ASSERT(!irop_is_immediate(limit_op));
  UT_ASSERT_EQ(utb_vreg(limit_op), VR_VAR(1));
  UT_ASSERT_EQ(cond, UT_GE);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_exit_op_lval_limit_rejected)
{
  /* CMP i, *ptr (an lval dereference as src2) — not a "simple vreg" per
   * src2_is_simple_vreg's is_lval guard, so this CMP must NOT match. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_lval(utb_var(1, I32))); /* 1 CMP i, *p */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE);
  IRLoop L = utb_loop(1, 1, 2, 0);

  int cmp, jmp, cond, exit_t;
  IROperand limit_op;
  UT_ASSERT_EQ(find_loop_exit_condition_op(ir, &L, VR_VAR(0), &cmp, &jmp, &limit_op, &cond, &exit_t), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ try_eliminate_iv_counter */

UT_TEST(test_eliminate_iv_counter_pretest_only_rewrites_to_ptr_cmp)
{
  /* Top-tested loop, header CMP is the ONLY exit test (no back-edge CMP).
   * Simulates the post-transform_derived_iv state directly (bypassing the
   * disabled transform_derived_iv): a plausible ptr_vreg is supplied by
   * hand, matching what transform_derived_iv would have allocated.
   * reserved=1 marks TEMP0 (our hand-picked ptr_vreg) as already allocated
   * so the function's own tcc_ir_vreg_alloc_temp() (for end_vreg) returns a
   * distinct, fresh TEMP position instead of colliding with TEMP0. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_init_temp_intervals(ir, /*reserved=*/1);
  /* try_eliminate_iv_counter succeeds here and calls insert_instr_at, which
   * grows compact_instructions[] via compact_instructions_size; utb_new()
   * leaves that at 0, so it must be pre-set to the real allocated capacity
   * (see test_opt_licm.c / test_opt_reroll.c's identical utb_*_new() pattern). */
  ir->compact_instructions_size = UTB_MAX_INSTR;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 i=0 (preheader) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 header */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 exit=99 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 3 i++ */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 4 back-edge */

  IRLoop L = utb_loop(1, 1, 4, 0);
  InductionVar iv;
  iv.vreg = VR_VAR(0);
  iv.init_val = 0;
  iv.step = 1;
  iv.def_idx = 3;
  iv.init_idx = 0;

  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.stride = 4;
  div.share_with = -1;

  int ptr_vreg = VR_TEMP(0);
  int ret = try_eliminate_iv_counter(ir, &L, &iv, &div, ptr_vreg, /*idx_shift=*/0);
  UT_ASSERT_EQ(ret, 1);

  /* One instruction inserted at the header (end_ptr = ptr + limit*element_size). */
  UT_ASSERT_EQ(ir->next_instruction_index, 6);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP); /* old init NOP'd */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD); /* end_ptr = ptr + 20 */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 1)), ptr_vreg);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 1)), 20); /* limit(5)*element_size(4) */

  int end_vreg = utb_vreg(utb_dest(ir, 1));
  UT_ASSERT(end_vreg >= 0);
  UT_ASSERT(end_vreg != ptr_vreg); /* fresh temp, distinct from the pointer */

  /* Old header CMP (shifted to index 2) rewritten to ptr vs end_ptr. */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 2)), ptr_vreg);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, 2)), end_vreg);

  /* JUMPIF condition rewritten GE -> UGE (unsigned pointer compare). */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 3)), UT_UGE);

  /* IV increment NOP'd (shifted to index 4). */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_iv_counter_other_use_blocks_elimination)
{
  /* IV read by an extra instruction beyond CMP/increment -> other_uses>0 ->
   * must decline, leaving the IR completely untouched. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_init_temp_intervals(ir, /*reserved=*/1);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(99, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 3 extra use */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 */

  IRLoop L = utb_loop(1, 1, 5, 0);
  InductionVar iv;
  iv.vreg = VR_VAR(0);
  iv.init_val = 0;
  iv.step = 1;
  iv.def_idx = 4;
  iv.init_idx = 0;
  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.stride = 4;
  div.share_with = -1;

  int n_before = ir->next_instruction_index;
  UT_ASSERT_EQ(try_eliminate_iv_counter(ir, &L, &iv, &div, VR_TEMP(0), 0), 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

UT_TEST(test_eliminate_iv_counter_no_cmp_found_declines)
{
  /* No CMP+JUMPIF anywhere testing the IV -> both hdr and back-edge scans
   * fail -> return 0. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_init_temp_intervals(ir, /*reserved=*/1);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 1 header (no CMP) */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);          /* 3 */

  IRLoop L = utb_loop(1, 1, 3, 0);
  InductionVar iv;
  iv.vreg = VR_VAR(0);
  iv.init_val = 0;
  iv.step = 1;
  iv.def_idx = 2;
  iv.init_idx = 0;
  DerivedIV div;
  memset(&div, 0, sizeof div);
  div.stride = 4;
  div.share_with = -1;

  UT_ASSERT_EQ(try_eliminate_iv_counter(ir, &L, &iv, &div, VR_TEMP(0), 0), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ iv_strength_reduction_core */

UT_TEST(test_iv_sr_core_skips_loop_without_preheader)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE, UTB_NONE);

  IRLoops loops;
  memset(&loops, 0, sizeof loops);
  IRLoop one = utb_loop(0, 0, 2, -1); /* preheader_idx = -1 -> skipped */
  loops.loops = &one;
  loops.num_loops = 1;

  UT_ASSERT_EQ(iv_strength_reduction_core(ir, &loops), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_core_no_ivs_yields_zero_changes)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE); /* 0 preheader */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE); /* 1 */

  IRLoops loops;
  memset(&loops, 0, sizeof loops);
  IRLoop one = utb_loop(1, 1, 1, 0);
  loops.loops = &one;
  loops.num_loops = 1;

  UT_ASSERT_EQ(iv_strength_reduction_core(ir, &loops), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_core_ivs_but_no_divs_yields_zero_changes)
{
  /* An IV with no derived-pointer use in the loop -> find_derived_ivs
   * returns 0 -> iv_strength_reduction_core moves on without touching IR. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);

  IRLoops loops;
  memset(&loops, 0, sizeof loops);
  IRLoop one = utb_loop(1, 1, 2, 0);
  loops.loops = &one;
  loops.num_loops = 1;

  int n_before = ir->next_instruction_index;
  UT_ASSERT_EQ(iv_strength_reduction_core(ir, &loops), 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  utb_free(ir);
  return 0;
}

UT_TEST(test_iv_sr_core_memory_feeding_div_yields_zero)
{
  /* A loop WITH a genuine DIV (find_derived_ivs succeeds) whose address is
   * stored through (`STORE *T2`) produces zero total_changes end-to-end:
   * transform_derived_iv's escape scan disqualifies memory-feeding DIVs
   * (see test_transform_derived_iv_skips_memory_feeding_div and
   * docs/bugs.md #2). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);      /* 0 preheader */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_var(0, I32), utb_imm(2, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(200, I32), utb_temp(1, I32)); /* 2 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_var(0, I32), UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));  /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);               /* 5 */

  IRLoops loops;
  memset(&loops, 0, sizeof loops);
  IRLoop one = utb_loop(1, 1, 5, 0);
  int body[] = {1, 2, 3, 4};
  one.body_instrs = body;
  one.num_body_instrs = 4;
  loops.loops = &one;
  loops.num_loops = 1;

  int n_before = ir->next_instruction_index;
  UT_ASSERT_EQ(iv_strength_reduction_core(ir, &loops), 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_SHL); /* untouched: transform never fires */
  utb_free(ir);
  return 0;
}

/* ============================================ try_rotate_loop */

/* Build a canonical rotate-eligible top-tested loop:
 *   0: ASSIGN V0 = #0        preheader
 *   1: CMP V0, #5            header (hi)
 *   2: JUMPIF GE -> 8        exit_target=8
 *   3: JUMP -> 6             body_start=6
 *   4: ADD V0 = V0 + #1      latch (IV increment)
 *   5: JUMP -> 1             back-edge (backedge_idx=5, targets hi)
 *   6: STORE [100] = V0      body
 *   7: JUMP -> 4             body -> latch
 *   8: RETURNVOID            exit_target (a real instr, NOT a NOP, so the
 *                            "does fall-through reach exit_target" skip-NOP
 *                            scan in try_rotate_loop stops exactly here)
 * Returns the loop (header_idx=1, start_idx=1, end_idx=7, preheader_idx=0). */
static IRLoop emit_rotatable_loop(TCCIRState *ir)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);     /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);              /* 3 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);              /* 5 */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 6 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);              /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);               /* 8 exit_target */
  return utb_loop(1, 1, 7, 0);
}

UT_TEST(test_rotate_loop_basic_top_tested_becomes_bottom_tested)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  IRLoop L = emit_rotatable_loop(ir);

  int ret = try_rotate_loop(ir, &L);
  UT_ASSERT_EQ(ret, 1);

  /* Old header CMP/JUMPIF (indices 1,2) are left in place (still may be
   * targeted from outside) — untouched. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMPIF);

  /* Relocated body at index 3 (region_start = hi+2 = 3): the STORE, now the
   * back-edge target. */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_STORE);
  UT_ASSERT_EQ(ir->compact_instructions[3].is_jump_target, 1);

  /* Latch (IV increment) follows at index 4. */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_ADD);

  /* Tail CMP (duplicate of header test) at index 5. */
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 5)), VR_VAR(0));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 5)), 5);

  /* Tail JUMPIF with inverted condition (GE -> LT), targeting the relocated
   * body (index 3). */
  UT_ASSERT_EQ(utb_op(ir, 6), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 6)), UT_LT);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 6)), 3);

  /* Slot 7 is the unused fifth NOP'd slot (only 4 of the 5 available slots
   * were needed: body + latch + tail CMP + tail JUMPIF).  Fall-through from
   * index 6 reaches index 7 (NOP) then index 8 (exit_target, RETURNVOID) —
   * no explicit exit JUMP was needed since fall-through already lands there. */
  UT_ASSERT_EQ(utb_op(ir, 7), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 8), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  return 0;
}

UT_TEST(test_rotate_loop_missing_header_jump_declines)
{
  /* Header pattern must be CMP, JUMPIF, JUMP — replace the body-entry JUMP
   * at hi+2 with something else -> reject before any mutation. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* not JUMP */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);
  IRLoop L = utb_loop(1, 1, 4, 0);

  UT_ASSERT_EQ(try_rotate_loop(ir, &L), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_rotate_loop_no_backedge_declines)
{
  /* No JUMP anywhere targets the header -> backedge_idx stays -1 -> reject. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));    /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(UT_GE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);          /* 3 body_start=6 */
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32)); /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);           /* 5 no back-edge! */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE); /* 6 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);          /* 7 */
  IRLoop L = utb_loop(1, 1, 7, 0);

  UT_ASSERT_EQ(try_rotate_loop(ir, &L), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP); /* untouched */
  utb_free(ir);
  return 0;
}

UT_TEST(test_rotate_loop_call_in_body_declines)
{
  /* A function call in the body blocks rotation (call-clobbered live ranges
   * across the rotated shape). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
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
           utb_imm((int32_t)TCCIR_ENCODE_CALL(0, 0), I32));                      /* 6 body call */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);              /* 7 */
  IRLoop L = utb_loop(1, 1, 7, 0);

  UT_ASSERT_EQ(try_rotate_loop(ir, &L), 0);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_CMP); /* untouched */
  utb_free(ir);
  return 0;
}

UT_TEST(test_rotate_loop_uninvertible_cond_declines)
{
  /* JUMPIF's condition token is not a recognised relational op ->
   * invert_condition returns -1 -> reject after all structural checks pass. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(9, I32), utb_imm(0x00, I32), UTB_NONE); /* bogus cond */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(6, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  IRLoop L = utb_loop(1, 1, 7, 0);

  UT_ASSERT_EQ(try_rotate_loop(ir, &L), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_loop_utils)
{
  UT_COVERS("signed_to_unsigned_cond");
  UT_COVERS("compute_trip_count");
  UT_COVERS("find_induction_vars_ex");
  UT_COVERS("find_loop_exit_condition");
  UT_COVERS("try_unroll_loop_ex");
  UT_COVERS("loop_size_cmp");
  UT_COVERS("transform_derived_iv");
  UT_COVERS("try_eliminate_loop");
  UT_COVERS("find_derived_ivs");
  UT_COVERS("find_loop_exit_condition_op");
  UT_COVERS("try_eliminate_iv_counter");
  UT_COVERS("iv_strength_reduction_core");
  UT_COVERS("try_rotate_loop");
  UT_COVERS("insert_instr_at");
  UT_RUN(test_s2u_signed_mappings);
  UT_RUN(test_s2u_eq_ne_unchanged);
  UT_RUN(test_s2u_already_unsigned_passthrough);
  UT_RUN(test_s2u_unknown_token_returns_minus_one);
  UT_RUN(test_trip_count_invalid_step);
  UT_RUN(test_trip_count_ge_divisible);
  UT_RUN(test_trip_count_ge_nondivisible_rounds_up);
  UT_RUN(test_trip_count_ge_range_zero_and_negative);
  UT_RUN(test_trip_count_gt_divisible_and_nondivisible);
  UT_RUN(test_trip_count_gt_range_zero_and_negative);
  UT_RUN(test_trip_count_ne_exact);
  UT_RUN(test_trip_count_ne_zero_range_exits_immediately);
  UT_RUN(test_trip_count_ne_negative_range_infinite);
  UT_RUN(test_trip_count_ne_not_divisible_infinite);
  UT_RUN(test_trip_count_unsupported_cond);
  UT_RUN(test_trip_count_huge_range_overflows_int_to_minus_one);
  UT_RUN(test_find_iv_basic_counting_loop);
  UT_RUN(test_find_iv_multiple_defs_not_iv);
  UT_RUN(test_find_iv_no_init_in_preheader_not_iv);
  UT_RUN(test_find_iv_non_var_dest_not_iv);
  UT_RUN(test_find_iv_copy_through_allowed);
  UT_RUN(test_find_exit_top_tested);
  UT_RUN(test_find_exit_bottom_tested_rotated);
  UT_RUN(test_find_exit_cmp_not_on_iv_not_found);
  UT_RUN(test_find_exit_no_jumpif_after_cmp_not_found);
  UT_RUN(test_loop_iv_and_exit_yield_trip_count);
  UT_RUN(test_unroll_register_body_three_iters);
  UT_RUN(test_unroll_store_body_blocks_unroll);
  UT_RUN(test_unroll_store_body_used_after_blocks_unroll);
  UT_RUN(test_unroll_store_body_single_iter_blocks_unroll);
  UT_RUN(test_unroll_trip_over_max_skips);
  UT_RUN(test_unroll_trip_zero_skips);
  UT_RUN(test_unroll_no_unroll_flag_skips);
  UT_RUN(test_unroll_body_with_call_skips);
  UT_RUN(test_unroll_no_iv_skips);
  UT_RUN(test_unroll_body_internal_jumpif_skips);
  UT_RUN(test_loop_size_cmp_orders_ascending_by_span);
  UT_RUN(test_transform_derived_iv_skips_memory_feeding_div);
  UT_RUN(test_transform_derived_iv_reduces_register_only_div);
  UT_RUN(test_transform_derived_iv_shared_path_refused);
  UT_RUN(test_insert_instr_at_shifts_and_retargets_jumps);
  UT_RUN(test_eliminate_loop_pure_counter_and_accumulator);
  UT_RUN(test_eliminate_loop_unused_iv_gets_no_final_assign);
  UT_RUN(test_eliminate_loop_side_effect_body_blocked);
  UT_RUN(test_eliminate_loop_no_iv_gives_up);
  UT_RUN(test_eliminate_loop_zero_trip_gives_up);
  UT_RUN(test_find_derived_ivs_shl_add_pattern);
  UT_RUN(test_find_derived_ivs_mul_variant_and_operand_order);
  UT_RUN(test_find_derived_ivs_shl_multi_use_not_nopable_skipped);
  UT_RUN(test_find_derived_ivs_dead_add_result_skipped);
  UT_RUN(test_find_derived_ivs_mla_fused_pattern);
  UT_RUN(test_find_derived_ivs_mla_base_redefined_in_loop_skipped);
  UT_RUN(test_find_derived_ivs_indexed_load_eliminable_pattern);
  UT_RUN(test_find_derived_ivs_indexed_load_not_eliminable_skipped);
  UT_RUN(test_find_exit_op_immediate_limit_matches_int_version);
  UT_RUN(test_find_exit_op_symbolic_vreg_limit);
  UT_RUN(test_find_exit_op_lval_limit_rejected);
  UT_RUN(test_eliminate_iv_counter_pretest_only_rewrites_to_ptr_cmp);
  UT_RUN(test_eliminate_iv_counter_other_use_blocks_elimination);
  UT_RUN(test_eliminate_iv_counter_no_cmp_found_declines);
  UT_RUN(test_iv_sr_core_skips_loop_without_preheader);
  UT_RUN(test_iv_sr_core_no_ivs_yields_zero_changes);
  UT_RUN(test_iv_sr_core_ivs_but_no_divs_yields_zero_changes);
  UT_RUN(test_iv_sr_core_memory_feeding_div_yields_zero);
  UT_RUN(test_rotate_loop_basic_top_tested_becomes_bottom_tested);
  UT_RUN(test_rotate_loop_missing_header_jump_declines);
  UT_RUN(test_rotate_loop_no_backedge_declines);
  UT_RUN(test_rotate_loop_call_in_body_declines);
  UT_RUN(test_rotate_loop_uninvertible_cond_declines);
}
