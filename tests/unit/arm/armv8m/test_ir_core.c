/*
 *  test_ir_core.c - suite for ir/core.c IR instruction building
 *
 *  Exercises instruction append, operand packing, leaf/call tracking,
 *  jump-chain backpatching, and the irop_config shape table.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

static SValue sv_var(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
  sv.type.t = VT_INT;
  return sv;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle and basic counts                                                 */
/* -------------------------------------------------------------------------- */

UT_TEST(test_alloc_fresh_block_has_zero_instructions)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(ir != NULL);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  UT_ASSERT_EQ(ir->iroperand_pool_count, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_add_packs_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue dest = sv_var(t0);
  SValue src1 = sv_var(v0);
  SValue src2 = sv_const(7);

  int idx = tcc_ir_put(ir, TCCIR_OP_ADD, &src1, &src2, &dest);
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);

  IRQuadCompact *q = &ir->compact_instructions[idx];
  UT_ASSERT_EQ(q->op, TCCIR_OP_ADD);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);

  UT_ASSERT_EQ(irop_get_vreg(d), t0);
  UT_ASSERT_EQ(irop_get_vreg(s1), v0);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(s2), 7);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_put_no_op_has_no_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  int before = ir->iroperand_pool_count;
  int idx = tcc_ir_put_no_op(ir, TCCIR_OP_NOP);
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->iroperand_pool_count, before);

  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT(irop_is_none(d));
  UT_ASSERT(irop_is_none(s1));
  UT_ASSERT(irop_is_none(s2));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_count_and_current_idx)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int t1 = tcc_ir_vreg_alloc_temp(ir);

  SValue s_t0 = sv_var(t0);
  SValue s_t1 = sv_var(t1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_t0, NULL, &s_t1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(tcc_ir_current_idx(ir), 0);

  SValue r_t1 = sv_var(t1);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &r_t1, NULL, NULL);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_ASSERT_EQ(tcc_ir_current_idx(ir), 1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Leaf / call tracking                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_leaf_by_default)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_call_marks_nonleaf)
{
  TCCIRState *ir = tcc_ir_alloc();
  SValue func = sv_const(0);
  SValue call_info = sv_const((int)TCCIR_ENCODE_CALL(0, 0));
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &func, &call_info, NULL);
  UT_ASSERT(!tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_nonleaf_mark_explicit)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_nonleaf_mark(ir);
  UT_ASSERT(!tcc_ir_is_leaf(ir));
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_call_id_next_monotonic)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 0);
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 1);
  UT_ASSERT_EQ(tcc_ir_call_id_next(ir), 2);
  UT_ASSERT_EQ(ir->next_call_id, 3);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Operand setters / getters                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_set_dest_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  int t1 = tcc_ir_vreg_alloc_temp(ir);

  SValue dest = sv_var(t0);
  SValue src = sv_const(1);
  int idx = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);

  IROperand new_dest = irop_make_vreg(t1, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, idx, new_dest);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[idx]);
  UT_ASSERT_EQ(irop_get_vreg(d), t1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Jump-chain backpatching                                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_backpatch_to_here)
{
  TCCIRState *ir = tcc_ir_alloc();

  /* JUMP with target 7, followed by a few NOPs. */
  SValue target = sv_jump_target(7);
  int head = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &target);
  tcc_ir_put_no_op(ir, TCCIR_OP_NOP);
  tcc_ir_put_no_op(ir, TCCIR_OP_NOP);

  /* Backpatch the jump chain to the current instruction position.
   * tcc_ir_backpatch_to_here stores ir->next_instruction_index as the target. */
  tcc_ir_backpatch_to_here(ir, head);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[head]);
  UT_ASSERT_EQ(irop_get_imm32(d), ir->next_instruction_index);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* irop_config shape                                                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_irop_config_shapes)
{
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_src1);
  UT_ASSERT(irop_config[TCCIR_OP_ADD].has_src2);

  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_NOP].has_src2);

  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_RETURNVALUE].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_src2);

  UT_ASSERT(irop_config[TCCIR_OP_JUMP].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_JUMP].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_JUMP].has_src2);

  UT_ASSERT(irop_config[TCCIR_OP_STORE].has_dest);
  UT_ASSERT(irop_config[TCCIR_OP_STORE].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_STORE].has_src2);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_core)
{
  UT_RUN(test_alloc_fresh_block_has_zero_instructions);
  UT_RUN(test_put_add_packs_operands);
  UT_RUN(test_put_no_op_has_no_operands);
  UT_RUN(test_count_and_current_idx);
  UT_RUN(test_leaf_by_default);
  UT_RUN(test_call_marks_nonleaf);
  UT_RUN(test_nonleaf_mark_explicit);
  UT_RUN(test_call_id_next_monotonic);
  UT_RUN(test_set_dest_roundtrip);
  UT_RUN(test_backpatch_to_here);
  UT_RUN(test_irop_config_shapes);
}
