/*
 *  test_ir_stack.c - suite for ir/stack.c stack layout helpers
 *
 *  Exercises stack-slot queries, physical-register assignment, frame-size
 *  queries, and the legacy wrapper APIs.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

/* -------------------------------------------------------------------------- */
/* Empty-state queries                                                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_slot_count_empty)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_slot_by_vreg_invalid)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_stack_slot_by_vreg(ir, -1) == NULL);
  UT_ASSERT(tcc_ir_stack_slot_by_vreg(ir, 0) == NULL);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_slot_by_offset_invalid)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_stack_slot_by_offset(ir, -8) == NULL);
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

UT_TEST(test_args_offset_and_size_default)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_stack_args_offset(ir), 0);
  UT_ASSERT_EQ(tcc_ir_stack_args_size(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Physical register assignment                                               */
/* -------------------------------------------------------------------------- */

UT_TEST(test_reg_assign_and_get)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);

  tcc_ir_stack_reg_assign(ir, v0, 0, 5, PREG_NONE);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT_EQ(r0, 5);
  UT_ASSERT_EQ(r1, PREG_NONE);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spilled_var_gets_spilled_flag)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);

  tcc_ir_stack_reg_assign(ir, v0, -16, 0, PREG_NONE);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT(r0 & PREG_SPILLED);
  UT_ASSERT_EQ(r0 & PREG_REG_NONE, PREG_REG_NONE);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_reg_get_unassigned_vreg)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int r0 = 123, r1 = 456;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT_EQ(r0, PREG_NONE);
  UT_ASSERT_EQ(r1, PREG_NONE);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Manual slot manipulation / reset                                           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_stack_reset_clears_slots)
{
  TCCIRState *ir = tcc_ir_alloc();
  ir->stack_layout.slot_count = 1;
  ir->stack_layout.slots = (TCCStackSlot *)tcc_malloc(sizeof(TCCStackSlot));
  memset(ir->stack_layout.slots, 0, sizeof(TCCStackSlot));
  ir->stack_layout.slots[0].offset = -8;
  ir->stack_layout.slots[0].size = 4;

  tcc_ir_stack_reset(ir);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_slot_by_index_bounds)
{
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_stack_slot_by_index(ir, 0) == NULL);
  UT_ASSERT(tcc_ir_stack_slot_by_index(ir, -1) == NULL);

  ir->stack_layout.slot_count = 1;
  ir->stack_layout.slots = (TCCStackSlot *)tcc_malloc(sizeof(TCCStackSlot));
  memset(ir->stack_layout.slots, 0, sizeof(TCCStackSlot));
  ir->stack_layout.slots[0].offset = -12;
  ir->stack_layout.slots[0].size = 4;

  const TCCStackSlot *s = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->offset, -12);
  UT_ASSERT_EQ(s->size, 4);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Legacy wrappers                                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_legacy_build_stack_layout_no_crash)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_build_stack_layout(ir);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_legacy_assign_physical_register)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);

  tcc_ir_assign_physical_register(ir, v0, 0, 7, PREG_NONE);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT_EQ(r0, 7);

  tcc_ir_free(ir);
  return 0;
}
