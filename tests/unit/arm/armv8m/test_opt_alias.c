/*
 *  test_opt_alias.c - suite for ir/opt_alias.c (stack-slot aliasing helpers)
 *
 *  Pure operand/slot predicates plus the def-use helper find_deref_use_operand.
 *  Corner cases pinned: every btype size, the same-slot/references-slot
 *  predicates distinguishing offset/local/llocal, the stack-address-operand
 *  shape, and the single-deref-use resolver (exactly one match -> which slot).
 */

#include "ir_build.h"

#include "ut.h"
#include "opt_alias.h"

#define I32 IROP_BTYPE_INT32

/* ============================================ ir_opt_store_btype_size_bytes */

UT_TEST(test_store_btype_size_all_widths)
{
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_INT8), 1);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_INT16), 2);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_INT32), 4);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_FLOAT32), 4);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_INT64), 8);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_FLOAT64), 8);
  /* Btypes not handled by the switch (STRUCT=4, FUNC=5, out-of-range). */
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(IROP_BTYPE_STRUCT), 0);
  UT_ASSERT_EQ(ir_opt_store_btype_size_bytes(99), 0);
  return 0;
}

/* ============================================ stackoff_same_slot */

UT_TEST(test_same_slot_equal_offsets_match)
{
  IROperand a = utb_stackoff(16, 1, 0, 0, I32);
  IROperand b = utb_stackoff(16, 1, 0, 0, I32);
  UT_ASSERT(stackoff_same_slot(a, b));
  return 0;
}

UT_TEST(test_same_slot_different_offset_no_match)
{
  IROperand a = utb_stackoff(16, 1, 0, 0, I32);
  IROperand b = utb_stackoff(24, 1, 0, 0, I32);
  UT_ASSERT(!stackoff_same_slot(a, b));
  return 0;
}

UT_TEST(test_same_slot_llocal_flag_distinguishes)
{
  /* Same offset but different is_llocal -> different slot (distinct locals). */
  IROperand a = utb_stackoff(16, 1, 0, 0, I32);
  IROperand b = utb_stackoff(16, 1, 1, 0, I32);
  UT_ASSERT(!stackoff_same_slot(a, b));
  return 0;
}

UT_TEST(test_same_slot_non_stackoff_rejected)
{
  UT_ASSERT(!stackoff_same_slot(utb_imm(16, I32), utb_stackoff(16, 1, 0, 0, I32)));
  UT_ASSERT(!stackoff_same_slot(utb_temp(0, I32), utb_temp(0, I32)));
  return 0;
}

/* ============================================ operand_references_slot / is_stack_address_operand */

UT_TEST(test_operand_references_slot)
{
  IROperand slot = utb_stackoff(16, 1, 0, 0, I32);
  UT_ASSERT(operand_references_slot(utb_stackoff(16, 0, 0, 0, I32), slot)); /* same offset/local/llocal */
  UT_ASSERT(!operand_references_slot(utb_stackoff(20, 0, 0, 0, I32), slot));
  UT_ASSERT(!operand_references_slot(utb_imm(16, I32), slot)); /* not a stackoff */
  return 0;
}

UT_TEST(test_is_stack_address_operand)
{
  /* LEA form: local, non-lval, stackoff tag. */
  UT_ASSERT(is_stack_address_operand(utb_stackoff(16, 0, 0, 0, I32)));
  /* lval (deref) is not an address operand. */
  UT_ASSERT(!is_stack_address_operand(utb_stackoff(16, 1, 0, 0, I32)));
  /* non-stackoff. */
  UT_ASSERT(!is_stack_address_operand(utb_temp(0, I32)));
  UT_ASSERT(!is_stack_address_operand(utb_imm(16, I32)));
  return 0;
}

/* ============================================ find_deref_use_operand */

UT_TEST(test_find_deref_use_single_match_in_src1)
{
  /* ADD T0 = *V0 , #1 : src1 is a lval V0 deref -> which=1. */
  TCCIRState *ir = utb_new();
  int qi = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32),
                    utb_lval(utb_var(0, I32)), utb_imm(1, I32));
  int which = -1;
  UT_ASSERT_EQ(find_deref_use_operand(ir, qi, irop_get_vreg(utb_var(0, I32)), &which), 1);
  UT_ASSERT_EQ(which, 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_deref_use_single_match_in_src2)
{
  TCCIRState *ir = utb_new();
  int qi = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32),
                    utb_imm(1, I32), utb_lval(utb_var(0, I32)));
  int which = -1;
  UT_ASSERT_EQ(find_deref_use_operand(ir, qi, irop_get_vreg(utb_var(0, I32)), &which), 1);
  UT_ASSERT_EQ(which, 2);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_deref_use_no_match_returns_zero)
{
  /* src1 is a non-lval V0 (the value, not a deref) -> not a deref use. */
  TCCIRState *ir = utb_new();
  int qi = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32),
                    utb_var(0, I32), utb_imm(1, I32));
  int which = -1;
  UT_ASSERT_EQ(find_deref_use_operand(ir, qi, irop_get_vreg(utb_var(0, I32)), &which), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_deref_use_multiple_matches_returns_zero)
{
  /* Two lval references to the same vreg -> ambiguous, the resolver refuses. */
  TCCIRState *ir = utb_new();
  int qi = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32),
                    utb_lval(utb_var(0, I32)), utb_lval(utb_var(0, I32)));
  int which = -1;
  UT_ASSERT_EQ(find_deref_use_operand(ir, qi, irop_get_vreg(utb_var(0, I32)), &which), 0);
  utb_free(ir);
  return 0;
}

/* ============================================ ir_opt_stack_slot_range_for_offset */

UT_TEST(test_stack_slot_range_found_via_linear_scan)
{
  /* No offset-hash (offset_hash_size=0) -> the linear scan over slots runs.
   * Slot covering [16..24) must be resolved for an offset inside it. */
  TCCIRState *ir = utb_new();
  TCCStackSlot slots[2];
  memset(slots, 0, sizeof slots);
  slots[0].offset = 0;
  slots[0].size = 8;
  slots[1].offset = 16;
  slots[1].size = 8;
  ir->stack_layout.slots = slots;
  ir->stack_layout.slot_count = 2;
  ir->stack_layout.offset_hash_size = 0;

  int64_t base = -1, end = -1;
  UT_ASSERT_EQ(ir_opt_stack_slot_range_for_offset(ir, 20, &base, &end), 1);
  UT_ASSERT_EQ(base, 16);
  UT_ASSERT_EQ(end, 24);

  /* Offset outside any slot -> 0. */
  UT_ASSERT_EQ(ir_opt_stack_slot_range_for_offset(ir, 100, &base, &end), 0);
  /* NULL ir -> 0. */
  UT_ASSERT_EQ(ir_opt_stack_slot_range_for_offset(NULL, 20, &base, &end), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_stack_slot_range_zero_size_slot_skipped)
{
  TCCIRState *ir = utb_new();
  TCCStackSlot slots[1];
  memset(slots, 0, sizeof slots);
  slots[0].offset = 16;
  slots[0].size = 0; /* degenerate -> skipped */
  ir->stack_layout.slots = slots;
  ir->stack_layout.slot_count = 1;
  ir->stack_layout.offset_hash_size = 0;

  int64_t base = -1, end = -1;
  UT_ASSERT_EQ(ir_opt_stack_slot_range_for_offset(ir, 16, &base, &end), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_alias)
{
  UT_COVERS("ir_opt_store_btype_size_bytes");
  UT_COVERS("stackoff_same_slot");
  UT_COVERS("operand_references_slot");
  UT_COVERS("is_stack_address_operand");
  UT_COVERS("find_deref_use_operand");
  UT_COVERS("ir_opt_stack_slot_range_for_offset");
  UT_RUN(test_store_btype_size_all_widths);
  UT_RUN(test_same_slot_equal_offsets_match);
  UT_RUN(test_same_slot_different_offset_no_match);
  UT_RUN(test_same_slot_llocal_flag_distinguishes);
  UT_RUN(test_same_slot_non_stackoff_rejected);
  UT_RUN(test_operand_references_slot);
  UT_RUN(test_is_stack_address_operand);
  UT_RUN(test_find_deref_use_single_match_in_src1);
  UT_RUN(test_find_deref_use_single_match_in_src2);
  UT_RUN(test_find_deref_use_no_match_returns_zero);
  UT_RUN(test_find_deref_use_multiple_matches_returns_zero);
  UT_RUN(test_stack_slot_range_found_via_linear_scan);
  UT_RUN(test_stack_slot_range_zero_size_slot_skipped);
}
