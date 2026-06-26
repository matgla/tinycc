/*
 *  test_opt_jump_thread.c - suite for ir/opt_jump_thread.c (jump threading +
 *  fall-through elimination)
 *
 *  Two entry points are exercised:
 *
 *  tcc_ir_opt_jump_threading():  for each JUMP/JUMPIF, follows a chain of
 *    unconditional JUMPs (and skips NOPs) starting at the jump's target, then
 *    rewrites the jump's target operand to the ultimate destination.  The target
 *    is stored in the DEST operand's 32-bit immediate (read back via
 *    utb_dest(ir,i).u.imm32).  Guard: a conditional JUMPIF must NOT be retargeted
 *    BACKWARD (new_target < target) — that would land its taken edge inside an
 *    enclosing loop body and let downstream cleanup collapse a live loop-exit.
 *
 *  tcc_ir_opt_eliminate_fallthrough():  rewrites to NOP any JUMP/JUMPIF whose
 *    target equals the next real (non-NOP) instruction — a no-op control
 *    transfer.  For a plain JUMP this is unconditional.  For a JUMPIF additional
 *    safety checks gate the removal (epilogue / JUMP|RETURN|TRAP successor / no
 *    impure CALL earlier in the basic block).
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *
 *  Target encoding note: utb_imm(target_idx, I32) builds an IMM32 operand whose
 *  .u.imm32 == target_idx; the pass reads it via irop_get_imm64_ex() (which
 *  returns op.u.imm32 for IMM32) and writes the new target back into .u.imm32.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (defined in ir/opt_jump_thread.c; forward-declared to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_jump_threading(TCCIRState *ir);
int tcc_ir_opt_eliminate_fallthrough(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Emit a JUMP whose target index is `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* Emit a JUMPIF (conditional) with target index `tgt` and a temp condition. */
static int emit_jumpif(TCCIRState *ir, int tgt, int cond_temp)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_temp(cond_temp, I32), UTB_NONE);
}

/* Read a jump's current target index. */
static int jump_target(TCCIRState *ir, int i)
{
  return (int)utb_dest(ir, i).u.imm32;
}

/* ------------------------------------------------------- jump_threading tests */

/* JUMP -> JUMP chain collapse (POSITIVE):
 *   0: JUMP -> 1
 *   1: JUMP -> 2
 *   2: ADD          (real instruction = final target)
 * Following the unconditional-jump chain from target 1 reaches the real ADD at
 * index 2, so jump 0's target must be rewritten 1 -> 2.  Would FAIL (stay 1) if
 * the chain were not followed. */
UT_TEST(test_jt_chain_collapses_to_final_target)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP); /* still a JUMP, just retargeted */
  UT_ASSERT_EQ(jump_target(ir, j0), 2);        /* threaded past the middle JUMP */

  utb_free(ir);
  return 0;
}

/* JUMP through NOPs (POSITIVE):
 *   0: JUMP -> 1
 *   1: NOP
 *   2: NOP
 *   3: ADD
 * follow_jump_chain skips the NOPs at the target and find_first_non_nop lands on
 * the ADD at 3, so jump 0's target is rewritten 1 -> 3. */
UT_TEST(test_jt_skips_nops_to_real_instruction)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(jump_target(ir, j0), 3);

  utb_free(ir);
  return 0;
}

/* JUMP already pointing at a real instruction (NEGATIVE / no-op):
 *   0: JUMP -> 1
 *   1: ADD
 * Target 1 is already a non-jump, non-NOP instruction, so nothing to thread. */
UT_TEST(test_jt_direct_target_no_change)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(jump_target(ir, j0), 1);

  utb_free(ir);
  return 0;
}

/* Conditional backward-threading guard (NEGATIVE for JUMPIF):
 *   0: ADD          (real instruction, would be the chased target)
 *   1: JUMP -> 0    (unconditional back-edge to 0)
 *   2: JUMPIF -> 1  (conditional; chasing the chain at 1 would reach 0, BACKWARD)
 * follow_jump_chain(1) -> 0, which is < target(1).  Because instr 2 is a JUMPIF
 * and new_target(0) < target(1), the pass must REVERT the conditional target to
 * 1 (no backward conditional threading), leaving JUMPIF 2 unchanged.
 *
 * The unconditional JUMP at 1 is NOT subject to the guard and may be threaded
 * (target 0 is already a real instruction, so it stays 0 here). */
UT_TEST(test_jt_conditional_backward_guard)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 0);
  int jif = emit_jumpif(ir, 1, 0);

  tcc_ir_opt_jump_threading(ir);

  /* The conditional jump's target must remain 1 (guard prevents backward
   * retarget to 0). */
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------- eliminate_fallthrough */

/* Unconditional JUMP to the next instruction (POSITIVE):
 *   0: JUMP -> 1
 *   1: ADD
 * next_real after index 0 is 1, which equals the target -> the JUMP is a pure
 * no-op and must be rewritten to NOP; return >= 1. */
UT_TEST(test_ef_jump_to_next_becomes_nop)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Fall-through across NOPs (POSITIVE):
 *   0: JUMP -> 2
 *   1: NOP
 *   2: ADD
 * find_first_non_nop(1) skips the NOP and returns 2, which equals the target,
 * so the JUMP is still a no-op fall-through and is eliminated. */
UT_TEST(test_ef_jump_to_next_across_nop_becomes_nop)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Conditional JUMPIF fall-through (POSITIVE, safe path):
 *   0: JUMPIF -> 1
 *   1: ADD
 * target(1) == next_real(1).  Safety: next_real=1 < n and instr 1 is ADD (not a
 * JUMP/RETURN/TRAP), so case (a) does not fire; the backward CALL scan finds no
 * prior instructions (j starts at -1) -> safe.  The JUMPIF is eliminated to NOP. */
UT_TEST(test_ef_jumpif_to_next_safe_becomes_nop)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif(ir, 1, 0);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Real (non-fall-through) jump must be preserved (NEGATIVE):
 *   0: JUMP -> 2
 *   1: ADD          (next real after 0)
 *   2: SUB
 * next_real after 0 is 1, but the target is 2 -> not a fall-through, so the JUMP
 * is a genuine branch and must NOT be eliminated. */
UT_TEST(test_ef_real_branch_preserved)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_imm(5, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j0), 2);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------- corner-case tests */

/* Chain length N (3+ intermediate unconditional jumps):
 *   0: JUMP -> 1
 *   1: JUMP -> 2
 *   2: JUMP -> 3
 *   3: JUMP -> 4
 *   4: ADD
 * Jump 0 must thread directly to the final real instruction at index 4. */
UT_TEST(test_jt_chain_length_n)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  emit_jump(ir, 2);
  emit_jump(ir, 3);
  emit_jump(ir, 4);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j0), 4);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Cycle A -> B -> A must terminate without corrupting IR:
 *   0: JUMP -> 1
 *   1: JUMP -> 0
 *   2: JUMPIF -> 1  (external entry into the cycle)
 * follow_jump_chain(1) detects the cycle and returns 1 (the cycle head).
 * The conditional jump at 2 must therefore keep target 1. */
UT_TEST(test_jt_cycle_terminates)
{
  TCCIRState *ir = utb_new();

  emit_jump(ir, 1);
  emit_jump(ir, 0);
  int jif = emit_jumpif(ir, 1, 0);

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Jump-to-self must not loop forever and must leave the jump unchanged:
 *   0: JUMP -> 0 */
UT_TEST(test_jt_jump_to_self)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 0);

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j0), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Target at index 0 (first instruction boundary):
 *   0: ADD
 *   1: JUMP -> 0
 * Jump 1 already points to a real instruction at 0 -> no change. */
UT_TEST(test_jt_target_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int j1 = emit_jump(ir, 0);

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(jump_target(ir, j1), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Target at last instruction (n-1 boundary):
 *   0: JUMP -> 2
 *   1: NOP
 *   2: ADD
 * Jump 0 threads past the NOP to the real ADD at index 2 (n-1). */
UT_TEST(test_jt_target_last)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(jump_target(ir, j0), 2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Out-of-range target must be skipped without corrupting IR:
 *   0: JUMP -> 99
 *   1: ADD
 * The pass validates target < n and leaves the jump untouched. */
UT_TEST(test_jt_target_out_of_range)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 99);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j0), 99);
  /* Do not call utb_assert_wellformed here: the intentionally out-of-range
   * input target is correctly left untouched by the pass. */

  utb_free(ir);
  return 0;
}

/* Unconditional backward threading is allowed (only JUMPIF is guarded):
 *   0: ADD
 *   1: JUMP -> 0
 *   2: JUMP -> 1
 * Jump 2's target 1 threads back to 0 (new_target < target). Because this is
 * an unconditional JUMP, the backward guard does not apply. */
UT_TEST(test_jt_unconditional_backward_allowed)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 0);
  int j2 = emit_jump(ir, 1);

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j2), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Conditional forward threading through NOPs and jumps:
 *   0: ADD
 *   1: JUMP -> 2
 *   2: NOP
 *   3: ADD
 *   4: JUMPIF -> 1
 * Jumpif 4 target 1 threads forward to 3 (via jump 1 -> 2 -> skip NOP -> 3).
 * new_target (3) >= target (1), so the conditional backward guard does not apply. */
UT_TEST(test_jt_conditional_forward_threads)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(3, I32), utb_imm(4, I32));
  int jif = emit_jumpif(ir, 1, 0);

  int changes = tcc_ir_opt_jump_threading(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 3);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Jump threading must reach a fixpoint (second invocation reports 0 changes). */
UT_TEST(test_jt_idempotent)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_jump_threading, 10);

  UT_ASSERT(total >= 0); /* converged */
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j0), 2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Conditional JUMPIF fall-through across NOPs:
 *   0: JUMPIF -> 2
 *   1: NOP
 *   2: ADD
 * next_real after 0 skips NOP and is 2 == target -> eliminated. */
UT_TEST(test_ef_jumpif_to_next_across_nop)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif(ir, 2, 0);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Backward real branch must be preserved:
 *   0: ADD
 *   1: JUMP -> 0
 * next_real after 1 is 2 (n == epilogue), target is 0 -> not a fall-through,
 * so the backward JUMP stays. */
UT_TEST(test_ef_real_branch_backward_preserved)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int j1 = emit_jump(ir, 0);

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, j1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(jump_target(ir, j1), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Trailing JUMP to epilogue (target == n) at the last slot is a no-op:
 *   0: ADD
 *   1: JUMP -> 2
 * next_real after 1 is 2 (past end), target is 2 -> eliminated. */
UT_TEST(test_ef_jump_to_epilogue)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int j1 = emit_jump(ir, 2);

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, j1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Conditional JUMPIF whose fallthrough and taken edge both reach the same
 * target via the immediately following unconditional JUMP:
 *   0: JUMPIF -> 2
 *   1: JUMP -> 2
 *   2: ADD
 * Both arms go to 2, so the JUMPIF is eliminated. */
UT_TEST(test_ef_jumpif_both_arms_converge)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif(ir, 2, 0);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_eliminate_fallthrough(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Eliminate fall-through must converge (a chain of JUMPs collapses
 * sequentially). */
UT_TEST(test_ef_idempotent)
{
  TCCIRState *ir = utb_new();

  int j0 = emit_jump(ir, 1);
  int j1 = emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_eliminate_fallthrough, 10);

  UT_ASSERT(total >= 0); /* converged */
  UT_ASSERT_EQ(utb_op(ir, j0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_jump_thread)
{
  UT_COVERS("jump_threading");
  UT_COVERS("eliminate_fallthrough");

  /* jump_threading */
  UT_RUN(test_jt_chain_collapses_to_final_target);
  UT_RUN(test_jt_skips_nops_to_real_instruction);
  UT_RUN(test_jt_direct_target_no_change);
  UT_RUN(test_jt_conditional_backward_guard);
  UT_RUN(test_jt_chain_length_n);
  UT_RUN(test_jt_cycle_terminates);
  UT_RUN(test_jt_jump_to_self);
  UT_RUN(test_jt_target_zero);
  UT_RUN(test_jt_target_last);
  UT_RUN(test_jt_target_out_of_range);
  UT_RUN(test_jt_unconditional_backward_allowed);
  UT_RUN(test_jt_conditional_forward_threads);
  UT_RUN(test_jt_idempotent);

  /* eliminate_fallthrough */
  UT_RUN(test_ef_jump_to_next_becomes_nop);
  UT_RUN(test_ef_jump_to_next_across_nop_becomes_nop);
  UT_RUN(test_ef_jumpif_to_next_safe_becomes_nop);
  UT_RUN(test_ef_real_branch_preserved);
  UT_RUN(test_ef_jumpif_to_next_across_nop);
  UT_RUN(test_ef_real_branch_backward_preserved);
  UT_RUN(test_ef_jump_to_epilogue);
  UT_RUN(test_ef_jumpif_both_arms_converge);
  UT_RUN(test_ef_idempotent);
}
