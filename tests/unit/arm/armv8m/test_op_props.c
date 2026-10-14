/*
 *  test_op_props.c - suite for ir/op_props.c (the opcode property table) and
 *  opt/util/range.c (hazard queries over instructions and ranges, block
 *  cursors).
 *
 *  The table half is a selftest: every opcode below TCCIR_OP_COUNT must have a
 *  row, and the rows must be consistent with each other and with the hazard
 *  vocabulary.  A new opcode without a row fails here, not as a miscompile in
 *  whichever pass first treats it as harmless.
 */

#include "ir_build.h"
#include "opt_range.h"

#include "ut.h"

#define I32 IROP_BTYPE_INT32

/* ----------------------------------------------------------- the table */

UT_TEST(test_op_props_every_op_has_a_row)
{
  for (int op = 0; op < TCCIR_OP_COUNT; op++)
  {
    if (!ir_op_has(op, IROP_A_KNOWN))
      printf("    op %d has no ir_op_props[] row\n", op);
    UT_ASSERT(ir_op_has(op, IROP_A_KNOWN));
  }
  return 0;
}

UT_TEST(test_op_props_rows_carry_only_opcode_bits)
{
  for (int op = 0; op < TCCIR_OP_COUNT; op++)
    UT_ASSERT_EQ(ir_op_props[op] & IR_HZ_FROM_INSTR, 0u);
  return 0;
}

UT_TEST(test_op_props_hazard_masks_partition)
{
  UT_ASSERT_EQ(IR_HZ_FROM_OP | IR_HZ_FROM_INSTR, IR_HZ_ALL);
  UT_ASSERT_EQ(IR_HZ_FROM_OP & IR_HZ_FROM_INSTR, 0u);
  const uint32_t attrs = IROP_A_NO_FALLTHROUGH | IROP_A_RETURNS_TWICE | IROP_A_SLOT3 | IROP_A_FP |
                         IROP_A_COMMUTATIVE | IROP_A_MAY_BRANCH | IROP_A_KNOWN;
  UT_ASSERT_EQ(attrs & IR_HZ_ALL, 0u);
  return 0;
}

UT_TEST(test_op_props_every_hazard_has_a_name)
{
  for (int b = 0; b < 32; b++)
  {
    uint32_t bit = 1u << b;
    if (bit & IR_HZ_ALL)
      UT_ASSERT(strcmp(ir_hazard_name(bit), "?") != 0);
  }
  return 0;
}

/* A deny-by-default range query must see every op that ends a block. */
UT_TEST(test_op_props_block_enders_are_hazards)
{
  for (int op = 0; op < TCCIR_OP_COUNT; op++)
    if (ir_op_has(op, IROP_ENDS_BLOCK))
      UT_ASSERT(ir_op_has(op, IR_HZ_FROM_OP));
  return 0;
}

UT_TEST(test_op_props_slot3_covers_mac)
{
  for (int op = 0; op < TCCIR_OP_COUNT; op++)
    if (tcc_ir_op_is_mac(op))
      UT_ASSERT(ir_op_has(op, IROP_A_SLOT3));
  UT_ASSERT(ir_op_has(TCCIR_OP_SELECT, IROP_A_SLOT3));
  UT_ASSERT(ir_op_has(TCCIR_OP_LOAD_INDEXED, IROP_A_SLOT3));
  UT_ASSERT(ir_op_has(TCCIR_OP_STORE_INDEXED, IROP_A_SLOT3));
  return 0;
}

UT_TEST(test_op_props_opset_spans_both_words)
{
  IROpSet s = IROPSET(TCCIR_OP_ADD, TCCIR_OP_UMAAL, TCCIR_OP_NOP);
  UT_ASSERT(TCCIR_OP_UMAAL >= 64); /* exercises the second word */
  UT_ASSERT(ir_opset_has(s, TCCIR_OP_ADD));
  UT_ASSERT(ir_opset_has(s, TCCIR_OP_UMAAL));
  UT_ASSERT(ir_opset_has(s, TCCIR_OP_NOP));
  UT_ASSERT(!ir_opset_has(s, TCCIR_OP_SUB));
  UT_ASSERT(!ir_opset_has(IROPSET_NONE, TCCIR_OP_ADD));
  return 0;
}

/* ----------------------------------------------------------- ranges */

/* t1 = t0; <mid>; t3 = t2 -- returns a state with the middle op at index 1. */
static TCCIRState *rng_three(TccIrOp mid_op, IROperand mid_dest)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, mid_op, mid_dest, utb_temp(4, I32), utb_temp(5, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
  return ir;
}

UT_TEST(test_range_all_rejects_a_store)
{
  TCCIRState *ir = rng_three(TCCIR_OP_STORE, utb_lval(utb_temp(6, I32)));
  UT_ASSERT(!ir_range_safe(ir, 0, 2, IR_HZ_ALL));
  UT_ASSERT_EQ(ir_range_first_hazard(ir, 0, 2, IR_HZ_ALL), 1);
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], IR_HZ_ALL), IR_HZ_MEM_WRITE | IR_HZ_DEST_LVAL);
  /* proving the write harmless takes both: the opcode and its lvalue dest */
  UT_ASSERT(!ir_range_safe(ir, 0, 2, IR_HZ_ALL & ~IR_HZ_MEM_WRITE));
  UT_ASSERT(ir_range_safe(ir, 0, 2, IR_HZ_ALL & ~(IR_HZ_MEM_WRITE | IR_HZ_DEST_LVAL)));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_pure_alu_is_safe)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ADD, utb_temp(6, I32));
  UT_ASSERT(ir_range_safe(ir, 0, 2, IR_HZ_ALL));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_lvalue_dest_on_alu_is_a_write)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ADD, utb_lval(utb_temp(6, I32)));
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], IR_HZ_ALL), IR_HZ_DEST_LVAL);
  UT_ASSERT(!ir_range_safe(ir, 0, 2, IR_HZ_ALL & ~IR_HZ_MEM_WRITE));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_stackoff_dest_is_a_frame_write)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ASSIGN, utb_stackoff(-8, 0, 0, 0, I32));
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], IR_HZ_ALL), IR_HZ_DEST_STACKOFF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_lvalue_source_is_a_read)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(6, I32), utb_lval(utb_temp(4, I32)), utb_temp(5, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], IR_HZ_ALL), IR_HZ_SRC_LVAL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_mac_accumulator_lvalue_is_a_read)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit4(ir, TCCIR_OP_MLA, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32), utb_lval(utb_temp(7, I32)));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], IR_HZ_ALL), IR_HZ_SRC_LVAL);
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_nop_join_is_a_hazard)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ADD, utb_temp(6, I32));
  ir->compact_instructions[1].op = TCCIR_OP_NOP;
  ir->compact_instructions[1].is_jump_target = 1;
  UT_ASSERT_EQ(ir_range_first_hazard(ir, 0, 2, IR_HZ_ALL), 1);
  UT_ASSERT(ir_range_safe(ir, 0, 2, IR_HZ_ALL & ~IR_HZ_JOIN));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_join_at_end)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ADD, utb_temp(6, I32));
  ir->compact_instructions[2].is_jump_target = 1;
  UT_ASSERT_EQ(ir_range_first_hazard(ir, 0, 2, IR_HZ_ALL), 2);
  UT_ASSERT(ir_range_safe(ir, 0, 2, IR_HZ_ALL & ~IR_HZ_JOIN_END));
  /* the anchor's own join flag is not part of the range */
  ir->compact_instructions[2].is_jump_target = 0;
  ir->compact_instructions[0].is_jump_target = 1;
  UT_ASSERT(ir_range_safe(ir, 0, 2, IR_HZ_ALL));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_empty_and_inverted)
{
  TCCIRState *ir = rng_three(TCCIR_OP_STORE, utb_lval(utb_temp(6, I32)));
  UT_ASSERT(ir_range_safe(ir, 0, 1, IR_HZ_ALL)); /* nothing strictly between */
  UT_ASSERT(ir_range_safe(ir, 1, 1, IR_HZ_ALL));
  UT_ASSERT_EQ(ir_range_first_hazard(ir, 2, 0, IR_HZ_ALL), 2); /* inverted: a hazard */
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_except_keeps_instruction_hazards)
{
  TCCIRState *ir = rng_three(TCCIR_OP_ASM_OUTPUT, utb_lval(utb_temp(6, I32)));
  IROpSet asm_out = IROPSET(TCCIR_OP_ASM_OUTPUT);
  UT_ASSERT_EQ(ir_q_hazards_except(ir, &ir->compact_instructions[1], IR_HZ_ALL, asm_out), IR_HZ_DEST_LVAL);
  ir->iroperand_pool[ir->compact_instructions[1].operand_base] = utb_temp(6, I32);
  UT_ASSERT(ir_range_safe_except(ir, 0, 2, IR_HZ_ALL, asm_out));
  UT_ASSERT(!ir_range_safe(ir, 0, 2, IR_HZ_ALL));
  utb_free(ir);
  return 0;
}

UT_TEST(test_range_volatile_access)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(6, I32), utb_lval(utb_temp(4, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
  const uint32_t reads_ok = IR_HZ_ALL & ~(IR_HZ_MEM_READ | IR_HZ_SRC_LVAL);
  /* a function with no volatile access: an unmarked lvalue is not volatile */
  UT_ASSERT(ir_range_safe(ir, 0, 2, reads_ok));
  ir->func_has_volatile_access = 1;
  UT_ASSERT_EQ(ir_q_hazards(ir, &ir->compact_instructions[1], reads_ok), IR_HZ_VOLATILE);
  /* proven non-volatile */
  IROperand *src = &ir->iroperand_pool[ir->compact_instructions[1].operand_base + 1];
  src->aux |= IROP_AUX_NONVOLATILE;
  UT_ASSERT(ir_range_safe(ir, 0, 2, reads_ok));
  utb_free(ir);
  return 0;
}

/* ----------------------------------------------------------- cursors */

UT_TEST(test_bb_next_skips_nops_and_stops_at_a_nop_join)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_temp(1, I32)); /* 2 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                    /* 3: join */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(2, I32)); /* 4 */
  ir->compact_instructions[3].is_jump_target = 1;
  int n = ir->next_instruction_index;
  UT_ASSERT_EQ(ir_bb_next(ir, 0, n), 2);
  UT_ASSERT_EQ(ir_bb_next(ir, 2, n), -1);
  UT_ASSERT_EQ(ir_bb_next(ir, 0, 2), -1); /* limit */
  UT_ASSERT_EQ(ir_bb_prev(ir, 2, 0), 0);
  UT_ASSERT_EQ(ir_bb_prev(ir, 4, 0), -1); /* the NOP join heads 4's block */
  ir->compact_instructions[4].is_jump_target = 1;
  UT_ASSERT_EQ(ir_bb_prev(ir, 4, 0), -1); /* 4 heads its own block */
  utb_free(ir);
  return 0;
}

UT_TEST(test_bb_cursor_stops_at_block_enders)
{
  static const TccIrOp enders[] = {TCCIR_OP_JUMP,   TCCIR_OP_JUMPIF,     TCCIR_OP_IJUMP,
                                   TCCIR_OP_RETURNVOID, TCCIR_OP_TRAP,   TCCIR_OP_SETJMP,
                                   TCCIR_OP_LONGJMP, TCCIR_OP_INLINE_ASM, TCCIR_OP_SWITCH_TABLE};
  for (unsigned e = 0; e < sizeof(enders) / sizeof(enders[0]); e++)
  {
    TCCIRState *ir = utb_new();
    utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
    utb_emit(ir, enders[e], utb_imm(0, I32), utb_temp(1, I32), utb_imm(0, I32));
    utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32), UTB_NONE);
    UT_ASSERT_EQ(ir_bb_next(ir, 0, 3), 1);
    UT_ASSERT_EQ(ir_bb_next(ir, 1, 3), -1);
    UT_ASSERT_EQ(ir_bb_prev(ir, 2, 0), -1);
    UT_ASSERT_EQ(ir_bb_prev(ir, 1, 0), 0);
    utb_free(ir);
  }
  return 0;
}

UT_TEST(test_bb_cursor_walks_through_calls)
{
  TCCIRState *ir = rng_three(TCCIR_OP_FUNCCALLVAL, utb_temp(6, I32));
  UT_ASSERT_EQ(ir_bb_next(ir, 0, 3), 1);
  UT_ASSERT_EQ(ir_bb_next(ir, 1, 3), 2);
  UT_ASSERT_EQ(ir_bb_prev(ir, 2, 0), 1);
  utb_free(ir);
  return 0;
}
