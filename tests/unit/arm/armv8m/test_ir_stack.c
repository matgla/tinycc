/*
 *  test_ir_stack.c - unit tests for the tinycc IR stack-layout module
 *
 *  Phase 2 coverage: ir/stack.c query/assignment helpers and legacy wrappers.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ut.h"

UT_TEST(test_slot_count_empty)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(ir != NULL);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_reg_assign_and_get)
{
  TCCIRState *ir = tcc_ir_alloc();
  int vreg = tcc_ir_vreg_alloc_var(ir);
  int r0 = -1, r1 = -1;

  tcc_ir_stack_reg_assign(ir, vreg, 0, 5, PREG_NONE);
  tcc_ir_stack_reg_get(ir, vreg, &r0, &r1);

  UT_ASSERT_EQ(r0, 5);
  UT_ASSERT_EQ(r1, PREG_NONE);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spilled_var_gets_spilled_flag)
{
  TCCIRState *ir = tcc_ir_alloc();
  int vreg = tcc_ir_vreg_alloc_var(ir);
  int r0 = -1, r1 = -1;

  tcc_ir_stack_reg_assign(ir, vreg, -16, 0, PREG_NONE);
  tcc_ir_stack_reg_get(ir, vreg, &r0, &r1);

  UT_ASSERT((r0 & PREG_SPILLED) != 0);
  UT_ASSERT_EQ(r1, PREG_NONE);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_stack_slot_by_vreg_invalid)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_stack_slot_by_vreg(ir, -1) == NULL);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_frame_size_empty)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_stack_frame_size(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_alignment_is_eight)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_stack_alignment(ir), 8);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_args_offset_size_default)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_stack_args_offset(ir), 0);
  UT_ASSERT_EQ(tcc_ir_stack_args_size(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_legacy_wrappers_call_through)
{
  TCCIRState *ir = tcc_ir_alloc();
  int vreg = tcc_ir_vreg_alloc_var(ir);
  int r0 = -1, r1 = -1;

  /* Legacy build wrapper must be safe on an empty IR block. */
  tcc_ir_build_stack_layout(ir);

  /* Legacy assign wrapper must forward to the new API. */
  tcc_ir_assign_physical_register(ir, vreg, 0, 7, PREG_NONE);
  tcc_ir_stack_reg_get(ir, vreg, &r0, &r1);

  UT_ASSERT_EQ(r0, 7);
  UT_ASSERT_EQ(r1, PREG_NONE);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_stack_reset_clears_slots)
{
  TCCIRState *ir = tcc_ir_alloc();

  /* Manually inject a slot to exercise the reset path. */
  ir->stack_layout.slot_count = 1;
  ir->stack_layout.slots = (TCCStackSlot *)tcc_malloc(sizeof(TCCStackSlot));
  memset(ir->stack_layout.slots, 0, sizeof(TCCStackSlot));
  ir->stack_layout.slots[0].offset = -8;
  ir->stack_layout.slots[0].size = 4;

  tcc_ir_stack_reset(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);
  UT_ASSERT(tcc_ir_stack_slot_by_index(ir, 0) == NULL);
  tcc_ir_free(ir);
  return 0;
}

UT_SUITE(ir_stack)
{
  UT_RUN(test_slot_count_empty);
  UT_RUN(test_reg_assign_and_get);
  UT_RUN(test_spilled_var_gets_spilled_flag);
  UT_RUN(test_stack_slot_by_vreg_invalid);
  UT_RUN(test_frame_size_empty);
  UT_RUN(test_alignment_is_eight);
  UT_RUN(test_args_offset_size_default);
  UT_RUN(test_legacy_wrappers_call_through);
  UT_RUN(test_stack_reset_clears_slots);
}
