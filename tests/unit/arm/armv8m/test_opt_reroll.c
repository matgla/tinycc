/*
 *  test_opt_reroll.c - suite for ir/opt_reroll.c (identical-block loop re-rolling)
 *
 *  tcc_ir_opt_reroll() scans the linear instruction stream for runs of N
 *  consecutive structurally-identical IR blocks (period P, REROLL_MIN_PERIOD=3 <=
 *  P <= REROLL_MAX_PERIOD=32, repeated N >= REROLL_MIN_REPEATS=4) where the
 *  vregs DEFINED inside the body may be consistently renamed across iterations.
 *  When it finds such a run AND the run is "safe" (either no vreg defined in the
 *  run is read outside it, OR the per-iteration rename is the identity for every
 *  internal vreg), it re-rolls the run into a counted loop:
 *
 *      counter = 0            (ASSIGN, inserted at base)
 *      body[0..P)            (canonical body, iter 0, kept in place)
 *      NOP x (N-1)*P         (iterations 1..N-1 blanked in place)
 *      counter = counter + 1  (ADD)
 *      CMP counter, N         (CMP, no dest)
 *      JUMPIF (TOK_LT) -> body_start
 *
 *  It returns the number of runs re-rolled (0 = no change).
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.  The
 *  rewrite calls insert_instr_at()/tcc_ir_get_vreg_var(), which grow the pools
 *  and the variable live-interval table via the capacity/size bookkeeping
 *  fields, so utb_reroll_new() sets those to the real allocated sizes.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h / opt_reroll.h; forward-declared to
 * avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_reroll(TCCIRState *ir);

/* ssa:reroll regalloc-time driver (declared in ir/opt/ssa_opt.h). */
int ssa_opt_reroll(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Pass constants (mirrored from ir/opt_reroll.c — kept in sync here so the
 * oracle assertions are independent of the production #defines). */
#define UT_REROLL_MIN_PERIOD 3
#define UT_REROLL_MIN_REPEATS 4

/* utb_new() leaves the capacity/size bookkeeping at 0 (it pre-fills the buffers
 * but not the size fields).  The reroll rewrite inserts 4 instructions
 * (insert_instr_at, which grows via compact_instructions_size /
 * iroperand_pool_capacity) and allocates a fresh counter vreg
 * (tcc_ir_get_vreg_var, which grows the variables_live_intervals table via
 * variables_live_intervals_size).  Point all three at the real allocated sizes
 * so the existing UTB_MAX_* buffers are used in place — our sequences are tiny,
 * well under the limits, so no reallocation is triggered. */
static TCCIRState *utb_reroll_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->variables_live_intervals_size = UTB_MAX_INSTR;
  ir->next_local_variable = 0;
  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * ir->variables_live_intervals_size);
  return ir;
}

/* Count NOP instructions in [0, next_instruction_index). */
static int count_nops(TCCIRState *ir)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      n++;
  return n;
}

/* Find the first instruction with the given opcode at or after `from`. */
static int find_op(TCCIRState *ir, TccIrOp op, int from)
{
  for (int i = from; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == op)
      return i;
  return -1;
}

/* Emit a P=3 canonical body that uses ENTIRELY FRESH temps (base index `t`):
 *   T(t+0) = ASSIGN imm                  (anchor)
 *   T(t+1) = ADD    T(t+0), #1
 *   T(t+2) = MUL    T(t+1), #3
 * Because no temp defined in the body is referenced outside the run, the run
 * is safe to reroll via run_safe_no_external_use.  The cross-iteration rename
 * is T(t)->T(t+3)->... (NOT the identity), exercising the renaming path. */
static void emit_fresh_body3(TCCIRState *ir, int t, int anchor_imm)
{
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(t + 0, I32), utb_imm(anchor_imm, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(t + 1, I32), utb_temp(t + 0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(t + 2, I32), utb_temp(t + 1, I32), utb_imm(3, I32));
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: 4 identical fresh-temp blocks (P=3, N=4) re-roll into one counted
 * loop.  Assert the high-level shape: 1 reroll, 9 NOPs (the 3 iterations after
 * the first, 3 instrs each), and the inserted ASSIGN/ADD/CMP/JUMPIF tail. */
UT_TEST(test_reroll_basic_run_rerolls)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 5); /* anchor const identical across iters */

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 1);

  /* Inserted counter = 0 at index 0 (ASSIGN of an immediate). */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  /* Canonical body now at [1, 4): ASSIGN, ADD, MUL. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_MUL);
  /* Iterations 1..3 (9 instrs) blanked to NOP. */
  UT_ASSERT_EQ(count_nops(ir), 9);
  /* Loop tail: ADD (counter++), CMP, JUMPIF. */
  int add_i = find_op(ir, TCCIR_OP_ADD, 4);
  int cmp_i = find_op(ir, TCCIR_OP_CMP, 0);
  int jmp_i = find_op(ir, TCCIR_OP_JUMPIF, 0);
  UT_ASSERT(add_i > 3);
  UT_ASSERT_EQ(cmp_i, add_i + 1);
  UT_ASSERT_EQ(jmp_i, add_i + 2);

  utb_free(ir);
  return 0;
}

/* POSITIVE oracle: verify the precise operands the rewrite emits.
 *
 * For base=0, P=3, N=4 the layout is deterministic:
 *   [0]  ASSIGN counter, #0
 *   [1]  body[0]   (is_jump_target == 1)
 *   [2..3] body[1..2]
 *   [4..12] 9 NOPs
 *   [13] ADD counter, counter, #1
 *   [14] CMP counter, #4
 *   [15] JUMPIF (#TOK_LT) -> 1     (no_unroll == 1)
 * The counter is a fresh VAR vreg (position 0 in this fresh state). */
UT_TEST(test_reroll_emits_exact_loop_structure)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 7);

  int changes = tcc_ir_opt_reroll(ir);
  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 16);

  int counter_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0);

  /* [0] counter = 0 */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 0)), counter_vreg);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 0)), 0);

  /* body start marked as a branch target */
  UT_ASSERT_EQ(ir->compact_instructions[1].is_jump_target, 1);

  /* [13] counter = counter + 1 */
  UT_ASSERT_EQ(utb_op(ir, 13), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, 13)), counter_vreg);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 13)), counter_vreg);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 13)), 1);

  /* [14] CMP counter, N(=4) — CMP has no dest, operands are src1/src2 */
  UT_ASSERT_EQ(utb_op(ir, 14), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, 14)), counter_vreg);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, 14)), 4);

  /* [15] JUMPIF (TOK_LT) -> body_start(=1) */
  UT_ASSERT_EQ(utb_op(ir, 15), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 15)), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 15)), TOK_LT);
  UT_ASSERT_EQ(ir->compact_instructions[15].no_unroll, 1);

  utb_free(ir);
  return 0;
}

/* POSITIVE boundary: exactly REROLL_MIN_REPEATS(=4) repeats re-rolls.  The CMP
 * limit and the NOP count must reflect N=4. */
UT_TEST(test_reroll_min_repeats_boundary_rerolls)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < UT_REROLL_MIN_REPEATS; k++)
    emit_fresh_body3(ir, k * 3, 2);

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(count_nops(ir), (UT_REROLL_MIN_REPEATS - 1) * UT_REROLL_MIN_PERIOD);
  int cmp_i = find_op(ir, TCCIR_OP_CMP, 0);
  UT_ASSERT(cmp_i >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, cmp_i)), UT_REROLL_MIN_REPEATS);

  utb_free(ir);
  return 0;
}

/* POSITIVE longer run: N=6 repeats of the P=3 body re-rolls; CMP limit == 6,
 * and (N-1)*P == 15 NOPs are introduced. */
UT_TEST(test_reroll_longer_run_uses_all_repeats)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 6; k++)
    emit_fresh_body3(ir, k * 3, 9);

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(count_nops(ir), 5 * 3);
  int cmp_i = find_op(ir, TCCIR_OP_CMP, 0);
  UT_ASSERT(cmp_i >= 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, cmp_i)), 6);

  utb_free(ir);
  return 0;
}

/* POSITIVE self-feedback / identity rename: every iteration READS and WRITES the
 * SAME var vreg V0 (an accumulator), with NO fresh per-iteration vregs.  The
 * result V0 is consumed AFTER the run (a RETURNVALUE), so no-external-use is
 * FALSE.  But each iteration is byte-identical, so the cross-iteration rename
 * binds V0->V0 (the identity for every internal vreg) and
 * run_has_identity_rename makes the reroll safe even with the external use.
 *
 *   body_k:  V0 = ADD V0, #1
 *            V0 = MUL V0, #2
 *            V0 = ADD V0, #3
 * (If any iteration used a fresh temp, the rename T(k)->T(k+..) would NOT be the
 *  identity, and run_has_identity_rename would correctly refuse — see the
 *  external-use negative test.) */
UT_TEST(test_reroll_identity_rename_self_feedback_rerolls)
{
  TCCIRState *ir = utb_reroll_new();

  /* Seed V0 before the run. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);

  for (int k = 0; k < 4; k++)
  {
    utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(1, I32));
    utb_emit(ir, TCCIR_OP_MUL, utb_var(0, I32), utb_var(0, I32), utb_imm(2, I32));
    utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_var(0, I32), utb_imm(3, I32));
  }

  /* V0 observed after the run. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 1);
  /* Confirm a CMP/JUMPIF tail was produced and iterations 1..3 collapsed to
   * NOPs (3 instrs each). */
  UT_ASSERT(find_op(ir, TCCIR_OP_CMP, 0) >= 0);
  UT_ASSERT(find_op(ir, TCCIR_OP_JUMPIF, 0) >= 0);
  UT_ASSERT_EQ(count_nops(ir), 3 * 3);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: only 3 identical blocks (< REROLL_MIN_REPEATS=4) -> no reroll. */
UT_TEST(test_reroll_three_repeats_no_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 3; k++)
    emit_fresh_body3(ir, k * 3, 5);
  /* Pad to clear the early-out (next_instruction_index >= 12) with distinct,
   * non-rerollable tail instructions. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(20, I32), utb_imm(11, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(21, I32), utb_imm(12, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(22, I32), utb_imm(13, I32), UTB_NONE);

  int before = ir->next_instruction_index;
  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, before);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: fewer than REROLL_MIN_PERIOD*REROLL_MIN_REPEATS(=12) instructions ->
 * the driver early-returns 0 without touching anything. */
UT_TEST(test_reroll_too_few_instructions_no_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  /* 3 identical 3-instr blocks = 9 instrs < 12. */
  for (int k = 0; k < 3; k++)
    emit_fresh_body3(ir, k * 3, 5);

  int before = ir->next_instruction_index;
  UT_ASSERT(before < UT_REROLL_MIN_PERIOD * UT_REROLL_MIN_REPEATS);

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, before);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: four distinct blocks (different anchor constants per iteration) do
 * NOT match structurally (the IMM32 anchor differs), so nothing re-rolls. */
UT_TEST(test_reroll_distinct_blocks_no_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 100 + k); /* anchor immediate differs each iter */

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE external use, non-identity rename: each iteration defines fresh temps
 * (so the rename is NOT the identity), but one of those internal temps is read
 * OUTSIDE the run.  run_has_identity_rename is false AND
 * run_safe_no_external_use is false -> the run must NOT be re-rolled. */
UT_TEST(test_reroll_external_use_blocks_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 5);

  /* Read iteration-0's T2 (a temp defined inside the run) after the run.
   * This is an external use of a run-internal vreg with a non-identity rename,
   * so the reroll is unsafe. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE unsafe op in body: a JUMP inside the candidate body is on the
 * op_is_unsafe_for_reroll list, so body_is_safe rejects every period that
 * includes it -> no reroll. */
UT_TEST(test_reroll_unsafe_op_in_body_no_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  /* Each 3-instr block contains a JUMP (unsafe).  Repeating it 4x would
   * otherwise look like a rerollable run, but the unsafe op blocks it. */
  for (int k = 0; k < 4; k++)
  {
    utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(k * 2 + 0, I32), utb_imm(5, I32), UTB_NONE);
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(k * 2 + 1, I32), utb_temp(k * 2 + 0, I32), utb_imm(1, I32));
    /* JUMP to the next block's first instruction (forward, in-range). */
    utb_emit(ir, TCCIR_OP_JUMP, utb_imm((k + 1) * 3, I32), UTB_NONE, UTB_NONE);
  }

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE internal jump target: a body whose instruction at offset > 0 is
 * marked is_jump_target is rejected (body_is_safe forbids internal branch
 * targets after instruction 0), so the run does not re-roll. */
UT_TEST(test_reroll_internal_jump_target_blocks_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 5);

  /* Mark the middle instruction of each block as a branch target. */
  for (int k = 0; k < 4; k++)
    ir->compact_instructions[k * 3 + 1].is_jump_target = 1;

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE period too short / mixed: a run whose only repeating unit is 2
 * instructions long cannot reroll (REROLL_MIN_PERIOD=3).  Build alternating
 * 2-instr pairs; no period in [3,32] yields >=4 matching repeats. */
UT_TEST(test_reroll_period_two_no_reroll)
{
  TCCIRState *ir = utb_reroll_new();

  /* 14 instrs: T(2k)=ASSIGN imm; T(2k+1)=ADD T(2k),#1, repeated 7 times.
   * The genuine period is 2 (below REROLL_MIN_PERIOD=3).  Reaching
   * REROLL_MIN_REPEATS=4 repeats needs (reps+1)*P <= 14 with reps>=4, i.e.
   * 5*P <= 14 -> P <= 2 — impossible for any period >= 3.  Larger periods that
   * do line up (e.g. P=4: A,D,A,D blocks) only manage 3 repeats here, short of
   * the 4-repeat minimum, so nothing re-rolls. */
  for (int k = 0; k < 7; k++)
  {
    utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(k * 2 + 0, I32), utb_imm(5, I32), UTB_NONE);
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(k * 2 + 1, I32), utb_temp(k * 2 + 0, I32), utb_imm(1, I32));
  }

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(count_nops(ir), 0);

  utb_free(ir);
  return 0;
}

/* IDEMPOTENCE: after one reroll the run is replaced by a counted loop whose
 * body now starts at a jump target and is followed by a JUMPIF back-edge
 * (an unsafe op).  A second application therefore finds nothing to re-roll. */
UT_TEST(test_reroll_idempotent)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 5);

  int first = tcc_ir_opt_reroll(ir);
  UT_ASSERT_EQ(first, 1);

  int second = tcc_ir_opt_reroll(ir);
  UT_ASSERT_EQ(second, 0);

  utb_free(ir);
  return 0;
}

/* DEGENERATE: empty IR (no instructions) is a clean no-op returning 0. */
UT_TEST(test_reroll_empty_ir_no_change)
{
  TCCIRState *ir = utb_reroll_new();

  int changes = tcc_ir_opt_reroll(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 0);

  utb_free(ir);
  return 0;
}

/* SSA DRIVER: ssa_opt_reroll() is the thin regalloc-time wrapper over the
 * engine (docs/plan_legacy_loop_reroll_ssa.md).  It must produce the identical
 * rewrite and count as the direct engine call, and be idempotent. */
UT_TEST(test_ssa_reroll_driver_rerolls_and_is_idempotent)
{
  TCCIRState *ir = utb_reroll_new();

  for (int k = 0; k < 4; k++)
    emit_fresh_body3(ir, k * 3, 5);

  int changes = ssa_opt_reroll(ir);
  UT_ASSERT_EQ(changes, 1);

  /* Same rewritten shape as the direct-engine test: counter=0 at [0], body at
   * [1,4), 9 NOPs, ADD/CMP/JUMPIF tail. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(count_nops(ir), 9);
  UT_ASSERT(find_op(ir, TCCIR_OP_CMP, 0) >= 0);
  UT_ASSERT(find_op(ir, TCCIR_OP_JUMPIF, 0) >= 0);

  /* Idempotent: the rerolled loop's JUMPIF back-edge is unsafe, so a second
   * run finds nothing. */
  UT_ASSERT_EQ(ssa_opt_reroll(ir), 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("reroll");
