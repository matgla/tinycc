/*
 *  test_opt_jumpif_invert.c - suite for jumpif_invert.c (JUMPIF inversion over
 *  an immediately-following unconditional jump)
 *
 *  The pass inverts a JUMPIF's condition and re-targets it to the JUMP's
 *  destination, NOPing the JUMP, when:
 *    - The JUMPIF's destination equals the first non-NOP instruction after the
 *      JUMP (i.e., the JUMP falls through to the JUMPIF's target)
 *    - The JUMP is not a backward jump (when allow_backward=0)
 *    - invert_condition() returns a valid inverted token
 *    - No other jump/jumpif/switch-table entry targets the JUMP or the NOPs
 *      immediately preceding it (would silently reroute an incoming edge)
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

/* Pass entry point (defined in source/opt/flat/cfg/jumpif_invert.c; forward-
 * declared to avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_jumpif_invert(TCCIRState *ir, int allow_backward);

#define I32 IROP_BTYPE_INT32
#define TOK_EQ 0x94 /* == */
#define TOK_NE 0x95 /* != */
#define TOK_ULT 0x92 /* < (unsigned) */
#define TOK_UGE 0x93 /* >= (unsigned) */
#define TOK_ULE 0x96 /* <= (unsigned) */
#define TOK_UGT 0x97 /* > (unsigned) */
#define TOK_LT 0x9c /* < */
#define TOK_GE 0x9d /* >= */
#define TOK_LE 0x9e /* <= */
#define TOK_GT 0x9f /* > */

/* Emit a JUMP whose target index is `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* Emit a JUMPIF (conditional) with target index `tgt` and a condition token. */
static int emit_jumpif_cond(TCCIRState *ir, int tgt, int cond_token)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_imm(cond_token, I32), UTB_NONE);
}

/* Read a jump's current target index. */
static int jump_target(TCCIRState *ir, int i)
{
  return (int)utb_dest(ir, i).u.imm32;
}

/* Read a JUMPIF's current condition token. */
static int jumpif_cond(TCCIRState *ir, int i)
{
  return (int)irop_get_imm64_ex(ir, utb_src1(ir, i));
}

/* =========================================================== positive cases */

/* JUMPIF -> JUMP -> target (POSITIVE, allow_backward=1):
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: ADD
 * The JUMP falls through to the JUMPIF's target, the JUMP is forward, the
 * condition is invertible, and no other edge targets the JUMP or NOPs before it.
 * The pass must invert the JUMPIF's condition to NE, retarget it to 2, and NOP
 * the JUMP. */
UT_TEST(test_ji_forward_jump_falls_through_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> target (POSITIVE, allow_backward=0):
 *   0: JUMPIF(NE) -> 3
 *   1: JUMP -> 3
 *   2: NOP
 *   3: ADD
 * The JUMP is forward (target=3 > jump_idx=1), so allow_backward=0 does not
 * block the inversion. The JUMPIF's condition inverts to EQ and target changes
 * to 3. */
UT_TEST(test_ji_allow_backward_zero_forward_jump_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 3, TOK_NE);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 0);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 3);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_EQ);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* All condition pairs (POSITIVE):
 *   0: JUMPIF(cond) -> 2
 *   1: JUMP -> 2
 *   2: ADD
 * Each condition must invert correctly. */
UT_TEST(test_ji_all_condition_pairs_invert)
{
  struct {
    int cond;
    int expected_inv;
  } pairs[] = {
    {TOK_EQ, TOK_NE},
    {TOK_NE, TOK_EQ},
    {TOK_LT, TOK_GE},
    {TOK_GE, TOK_LT},
    {TOK_GT, TOK_LE},
    {TOK_LE, TOK_GT},
    {TOK_UGE, TOK_ULT},
    {TOK_UGT, TOK_ULE},
    {TOK_ULT, TOK_UGE},
    {TOK_ULE, TOK_UGT},
  };

  for (int p = 0; p < 10; p++)
  {
    TCCIRState *ir = utb_new();

    int jif = emit_jumpif_cond(ir, 2, pairs[p].cond);
    emit_jump(ir, 2);
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

    int changes = tcc_ir_opt_jumpif_invert(ir, 1);

    UT_ASSERT_EQ(changes, 1);
    UT_ASSERT_EQ(jumpif_cond(ir, jif), pairs[p].expected_inv);
    UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);

    utb_free(ir);
  }

  return 0;
}

/* Multiple JUMPIFs, only one qualifies (POSITIVE):
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: JUMPIF(NE) -> 4
 *   3: JUMP -> 4
 *   4: ADD
 * The first JUMPIF at 0 qualifies (JUMP at 1 falls through to 2). The second
 * JUMPIF at 2 does NOT qualify (JUMP at 3's target=4 != JUMPIF's dest=4? Wait,
 * 4==4, so it does qualify. Let me fix: JUMPIF's dest must equal the instruction
 * after the JUMP. For JUMPIF at 2, dest=4, JUMP at 3, after=4 (skip NOPs from
 * 4 -> ADD at 4 is real), 4==4 -> qualifies. So both qualify.
 *
 * Let me fix: JUMPIF's dest must NOT equal the instruction after the JUMP.
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: JUMPIF(NE) -> 3
 *   3: JUMP -> 4
 *   4: ADD
 * JUMPIF at 2, dest=3, JUMP at 3, after=4 (skip NOPs from 4 -> ADD at 4 is
 * real), 4 != 3 -> no inversion. Correct. */
UT_TEST(test_ji_multiple_jumpifs_only_one_qualifies)
{
  TCCIRState *ir = utb_new();

  int jif0 = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 2);
  int jif2 = emit_jumpif_cond(ir, 3, TOK_NE);
  emit_jump(ir, 4);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif0), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif0), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  /* Second JUMPIF unchanged */
  UT_ASSERT_EQ(utb_op(ir, jif2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif2), 3);
  UT_ASSERT_EQ(jumpif_cond(ir, jif2), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* =========================================================== negative cases */

/* JUMP does not fall through to JUMPIF's target (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 3
 *   2: NOP
 *   3: ADD
 * JUMP's target=3 != JUMPIF's dest=1 (after NOP skip, next real after JUMP is 3,
 * which != JUMPIF's dest=1). The pass must leave everything untouched. */
UT_TEST(test_ji_jump_target_does_not_match_jumpif_dest_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_EQ);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* Backward JUMP with allow_backward=0 (NEGATIVE):
 *   0: ADD
 *   1: JUMPIF(EQ) -> 2
 *   2: JUMP -> 0
 * JUMP's target=0 < JUMPIF's index=1, so allow_backward=0 blocks the inversion. */
UT_TEST(test_ji_backward_jump_blocked_when_allow_backward_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 0);

  int changes = tcc_ir_opt_jumpif_invert(ir, 0);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_EQ);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* Backward JUMP with allow_backward=1 (POSITIVE):
 *   0: ADD
 *   1: JUMPIF(EQ) -> 2
 *   2: JUMP -> 0
 * allow_backward=1 permits the inversion even though the JUMP is backward. */
UT_TEST(test_ji_backward_jump_allowed_when_allow_backward_one)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 0);

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 0);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NOPs between JUMPIF and JUMP, but JUMP's target != JUMPIF's dest (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: NOP
 *   2: JUMP -> 3
 *   3: ADD
 * After skipping NOPs, the JUMP at 2's target=3 != JUMPIF's dest=1. No inversion. */
UT_TEST(test_ji_nops_between_jumpif_and_jump_no_match_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* NOPs between JUMPIF and JUMP, JUMP falls through (POSITIVE):
 *   0: JUMPIF(EQ) -> 4
 *   1: NOP
 *   2: NOP
 *   3: JUMP -> 4
 *   4: ADD
 * After skipping NOPs, the JUMP at 3's target=4 == next_real after JUMP (skip
 * NOPs from 4 -> 4 is ADD, real), so after=4. The check is `after != a`
 * where a = JUMPIF's dest = 4. So 4 == 4 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_nops_between_jumpif_and_jump_falls_through_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 4, TOK_EQ);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  emit_jump(ir, 4);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 4);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Other jump targets the JUMP (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: JUMP -> 1
 *   3: ADD
 * Jump at 2 targets 1 (the JUMP at 1), so the JUMP is "targeted". The pass must
 * leave everything untouched to avoid silently rerouting the edge from 2 to the
 * JUMPIF's new target. */
UT_TEST(test_ji_targeted_jump_preserved)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_EQ);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* Other JUMPIF targets the JUMP (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: JUMPIF(NE) -> 1
 *   3: ADD
 * JUMPIF at 2 targets 1 (the JUMP at 1), so the JUMP is "targeted". */
UT_TEST(test_ji_targeted_jumpif_preserved)
{
  TCCIRState *ir = utb_new();

  int jif0 = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  int jif2 = emit_jumpif_cond(ir, 1, TOK_NE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif0), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, jif2), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* Out-of-range JUMPIF destination (NEGATIVE):
 *   0: JUMPIF(EQ) -> 99
 *   1: JUMP -> 2
 *   2: ADD
 * The pass validates the destination is in [0, n) and skips out-of-range. */
UT_TEST(test_ji_out_of_range_dest_skipped)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 99, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 99);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* JUMPIF not immediately followed by JUMP (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: ADD
 *   2: JUMP -> 3
 *   3: ADD
 * The instruction immediately after the JUMPIF (skipping NOPs) is ADD at 1, not
 * a JUMP. No inversion. */
UT_TEST(test_ji_not_immediately_followed_by_jump_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_EQ);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* Idempotency (POSITIVE):
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: ADD
 * First pass: inverts JUMPIF, NOPs JUMP. Second pass: no JUMPIF -> JUMP pattern
 * left (JUMPIF is at 0, but 1 is NOP, 2 is ADD, so no JUMP immediately after),
 * so second pass reports 0 changes. */
UT_TEST(test_ji_idempotent)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int first = tcc_ir_opt_jumpif_invert(ir, 1);
  int second = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(first, 1);
  UT_ASSERT_EQ(second, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF at end of IR (NEGATIVE):
 *   0: ADD
 *   1: JUMPIF(EQ) -> 2
 * JUMPIF at index 1, but n=2, so jmp_idx=2 >= n. No JUMP to invert over. */
UT_TEST(test_ji_jumpif_at_end_no_change)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);

  utb_free(ir);
  return 0;
}

/* JUMPIF followed by only NOPs (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: NOP
 *   2: NOP
 * After skipping NOPs, jmp_idx >= n. No JUMP to invert over. */
UT_TEST(test_ji_jumpif_followed_by_only_nops_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);

  utb_free(ir);
  return 0;
}

/* Switch table targets the JUMP (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: ADD
 * Plus a switch table entry targeting 1. The JUMP is "targeted" via switch. */
UT_TEST(test_ji_switch_table_targeting_jump_preserved)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  /* Add a switch table entry targeting the JUMP at index 1. */
  ir->switch_tables = (TCCIRSwitchTable *)tcc_mallocz(sizeof(TCCIRSwitchTable));
  ir->num_switch_tables = 1;
  ir->switch_tables[0].num_entries = 1;
  ir->switch_tables[0].targets = (int *)tcc_mallocz(sizeof(int));
  ir->switch_tables[0].targets[0] = 1;

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);

  tcc_free(ir->switch_tables[0].targets);
  tcc_free(ir->switch_tables);
  utb_free(ir);
  return 0;
}

/* Switch table default target in range (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: ADD
 * Plus a switch table with default_target=1. The JUMP is "targeted" via default. */
UT_TEST(test_ji_switch_table_default_targeting_jump_preserved)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  /* Add a switch table with default_target=1. */
  ir->switch_tables = (TCCIRSwitchTable *)tcc_mallocz(sizeof(TCCIRSwitchTable));
  ir->num_switch_tables = 1;
  ir->switch_tables[0].num_entries = 0;
  ir->switch_tables[0].targets = NULL;
  ir->switch_tables[0].default_target = 1;

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);

  tcc_free(ir->switch_tables);
  utb_free(ir);
  return 0;
}

/* NOP before JUMP, switch table targets the NOP (NEGATIVE):
 *   0: JUMPIF(EQ) -> 2
 *   1: NOP
 *   2: JUMP -> 3
 *   3: ADD
 * Plus a switch table entry targeting 1 (the NOP before the JUMP). The JUMP is
 * "targeted" via the switch. */
UT_TEST(test_ji_switch_table_targeting_nop_before_jump_preserved)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  /* Add a switch table entry targeting the NOP at index 1. */
  ir->switch_tables = (TCCIRSwitchTable *)tcc_mallocz(sizeof(TCCIRSwitchTable));
  ir->num_switch_tables = 1;
  ir->switch_tables[0].num_entries = 1;
  ir->switch_tables[0].targets = (int *)tcc_mallocz(sizeof(int));
  ir->switch_tables[0].targets[0] = 1;

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);

  tcc_free(ir->switch_tables[0].targets);
  tcc_free(ir->switch_tables);
  utb_free(ir);
  return 0;
}

/* Switch table target outside range (POSITIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: ADD
 * Plus a switch table entry targeting 5 (outside the [0,3) range). The JUMP is
 * NOT "targeted" (5 > jmp_idx=1 is false, 5 <= 1 is false). Inversion proceeds. */
UT_TEST(test_ji_switch_table_target_outside_range_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  /* Add a switch table entry targeting 5 (outside range). */
  ir->switch_tables = (TCCIRSwitchTable *)tcc_mallocz(sizeof(TCCIRSwitchTable));
  ir->num_switch_tables = 1;
  ir->switch_tables[0].num_entries = 1;
  ir->switch_tables[0].targets = (int *)tcc_mallocz(sizeof(int));
  ir->switch_tables[0].targets[0] = 5;

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);

  tcc_free(ir->switch_tables[0].targets);
  tcc_free(ir->switch_tables);
  utb_free(ir);
  return 0;
}

/* Empty IR (NEGATIVE / no-op):
 * No instructions -> no JUMPIFs to process. */
UT_TEST(test_ji_empty_ir_no_change)
{
  TCCIRState *ir = utb_new();

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

/* Single JUMPIF with no JUMP after (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 * n=1, jmp_idx=1 >= n. No JUMP to invert over. */
UT_TEST(test_ji_single_jumpif_no_jump_after_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);

  utb_free(ir);
  return 0;
}

/* JUMPIF with condition that inverts to -1 (NEGATIVE):
 *   0: JUMPIF(0x00) -> 1
 *   1: JUMP -> 2
 *   2: ADD
 * invert_condition(0x00) returns -1, so the pass skips this JUMPIF. */
UT_TEST(test_ji_invalid_condition_skipped)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, 0x00);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> NOP -> target (POSITIVE):
 *   0: JUMPIF(EQ) -> 3
 *   1: JUMP -> 3
 *   2: NOP
 *   3: ADD
 * After the JUMP at 1, skip NOPs to find the next real instruction at 3.
 * 3 == JUMPIF's dest=3 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_jump_followed_by_nop_to_target_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 3, TOK_EQ);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 3);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> JUMPIF -> target (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 2
 *   2: JUMPIF(NE) -> 3
 *   3: ADD
 * The instruction after the JUMP at 1 is a JUMPIF at 2, not a real instruction
 * that matches the JUMPIF's dest=1. Actually, the code checks `jq->op !=
 * TCCIR_OP_JUMP`, so the JUMPIF at 2 is not a JUMP -> no inversion. */
UT_TEST(test_ji_jump_followed_by_jumpif_no_inversion)
{
  TCCIRState *ir = utb_new();

  int jif0 = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 2);
  int jif2 = emit_jumpif_cond(ir, 3, TOK_NE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif0), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, jif2), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> JUMP -> target (POSITIVE, chain):
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: JUMP -> 3
 *   3: ADD
 * The JUMP at 1's target=2, but after=2 (skip NOPs from 2 -> JUMP at 2 is not
 * NOP, so after=2). 2 == 2 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_jump_chain_falls_through_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 2);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP NOP);
  /* Second JUMP unchanged */
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> target, but target is JUMPIF's dest (NEGATIVE):
 *   0: JUMPIF(EQ) -> 1
 *   1: JUMP -> 0
 *   2: ADD
 * JUMP's target=0 < JUMPIF's index=0? No, 0 == 0. But the check is `after !=
 * a` where a=1 (JUMPIF's dest). after=2 (skip NOPs from 2 -> ADD at 2 is real,
 * so after=2). 2 != 1 -> no inversion. Correct.
 *
 * Actually, let me re-check: the JUMPIF's dest is 1, not 0. So a=1. after=2.
 * 2 != 1 -> no inversion. */
UT_TEST(test_ji_jump_to_jumpif_dest_no_match_no_change)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 1, TOK_EQ);
  emit_jump(ir, 0);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP JUMP);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> target, target is JUMPIF's dest (POSITIVE):
 *   0: JUMPIF(EQ) -> 2
 *   1: JUMP -> 2
 *   2: ADD
 * a=2, jmp_idx=1, after=2 (skip NOPs from 2 -> ADD at 2 is real, so after=2).
 * 2 == 2 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_jump_to_jumpif_dest_after_nop_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 2, TOK_EQ);
  emit_jump(ir, 2);
  utb_emit(ir, TCCIR_OP ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 2);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> target, target is JUMPIF's dest via NOP (POSITIVE):
 *   0: JUMPIF(EQ) -> 3
 *   1: JUMP -> 3
 *   2: NOP
 *   3: ADD
 * a=3, jmp_idx=1, after=2 (skip NOPs from 2 -> NOP at 2, so after=3). after=3
 * (ADD at 3 is real). 3 == 3 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_jump_to_jumpif_dest_via_nop_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 3, TOK_EQ);
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 3);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* JUMPIF -> JUMP -> target, target is JUMPIF's dest via multiple NOPs (POSITIVE):
 *   0: JUMPIF(EQ) -> 4
 *   1: JUMP -> 4
 *   2: NOP
 *   3: NOP
 *   4: ADD
 * a=4, jmp_idx=1, after=2 (skip NOPs from 2 -> NOP, after=3 -> NOP, after=4
 * -> ADD at 4 is real). 4 == 4 -> qualifies. Inversion proceeds. */
UT_TEST(test_ji_jump_to_jumpif_dest_via_multiple_nops_inverts)
{
  TCCIRState *ir = utb_new();

  int jif = emit_jumpif_cond(ir, 4, TOK_EQ);
  emit_jump(ir, 4);
  utb_emit(ir, TCCIR_OP NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_jumpif_invert(ir, 1);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP JUMPIF);
  UT_ASSERT_EQ(jump_target(ir, jif), 4);
  UT_ASSERT_EQ(jumpif_cond(ir, jif), TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}