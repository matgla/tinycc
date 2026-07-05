/*
 *  test_ir_stack_extra.c - extended suite for ir/stack.c
 *
 *  Covers the parts the base ir_stack suite doesn't reach: frame-size
 *  computation over real slots, the 64-bit (double/llong) spill path that
 *  also marks r1 spilled, register-get for a vreg with no live interval,
 *  non-default args offsets, and the NULL-ir guards.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

/* -------------------------------------------------- frame size */

UT_TEST(test_frame_size_multiple_slots_max_end)
{
  TCCIRState *ir = tcc_ir_alloc();
  ir->stack_layout.slot_count = 3;
  ir->stack_layout.slots = (TCCStackSlot *)tcc_malloc(sizeof(TCCStackSlot) * 3);
  memset(ir->stack_layout.slots, 0, sizeof(TCCStackSlot) * 3);
  ir->stack_layout.slots[0].offset = 8;
  ir->stack_layout.slots[0].size = 4;   /* end 12 */
  ir->stack_layout.slots[1].offset = 24;
  ir->stack_layout.slots[1].size = 8;   /* end 32 */
  ir->stack_layout.slots[2].offset = 16;
  ir->stack_layout.slots[2].size = 4;   /* end 20 */

  /* frame_size = max end = 32. */
  UT_ASSERT_EQ(tcc_ir_stack_frame_size(ir), 32);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_frame_size_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_stack_frame_size(NULL), 0);
  return 0;
}

/* -------------------------------------------------- 64-bit spill */

UT_TEST(test_double_spill_marks_r1_spilled)
{
  /* A 64-bit (double) value spilled to stack must have BOTH r0 and r1 marked
   * PREG_SPILLED so codegen reloads the high word instead of treating pr1 as
   * a live register. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  IRLiveInterval *li = tcc_ir_get_live_interval(ir, v0);
  UT_ASSERT(li != NULL);
  li->is_double = 1;

  tcc_ir_stack_reg_assign(ir, v0, -16, 0, PREG_NONE);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT(r0 & PREG_SPILLED);
  UT_ASSERT(r1 & PREG_SPILLED); /* high word also spilled */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_llong_spill_marks_r1_spilled)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  IRLiveInterval *li = tcc_ir_get_live_interval(ir, v0);
  li->is_llong = 1;

  tcc_ir_stack_reg_assign(ir, v0, -32, 1, 2);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT(r0 & PREG_SPILLED);
  UT_ASSERT(r1 & PREG_SPILLED);
  UT_ASSERT_EQ(r0 & PREG_REG_NONE, PREG_REG_NONE);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_32bit_spill_leaves_r1_none)
{
  /* A 32-bit value spilled: r0 spilled, r1 = PREG_NONE (not spilled). */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);

  tcc_ir_stack_reg_assign(ir, v0, -8, 3, PREG_NONE);

  int r0, r1;
  tcc_ir_stack_reg_get(ir, v0, &r0, &r1);
  UT_ASSERT(r0 & PREG_SPILLED);
  UT_ASSERT_EQ(r1, PREG_NONE);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_stack_reg_offset_recorded)
{
  /* The spill offset is stored in allocation.offset regardless of spill. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);

  tcc_ir_stack_reg_assign(ir, v0, -20, 4, PREG_NONE);
  IRLiveInterval *li = tcc_ir_get_live_interval(ir, v0);
  UT_ASSERT_EQ(li->allocation.offset, -20);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------- guards */

UT_TEST(test_reg_get_no_live_interval)
{
  /* A vreg with no live interval -> PREG_NONE for both outputs. */
  TCCIRState *ir = tcc_ir_alloc();
  int r0 = 123, r1 = 456;
  tcc_ir_stack_reg_get(ir, 9999, &r0, &r1);
  UT_ASSERT_EQ(r0, PREG_NONE);
  UT_ASSERT_EQ(r1, PREG_NONE);

  /* NULL r0/r1 pointers must not crash. */
  tcc_ir_stack_reg_get(ir, 9999, NULL, NULL);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_args_offset_and_size_nonzero)
{
  TCCIRState *ir = tcc_ir_alloc();
  ir->call_outgoing_base = 16;
  ir->call_outgoing_size = 48;
  UT_ASSERT_EQ(tcc_ir_stack_args_offset(ir), 16);
  UT_ASSERT_EQ(tcc_ir_stack_args_size(ir), 48);
  /* NULL ir -> 0 for both. */
  UT_ASSERT_EQ(tcc_ir_stack_args_offset(NULL), 0);
  UT_ASSERT_EQ(tcc_ir_stack_args_size(NULL), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_slot_count_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(NULL), 0);
  return 0;
}

UT_TEST(test_stack_reset_null_ir_no_crash)
{
  tcc_ir_stack_reset(NULL);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(ir_stack_extra)
{
  UT_COVERS("tcc_ir_stack_frame_size");
  UT_COVERS("tcc_ir_stack_reg_assign");
  UT_RUN(test_frame_size_multiple_slots_max_end);
  UT_RUN(test_frame_size_null_ir);
  UT_RUN(test_double_spill_marks_r1_spilled);
  UT_RUN(test_llong_spill_marks_r1_spilled);
  UT_RUN(test_32bit_spill_leaves_r1_none);
  UT_RUN(test_stack_reg_offset_recorded);
  UT_RUN(test_reg_get_no_live_interval);
  UT_RUN(test_args_offset_and_size_nonzero);
  UT_RUN(test_slot_count_null_ir);
  UT_RUN(test_stack_reset_null_ir_no_crash);
}
