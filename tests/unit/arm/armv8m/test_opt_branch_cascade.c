/*
 *  test_opt_branch_cascade.c - suite for the Phase 2 branch/const-prop
 *  cascade family (docs/plan_ut_next_steps.md Phase 2): or_bool, setif_fuse,
 *  stack_bool, stack_nonnull (ir/opt_branch.c) and var_tmp_fwd
 *  (ir/opt_promote.c).
 *
 *  NOT covered here (documented gap, same class as "esp_cleanup" in
 *  test_opt_store_fwd.c): branch_fold_2x, const_cascade, kb_cascade, and
 *  branch_cleanup are all `static` compound-orchestration wrappers in
 *  ir/opt_pipeline.c that just re-run already-tested passes
 *  (tcc_ir_opt_branch_folding, tcc_ir_opt_known_bits, tcc_ir_opt_const_prop,
 *  tcc_ir_opt_jump_threading, etc.) to a fixpoint with no independent
 *  transformation logic of their own. Not reachable from a host-native unit
 *  TU (internal linkage); need the golden-IR (`-dump-ir-passes=`) track.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (defined in ir/opt_branch.c and ir/opt_promote.c;
 * forward-declared here to avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_setif_branch_fuse(TCCIRState *ir);
int tcc_ir_opt_stack_bool_diamond(TCCIRState *ir);
int ssa_opt_or_bool_diamond(TCCIRState *ir);
int tcc_ir_opt_var_tmp_fwd(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define TOK_EQ 0x94 /* == */
#define TOK_NE 0x95 /* != */

/* ------------------------------------------------------------------ helpers */

/* The address of a stack slot (what a LEA computes into a temp): not an
 * lvalue, just a value. */
static IROperand utb_slot_addr(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

/* A direct stack-slot value reference (STORE dest / TEST_ZERO src / OR src):
 * the anonymous compiler-temp slot itself, not addressed through a vreg. */
static IROperand utb_slot_lval(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* or_bool_diamond grows the operand pool (tcc_ir_pool_ensure) for the new OR
 * instruction's operands. utb_new() pre-fills iroperand_pool but leaves
 * iroperand_pool_capacity at 0; tcc_ir_pool_ensure's growth loop
 * (`while (capacity < needed) capacity *= 2;`) never advances from 0, hanging
 * forever. Set the real allocated capacity so growth works (see
 * test_opt_licm.c's utb_loop_new() for the same pattern). */
static TCCIRState *utb_pool_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  return ir;
}

/* ================================================================== setif_fuse */

/* POSITIVE: CMP + SETIF(EQ) + TEST_ZERO + JUMPIF(NE), where the SETIF result
 * is used only by the TEST_ZERO, fuses into CMP + JUMPIF(EQ) directly on the
 * original CMP's flags (jump_tok==NE keeps setif_tok as-is). */
UT_TEST(test_setif_fuse_chain_collapses_to_direct_branch)
{
  TCCIRState *ir = utb_new();

  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(2, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  IROperand new_cond = utb_src1(ir, jumpif);
  UT_ASSERT(irop_is_immediate(new_cond));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_cond), TOK_EQ);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the SETIF result is read a second time (by RETURNVALUE),
 * so it is not single-use -- the chain must not fuse. */
UT_TEST(test_setif_fuse_multi_use_setif_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(2, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE); /* 2nd use of T2 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_SETIF);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ============================================================ branch_fold_test_zero */

/* POSITIVE: TEST_ZERO #0 + JUMPIF EQ -- the constant is zero so an EQ branch is
 * always taken: TEST_ZERO becomes NOP and the JUMPIF becomes an unconditional
 * JUMP that keeps the original target. */
UT_TEST(test_branch_fold_test_zero_taken_becomes_jump)
{
  TCCIRState *ir = utb_new();

  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(0, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, jumpif)), 3);

  utb_free(ir);
  return 0;
}

/* POSITIVE: TEST_ZERO #7 + JUMPIF EQ -- the constant is non-zero so the EQ
 * branch is never taken: both instructions become NOP (control falls through). */
UT_TEST(test_branch_fold_test_zero_not_taken_becomes_nop)
{
  TCCIRState *ir = utb_new();

  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(7, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the tested value is a runtime temp, not an immediate --
 * nothing is known about the flag, so the pair must survive untouched. */
UT_TEST(test_branch_fold_test_zero_non_immediate_kept)
{
  TCCIRState *ir = utb_new();

  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a not-taken TEST_ZERO whose flags are also consumed by a following
 * SETIF. After NOPing the dead branch, the SETIF (which would read stale flags)
 * is folded to the known constant: TEST_ZERO #7 + JUMPIF EQ (not taken) leaves a
 * SETIF NE which resolves to (7 != 0) == 1, an ASSIGN #1. */
UT_TEST(test_branch_fold_test_zero_folds_trailing_setif)
{
  TCCIRState *ir = utb_new();

  int test_zero = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(7, I32), UTB_NONE);
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_branch_fuse(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, test_zero), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_ASSIGN);
  IROperand imm = utb_src1(ir, setif);
  UT_ASSERT(irop_is_immediate(imm));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, imm), 1);

  utb_free(ir);
  return 0;
}

/* ================================================================== stack_bool */

/* POSITIVE: the classic inlined-bool diamond --
 *   0: StackLoc[-8] <- #5           (q_a, true arm)
 *   1: JUMP -> 3                     (q_b)
 *   2: StackLoc[-8] <- #0           (q_c, false arm / fallthrough)
 *   3: TEST_ZERO StackLoc[-8]        (merge)
 *   4: JUMPIF NE -> 6                (T=6, target_next=5)
 *   5: RETURNVALUE #1                (target_next)
 *   6: RETURNVALUE #2                (T)
 * collapses to direct jumps from each arm, NOPing the slot scaffolding. */
UT_TEST(test_stack_bool_diamond_collapses_to_direct_jumps)
{
  TCCIRState *ir = utb_new();

  int q_a = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(5, I32), UTB_NONE);
  int q_b = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  int q_c = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(0, I32), UTB_NONE);
  int merge = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_slot_lval(-8, I32), UTB_NONE);
  int q_e = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_stack_bool_diamond(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, q_a), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, q_c), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, merge), TCCIR_OP_NOP);
  /* val_a=5 (!=0) + NE -> a_jumps=true -> q_b becomes JUMP to T(6). */
  UT_ASSERT_EQ(utb_op(ir, q_b), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, q_b)), 6);
  /* val_b=0 (==0) + NE -> b_jumps=false -> q_e becomes JUMP to target_next(5). */
  UT_ASSERT_EQ(utb_op(ir, q_e), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, q_e)), 5);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): q_c stores a non-immediate value -- the diamond shape is
 * not provable, so nothing is touched. */
UT_TEST(test_stack_bool_diamond_non_immediate_store_kept)
{
  TCCIRState *ir = utb_new();

  int q_a = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(5, I32), UTB_NONE);
  int q_b = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  int q_c = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_temp(0, I32), UTB_NONE);
  int merge = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_slot_lval(-8, I32), UTB_NONE);
  int q_e = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_stack_bool_diamond(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, q_a), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, q_b), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, q_c), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, merge), TCCIR_OP_TEST_ZERO);
  UT_ASSERT_EQ(utb_op(ir, q_e), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ================================================================== or_bool */

/* POSITIVE: the `acc |= (cond ? 1 : 0)` diamond --
 *   0: JUMPIF EQ -> 3               (i_jmpif; skip true arm)
 *   1: StackLoc[-8] <- #1           (i_st_t, true arm)
 *   2: JUMP -> 4                     (i_jmp; skip false arm)
 *   3: StackLoc[-8] <- #0           (i_st_f, false arm)
 *   4: T2 <- T1 OR StackLoc[-8]      (i_or; merge)
 *   5: RETURNVALUE T2
 * collapses to each arm computing the OR result directly. */
UT_TEST(test_or_bool_diamond_collapses_to_direct_or)
{
  TCCIRState *ir = utb_pool_new();

  int i_jmpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int i_st_t = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(1, I32), UTB_NONE);
  int i_jmp = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  int i_st_f = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(0, I32), UTB_NONE);
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32), utb_temp(1, I32), utb_slot_lval(-8, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = ssa_opt_or_bool_diamond(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_jmpif), TCCIR_OP_JUMPIF); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, i_jmp), TCCIR_OP_JUMP);      /* untouched */
  UT_ASSERT_EQ(utb_op(ir, i_st_t), TCCIR_OP_OR);       /* dst = src OR #1 */
  UT_ASSERT_EQ(utb_op(ir, i_st_f), TCCIR_OP_ASSIGN);   /* dst = src */
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the slot is read a second time elsewhere (an extra
 * reference besides i_st_t/i_st_f/i_or) -- the pass must leave it alone. */
UT_TEST(test_or_bool_diamond_extra_slot_use_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int i_st_t = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  int i_st_f = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(0, I32), UTB_NONE);
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I32), utb_temp(1, I32), utb_slot_lval(-8, I32));
  int extra = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_slot_lval(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = ssa_opt_or_bool_diamond(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_st_t), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, i_st_f), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_op(ir, extra), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* ================================================================== var_tmp_fwd */

/* POSITIVE: `STORE V <- T` where T is defined immediately before it forwards
 * T into later reads of V within the same block. */
UT_TEST(test_var_tmp_fwd_forwards_adjacent_temp)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  IROperand src1 = utb_src1(ir, use);
  UT_ASSERT_EQ(utb_vreg_pos(src1), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): an intervening instruction sits between T's def and the
 * STORE, so T is not adjacent -- no forwarding. */
UT_TEST(test_var_tmp_fwd_nonadjacent_temp_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_imm(9, I32), utb_imm(9, I32)); /* intervening */
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand src1 = utb_src1(ir, use);
  int32_t vr = irop_get_vreg(src1);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr), TCCIR_VREG_TYPE_VAR);

  utb_free(ir);
  return 0;
}

/* POSITIVE: V appearing in a src2 read position is forwarded too. */
UT_TEST(test_var_tmp_fwd_forwards_src2)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(5, I32), utb_var(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  IROperand src2 = utb_src2(ir, use);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src2)), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(utb_vreg_pos(src2), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (DEREF guard): the STORE source is an lval TEMP (a memory deref),
 * not a register-held value -- forwarding would duplicate the read. */
UT_TEST(test_var_tmp_fwd_lval_temp_src_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(utb_src1(ir, use))), TCCIR_VREG_TYPE_VAR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (address guard): T is defined by a LEA, so it holds a computed
 * address; forwarding it would break downstream stack-slot liveness analyses. */
UT_TEST(test_var_tmp_fwd_lea_source_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(utb_src1(ir, use))), TCCIR_VREG_TYPE_VAR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (BB boundary): a back-edge JUMP targets the use, so V may hold a
 * different value on re-entry -- forwarding must stop at the jump target. */
UT_TEST(test_var_tmp_fwd_stops_at_jump_target)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE); /* back-edge to index 2 (the use) */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(utb_src1(ir, use))), TCCIR_VREG_TYPE_VAR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (aliasing): the VAR is address-taken AND a LEA of it exists, so its
 * slot may be read through the pointer -- forwarding is unsound. */
UT_TEST(test_var_tmp_fwd_addrtaken_with_lea_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 2);
  ir->variables_live_intervals[1].addrtaken = 1;

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(9, I32), utb_var(1, I32), UTB_NONE); /* &V1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(1, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(utb_src1(ir, use))), TCCIR_VREG_TYPE_VAR);

  utb_free(ir);
  return 0;
}

/* POSITIVE (stale annotation): address-taken is set but there is no LEA / chain
 * aliasing path, so the annotation is stale and forwarding is still safe. */
UT_TEST(test_var_tmp_fwd_addrtaken_no_lea_forwards)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);
  ir->variables_live_intervals[0].addrtaken = 1;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_temp(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_var_tmp_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, use)), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(utb_src1(ir, use))), TCCIR_VREG_TYPE_TEMP);

  utb_free(ir);
  return 0;
}

UT_COVERS("setif_fuse");
UT_COVERS("branch_fold_test_zero");
UT_COVERS("stack_bool");
UT_COVERS("ssa:or_bool_diamond");
UT_COVERS("var_tmp_fwd");
