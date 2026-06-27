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

/* Count STOREs into stackoff(100) whose src1 is an immediate; fill vals[].
 * Returns the count.  Used to verify per-iteration IV substitution. */
static int collect_store_iv_values(TCCIRState *ir, int *vals, int max)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index && n < max; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(d) != IROP_TAG_STACKOFF || (int)irop_get_imm64_ex(ir, d) != 100)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(s))
      continue;
    vals[n++] = (int)irop_get_imm64_ex(ir, s);
  }
  return n;
}

UT_TEST(test_unroll_three_iters_iv_substituted)
{
  /* for(V0=0; V0<3; V0++) STORE [100] = V0  ->  three stores with #0,#1,#2. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 3, 1, utb_var(0, I32));
  IRLoop L = utb_loop(1, 1, 5, 0);

  int ret = try_unroll_loop_ex(ir, &L, NULL, 0);

  UT_ASSERT_EQ(ret, 1);

  /* IV init, the IV increment, and the back-edge are NOP'd (not replicated).
   * The original CMP/JUMPIF/body slots at @1/@2/@3 are reused to write the
   * unrolled body copies (verified below via collect_store_iv_values). */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP); /* init  */
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP); /* iv inc */
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_NOP); /* back-edge */

  /* Three stores with IV values 0, 1, 2 (the per-iteration substitution). */
  int vals[8];
  int n = collect_store_iv_values(ir, vals, 8);
  UT_ASSERT_EQ(n, 3);
  UT_ASSERT_EQ(vals[0], 0);
  UT_ASSERT_EQ(vals[1], 1);
  UT_ASSERT_EQ(vals[2], 2);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_iv_used_after_loop_writes_final_value)
{
  /* A reader of V0 after the loop forces a final-value ASSIGN V0=#3. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 0, 3, 1, utb_var(0, I32));
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);          /* 6 (exit_target) spacer */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE); /* 7 reads V0 */
  IRLoop L = utb_loop(1, 1, 5, 0);

  int ret = try_unroll_loop_ex(ir, &L, NULL, 0);

  UT_ASSERT_EQ(ret, 1);
  /* Find the ASSIGN V0 = #3 (iv_final = 0 + 3*1). */
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
  UT_ASSERT(found_final);
  utb_free(ir);
  return 0;
}

UT_TEST(test_unroll_single_iteration)
{
  /* trip_count = 1: body copied exactly once with init value. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  emit_unrollable_loop(ir, 7, 8, 1, utb_var(0, I32)); /* init=7, limit=8 -> 1 trip */
  IRLoop L = utb_loop(1, 1, 5, 0);

  UT_ASSERT_EQ(try_unroll_loop_ex(ir, &L, NULL, 0), 1);
  int vals[8];
  UT_ASSERT_EQ(collect_store_iv_values(ir, vals, 8), 1);
  UT_ASSERT_EQ(vals[0], 7);
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

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_loop_utils)
{
  UT_COVERS("signed_to_unsigned_cond");
  UT_COVERS("compute_trip_count");
  UT_COVERS("find_induction_vars_ex");
  UT_COVERS("find_loop_exit_condition");
  UT_COVERS("try_unroll_loop_ex");
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
  UT_RUN(test_unroll_three_iters_iv_substituted);
  UT_RUN(test_unroll_iv_used_after_loop_writes_final_value);
  UT_RUN(test_unroll_single_iteration);
  UT_RUN(test_unroll_trip_over_max_skips);
  UT_RUN(test_unroll_trip_zero_skips);
  UT_RUN(test_unroll_no_unroll_flag_skips);
  UT_RUN(test_unroll_body_with_call_skips);
  UT_RUN(test_unroll_no_iv_skips);
  UT_RUN(test_unroll_body_internal_jumpif_skips);
}
