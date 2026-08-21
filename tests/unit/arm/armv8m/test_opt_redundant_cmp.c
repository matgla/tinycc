/*
 *  test_opt_redundant_cmp.c - suite for opt/flat/dce/redundant_cmp.c
 *  (tcc_ir_opt_redundant_cmp).
 *
 *  The pass deletes a CMP whose flags a dominating identical CMP already left
 *  in the APSR.  Because a surviving-flags claim is only as good as its
 *  reachability argument, the negative tests here matter more than the
 *  positive ones: each removes exactly one premise (single predecessor, no
 *  fall-in, NOP-only gap, identical operands) from a shape the pass DOES fire
 *  on, and asserts the second compare survives.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_redundant_cmp(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

#define TOK_EQ 0x94
#define TOK_NE 0x95
#define TOK_LT 0x9c
#define TOK_GE 0x9d

/* `if (a == b) <hit>; if (a < b) ...` -- the taken edge of the first branch
 * lands on the second compare, and nothing else reaches it. */
static TCCIRState *build_taken_edge(int gap_is_nop, int second_operand_differs,
                                    int extra_branch_to_target)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_temp(1, I32), b = utb_temp(2, I32), c = utb_temp(3, I32);

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);                              /* 0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_NE, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, c, utb_imm(7, I32), UTB_NONE);             /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(7, I32), UTB_NONE, UTB_NONE);        /* 3 */
  if (gap_is_nop)
    utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);              /* 4 */
  else
    utb_emit(ir, TCCIR_OP_ADD, c, a, b);         /* 4: clobbers nothing we */
                                                 /*    compare, but is not */
                                                 /*    a NOP -- refuse.    */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a,                                  /* 5 */
           second_operand_differs ? c : b);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 6 */
  if (extra_branch_to_target)
    utb_emit(ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);      /* 7 */
  else
    utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);       /* 7 */
  return ir;
}

/* Second compare on the branch's FALL-THROUGH successor instead. */
static TCCIRState *build_fallthrough(int target_is_reachable_elsewhere)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_temp(1, I32), b = utb_temp(2, I32);

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);                               /* 0 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);                               /* 2 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_GE, I32), UTB_NONE); /* 3 */
  if (target_is_reachable_elsewhere)
    utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);       /* 4 */
  else
    utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);               /* 4 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 5 */
  return ir;
}

UT_TEST(redundant_cmp_taken_edge_deleted)
{
  TCCIRState *ir = build_taken_edge(1, 0, 0);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[5].op, TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(redundant_cmp_taken_edge_kept_when_gap_is_not_nops)
{
  TCCIRState *ir = build_taken_edge(0, 0, 0);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[5].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(redundant_cmp_kept_when_operands_differ)
{
  TCCIRState *ir = build_taken_edge(1, 1, 0);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[5].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

/* A second branch into the same target means control can arrive without having
 * executed the first compare: the flags there are somebody else's. */
UT_TEST(redundant_cmp_kept_when_target_has_two_predecessors)
{
  TCCIRState *ir = build_taken_edge(1, 0, 1);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[5].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

/* The instruction above the target is not a terminator, and a branch from
 * elsewhere lands on it -- so control can fall into the target having never
 * executed the first compare, carrying whatever flags that other path left.
 * This is the case the terminator check exists for. */
UT_TEST(redundant_cmp_kept_when_target_can_be_fallen_into)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_temp(1, I32), b = utb_temp(2, I32), c = utb_temp(3, I32);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 0 -> the fall-in */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);                               /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_NE, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_ASSIGN, c, utb_imm(7, I32), UTB_NONE);              /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(8, I32), UTB_NONE, UTB_NONE);         /* 4 */
  utb_emit(ir, TCCIR_OP_ASSIGN, c, utb_imm(9, I32), UTB_NONE);              /* 5: no compare ran */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);                               /* 6: falls in from 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);          /* 8 */

  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[6].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(redundant_cmp_fallthrough_deleted)
{
  TCCIRState *ir = build_fallthrough(0);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[2].op, TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(redundant_cmp_fallthrough_kept_when_branch_targets_it)
{
  TCCIRState *ir = build_fallthrough(1);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[2].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

/* A compare whose flags are not consumed by a branch is out of scope: the pass
 * keys the whole reachability argument on the CMP/JUMPIF pair. */
UT_TEST(redundant_cmp_kept_without_a_branch)
{
  TCCIRState *ir = utb_new();
  IROperand a = utb_temp(1, I32), b = utb_temp(2, I32), c = utb_temp(3, I32);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);
  utb_emit(ir, TCCIR_OP_SETIF, c, utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[2].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

/* A computed jump can land anywhere, so the taken-edge case must be refused
 * for the whole function once one is present. */
UT_TEST(redundant_cmp_taken_edge_kept_with_a_computed_jump)
{
  TCCIRState *ir = build_taken_edge(1, 0, 0);
  /* Turn the trailing RETURNVOID into an indirect jump. */
  ir->compact_instructions[7].op = TCCIR_OP_IJUMP;
  UT_ASSERT_EQ(tcc_ir_opt_redundant_cmp(ir), 0);
  UT_ASSERT_EQ(ir->compact_instructions[5].op, TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_COVERS("redundant_cmp");
