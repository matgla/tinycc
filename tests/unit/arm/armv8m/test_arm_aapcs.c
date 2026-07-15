/*
 *  test_arm_aapcs.c - suite for arch/arm/arm_aapcs.c
 *
 *  Covers:
 *    - tcc_abi_classify_argument(): the core AAPCS-ish argument classifier.
 *      Scalar32/scalar64/struct-by-value placement, even-register-pair
 *      alignment for 64-bit args, register exhaustion -> stack spill (with
 *      NSAA tracking/growth), struct straddling registers+stack, the large
 *      (>16B) invisible-reference path (both with and without arg_flags
 *      allocated), and the defensive NULL/negative-index early-out.
 *    - tcc_abi_align_up_int(): trivial power-of-two alignment helper.
 *    - tcc_abi_call_layout_ensure_capacity(): dynamic array growth for the
 *      four parallel per-arg tables, both the already-has-capacity no-op
 *      path and the grow/realloc path (incl. zeroing of the new tail).
 *    - tcc_abi_call_layout_deinit(): frees layout resources without
 *      double-freeing or crashing on a zero/partial layout.
 */

#include "tcc.h"
#include "tccabi.h"

#include "ut.h"

/* --------------------------------------------------------------- helpers */

static TCCAbiArgDesc desc_scalar32(void)
{
  TCCAbiArgDesc d;
  memset(&d, 0, sizeof(d));
  d.kind = TCC_ABI_ARG_SCALAR32;
  d.size = 4;
  d.alignment = 4;
  return d;
}

static TCCAbiArgDesc desc_scalar64(void)
{
  TCCAbiArgDesc d;
  memset(&d, 0, sizeof(d));
  d.kind = TCC_ABI_ARG_SCALAR64;
  d.size = 8;
  d.alignment = 8;
  return d;
}

static TCCAbiArgDesc desc_struct(uint32_t size, uint8_t align)
{
  TCCAbiArgDesc d;
  memset(&d, 0, sizeof(d));
  d.kind = TCC_ABI_ARG_STRUCT_BYVAL;
  d.size = size;
  d.alignment = align;
  return d;
}

static void layout_init(TCCAbiCallLayout *layout)
{
  memset(layout, 0, sizeof(*layout));
}

/* ------------------------------------------------------ classify: guards */

UT_TEST(test_classify_null_layout_returns_stack_zero)
{
  TCCAbiArgDesc d = desc_scalar32();
  TCCAbiArgLoc loc = tcc_abi_classify_argument(NULL, 0, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 0);
  return 0;
}

UT_TEST(test_classify_null_arg_desc_returns_stack_zero)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, NULL);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 0);
  return 0;
}

UT_TEST(test_classify_negative_arg_index_returns_stack_zero)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar32();
  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, -1, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 0);
  /* Guard clause returns before touching layout state at all. */
  UT_ASSERT_EQ(layout.argc, 0);
  UT_ASSERT_EQ(layout.capacity, 0);
  return 0;
}

/* ------------------------------------------------------ classify: scalar32 */

UT_TEST(test_classify_scalar32_first_four_go_in_r0_r3)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar32();

  for (int i = 0; i < 4; i++)
  {
    TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, i, &d);
    UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
    UT_ASSERT_EQ(loc.reg_base, i);
    UT_ASSERT_EQ(loc.reg_count, 1);
    UT_ASSERT_EQ(loc.size, 4);
  }
  UT_ASSERT_EQ(layout.next_reg, 4);
  UT_ASSERT_EQ(layout.next_stack_off, 0);
  return 0;
}

UT_TEST(test_classify_scalar32_fifth_spills_to_stack)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar32();

  for (int i = 0; i < 4; i++)
    tcc_abi_classify_argument(&layout, i, &d);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 4, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 4);
  UT_ASSERT_EQ(layout.next_stack_off, 4);

  TCCAbiArgLoc loc2 = tcc_abi_classify_argument(&layout, 5, &d);
  UT_ASSERT_EQ(loc2.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc2.stack_off, 4);
  UT_ASSERT_EQ(layout.next_stack_off, 8);
  return 0;
}

UT_TEST(test_classify_scalar32_argc_tracks_highest_index_plus_one)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar32();

  tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(layout.argc, 1);
  tcc_abi_classify_argument(&layout, 2, &d);
  UT_ASSERT_EQ(layout.argc, 3);
  /* Re-classifying a lower index must not shrink argc. */
  tcc_abi_classify_argument(&layout, 1, &d);
  UT_ASSERT_EQ(layout.argc, 3);
  return 0;
}

UT_TEST(test_classify_default_stack_align_is_8_when_unset)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  UT_ASSERT_EQ(layout.stack_align, 0);
  TCCAbiArgDesc d = desc_scalar32();
  tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(layout.stack_align, 8);
  return 0;
}

UT_TEST(test_classify_stack_size_rounds_up_to_stack_align)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar32();

  /* Fill r0..r3, then push one 4-byte stack arg: next_stack_off becomes 4,
   * but stack_size must round that up to the 8-byte call-boundary align. */
  for (int i = 0; i < 5; i++)
    tcc_abi_classify_argument(&layout, i, &d);

  UT_ASSERT_EQ(layout.next_stack_off, 4);
  UT_ASSERT_EQ(layout.stack_size, 8);
  return 0;
}

/* ------------------------------------------------------ classify: scalar64 */

UT_TEST(test_classify_scalar64_takes_even_reg_pair_r0_r1)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar64();

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_base, 0);
  UT_ASSERT_EQ(loc.reg_count, 2);
  UT_ASSERT_EQ(loc.size, 8);
  UT_ASSERT_EQ(layout.next_reg, 2);
  return 0;
}

UT_TEST(test_classify_scalar64_after_one_scalar32_skips_odd_reg)
{
  /* r0 taken by a scalar32; next_reg=1 is odd, so the 64-bit arg must
   * round up to the next even register (r2:r3), leaving r1 unused/padding. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc d64 = desc_scalar64();

  tcc_abi_classify_argument(&layout, 0, &d32);
  UT_ASSERT_EQ(layout.next_reg, 1);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 1, &d64);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_base, 2);
  UT_ASSERT_EQ(loc.reg_count, 2);
  UT_ASSERT_EQ(layout.next_reg, 4);
  return 0;
}

UT_TEST(test_classify_scalar64_when_next_reg_is_3_spills_whole_arg_to_stack)
{
  /* next_reg==3 (odd) rounds up to 4, which already exceeds the "<=2" gate,
   * so the entire 64-bit value goes to the stack -- no partial reg/stack
   * straddle for scalar64 (unlike struct-by-value). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc d64 = desc_scalar64();

  tcc_abi_classify_argument(&layout, 0, &d32);
  tcc_abi_classify_argument(&layout, 1, &d32);
  tcc_abi_classify_argument(&layout, 2, &d32);
  UT_ASSERT_EQ(layout.next_reg, 3);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 3, &d64);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 8);
  UT_ASSERT_EQ(layout.next_stack_off, 8);
  UT_ASSERT_EQ(layout.next_reg, 4);
  return 0;
}

UT_TEST(test_classify_scalar64_stack_offset_8byte_aligned)
{
  /* One scalar32 on the stack (offset 0..3), then a scalar64 must be
   * pushed to stack_off=8 (rounded from 4), not immediately at 4. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc d64 = desc_scalar64();

  for (int i = 0; i < 4; i++)
    tcc_abi_classify_argument(&layout, i, &d32);
  TCCAbiArgLoc stack32 = tcc_abi_classify_argument(&layout, 4, &d32);
  UT_ASSERT_EQ(stack32.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(stack32.stack_off, 0);
  UT_ASSERT_EQ(layout.next_stack_off, 4);

  TCCAbiArgLoc loc64 = tcc_abi_classify_argument(&layout, 5, &d64);
  UT_ASSERT_EQ(loc64.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc64.stack_off, 8);
  UT_ASSERT_EQ(layout.next_stack_off, 16);
  return 0;
}

UT_TEST(test_classify_scalar64_twice_uses_r0r1_then_r2r3)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_scalar64();

  TCCAbiArgLoc a = tcc_abi_classify_argument(&layout, 0, &d);
  TCCAbiArgLoc b = tcc_abi_classify_argument(&layout, 1, &d);
  UT_ASSERT_EQ(a.reg_base, 0);
  UT_ASSERT_EQ(b.reg_base, 2);
  UT_ASSERT_EQ(layout.next_reg, 4);

  /* A third 64-bit arg no longer fits (next_reg==4 > 2) -> stack. */
  TCCAbiArgLoc c = tcc_abi_classify_argument(&layout, 2, &d);
  UT_ASSERT_EQ(c.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(c.stack_off, 0);
  return 0;
}

/* ------------------------------------------------- classify: struct small */

UT_TEST(test_classify_struct_small_fits_entirely_in_regs)
{
  /* 8-byte struct, natural 4-byte alignment -> 2 registers, no even-pair
   * rounding required (align < 8). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_struct(8, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_base, 0);
  UT_ASSERT_EQ(loc.reg_count, 2);
  UT_ASSERT_EQ(layout.next_reg, 2);
  return 0;
}

UT_TEST(test_classify_struct_8byte_align_rounds_ncrn_to_even)
{
  /* One scalar32 consumes r0 (next_reg=1), then an 8-byte-aligned struct
   * must skip r1 and start at r2. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dstruct = desc_struct(8, 8);

  tcc_abi_classify_argument(&layout, 0, &d32);
  UT_ASSERT_EQ(layout.next_reg, 1);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 1, &dstruct);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_base, 2);
  UT_ASSERT_EQ(loc.reg_count, 2);
  UT_ASSERT_EQ(layout.next_reg, 4);
  return 0;
}

UT_TEST(test_classify_struct_odd_size_rounds_up_to_word)
{
  /* size=5 rounds to slot_sz=8 -> regs_needed=2. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d = desc_struct(5, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_count, 2);
  UT_ASSERT_EQ(layout.next_reg, 2);
  return 0;
}

/* ---------------------------------------------- classify: struct straddle */

UT_TEST(test_classify_struct_straddles_regs_and_stack)
{
  /* next_reg=3 with a 2-word (8-byte) struct: 1 register available (r3),
   * 1 word must go to the stack. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dstruct = desc_struct(8, 4);

  tcc_abi_classify_argument(&layout, 0, &d32);
  tcc_abi_classify_argument(&layout, 1, &d32);
  tcc_abi_classify_argument(&layout, 2, &d32);
  UT_ASSERT_EQ(layout.next_reg, 3);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 3, &dstruct);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG_STACK);
  UT_ASSERT_EQ(loc.reg_base, 3);
  UT_ASSERT_EQ(loc.reg_count, 1);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.stack_size, 4);
  UT_ASSERT_EQ(layout.next_reg, 4);
  UT_ASSERT_EQ(layout.next_stack_off, 4);
  return 0;
}

UT_TEST(test_classify_struct_fully_out_of_regs_goes_entirely_to_stack)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dstruct = desc_struct(12, 4);

  for (int i = 0; i < 4; i++)
    tcc_abi_classify_argument(&layout, i, &d32);
  UT_ASSERT_EQ(layout.next_reg, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 4, &dstruct);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(layout.next_stack_off, 12);
  return 0;
}

UT_TEST(test_classify_struct_stack_portion_aligned_to_arg_alignment)
{
  /* A prior 4-byte stack arg leaves next_stack_off=4. A subsequent
   * fully-stacked struct with 8-byte alignment must round that up to 8
   * before placing its stack_off. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dstruct = desc_struct(8, 8);

  for (int i = 0; i < 4; i++)
    tcc_abi_classify_argument(&layout, i, &d32);
  TCCAbiArgLoc stack32 = tcc_abi_classify_argument(&layout, 4, &d32);
  UT_ASSERT_EQ(stack32.stack_off, 0);
  UT_ASSERT_EQ(layout.next_stack_off, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 5, &dstruct);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 8);
  UT_ASSERT_EQ(layout.next_stack_off, 16);
  return 0;
}

/* -------------------------------------------- classify: invisible ref */

UT_TEST(test_classify_large_struct_no_arg_flags_stays_by_value)
{
  /* size > 16 but arg_flags is NULL (caller/call-site side): must NOT take
   * the invisible-reference path -- classified as an ordinary by-value
   * composite instead (per the comment above the size>16 check). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  UT_ASSERT(layout.arg_flags == NULL);
  TCCAbiArgDesc d = desc_struct(20, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);
  /* 20 bytes / 4 = 5 words -> doesn't fit in 4 regs -> REG_STACK straddle,
   * not a 4-byte pointer. */
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG_STACK);
  UT_ASSERT_EQ(loc.reg_count, 4);
  UT_ASSERT_EQ(loc.stack_size, 4);
  return 0;
}

UT_TEST(test_classify_large_struct_with_arg_flags_uses_invisible_ref_in_reg)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 1);
  UT_ASSERT(layout.arg_flags != NULL);

  TCCAbiArgDesc d = desc_struct(24, 8);
  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);

  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_base, 0);
  UT_ASSERT_EQ(loc.reg_count, 1);
  UT_ASSERT_EQ(loc.size, 4);
  UT_ASSERT_EQ(layout.next_reg, 1);
  UT_ASSERT_EQ(layout.arg_flags[0] & TCC_ABI_ARG_FLAG_INVISIBLE_REF,
               TCC_ABI_ARG_FLAG_INVISIBLE_REF);

  /* The 8-byte natural alignment must NOT force even-register rounding
   * for an invisible reference (it's just a 4-byte pointer) -- confirmed
   * by reg_base==0 above (no skip) and by a following scalar32 landing at
   * r1, not r2. */
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgLoc loc2 = tcc_abi_classify_argument(&layout, 1, &d32);
  UT_ASSERT_EQ(loc2.reg_base, 1);
  return 0;
}

UT_TEST(test_classify_large_struct_with_arg_flags_invisible_ref_spills_to_stack)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 5);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dbig = desc_struct(32, 4);

  for (int i = 0; i < 4; i++)
    tcc_abi_classify_argument(&layout, i, &d32);
  UT_ASSERT_EQ(layout.next_reg, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 4, &dbig);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(loc.stack_off, 0);
  UT_ASSERT_EQ(loc.size, 4);
  UT_ASSERT_EQ(layout.next_stack_off, 4);
  UT_ASSERT_EQ(layout.arg_flags[4] & TCC_ABI_ARG_FLAG_INVISIBLE_REF,
               TCC_ABI_ARG_FLAG_INVISIBLE_REF);
  return 0;
}

UT_TEST(test_classify_struct_exactly_16_bytes_not_invisible_ref)
{
  /* Boundary: size>16 is a strict inequality, so exactly 16 bytes must
   * still be passed by value (fits exactly in r0..r3). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 1);
  TCCAbiArgDesc d = desc_struct(16, 4);

  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 0, &d);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(loc.reg_count, 4);
  UT_ASSERT_EQ(layout.arg_flags[0] & TCC_ABI_ARG_FLAG_INVISIBLE_REF, 0);
  return 0;
}

/* ---------------------------------------------------- classify: alignment */

UT_TEST(test_classify_argument_alignment_below_4_clamped_to_4)
{
  /* alignment=1 must be clamped to 4 for the local `align` used in the
   * REG_STACK stack-portion alignment step -- exercise via a struct that
   * straddles (so the align variable is actually used to align
   * next_stack_off). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  TCCAbiArgDesc d32 = desc_scalar32();
  TCCAbiArgDesc dstruct = desc_struct(8, 1);

  tcc_abi_classify_argument(&layout, 0, &d32);
  tcc_abi_classify_argument(&layout, 1, &d32);
  tcc_abi_classify_argument(&layout, 2, &d32);
  TCCAbiArgLoc loc = tcc_abi_classify_argument(&layout, 3, &dstruct);
  UT_ASSERT_EQ(loc.kind, TCC_ABI_LOC_REG_STACK);
  /* next_stack_off starts at 0 -- align-4 rounding is a no-op here, but
   * the important thing is it didn't crash/underflow with align=1
   * (~(align-1) with align=1 is ~0, a no-op mask, still well-defined). */
  UT_ASSERT_EQ(loc.stack_off, 0);
  return 0;
}

/* ---------------------------------------------------- tcc_abi_align_up_int */

UT_TEST(test_align_up_int_already_aligned_is_unchanged)
{
  UT_ASSERT_EQ(tcc_abi_align_up_int(8, 4), 8);
  UT_ASSERT_EQ(tcc_abi_align_up_int(0, 8), 0);
  return 0;
}

UT_TEST(test_align_up_int_rounds_up_to_next_multiple)
{
  UT_ASSERT_EQ(tcc_abi_align_up_int(1, 4), 4);
  UT_ASSERT_EQ(tcc_abi_align_up_int(5, 8), 8);
  UT_ASSERT_EQ(tcc_abi_align_up_int(9, 8), 16);
  UT_ASSERT_EQ(tcc_abi_align_up_int(3, 4), 4);
  return 0;
}

UT_TEST(test_align_up_int_align_of_1_is_identity)
{
  UT_ASSERT_EQ(tcc_abi_align_up_int(0, 1), 0);
  UT_ASSERT_EQ(tcc_abi_align_up_int(7, 1), 7);
  UT_ASSERT_EQ(tcc_abi_align_up_int(123, 1), 123);
  return 0;
}

/* --------------------------------------- tcc_abi_call_layout_ensure_capacity */

UT_TEST(test_ensure_capacity_null_layout_is_noop)
{
  /* Must not crash. */
  tcc_abi_call_layout_ensure_capacity(NULL, 4);
  return 0;
}

UT_TEST(test_ensure_capacity_non_positive_needed_is_noop)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 0);
  UT_ASSERT_EQ(layout.capacity, 0);
  UT_ASSERT(layout.locs == NULL);

  tcc_abi_call_layout_ensure_capacity(&layout, -5);
  UT_ASSERT_EQ(layout.capacity, 0);
  UT_ASSERT(layout.locs == NULL);
  return 0;
}

UT_TEST(test_ensure_capacity_from_zero_allocates_default_8)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 1);

  UT_ASSERT_EQ(layout.capacity, 8);
  UT_ASSERT(layout.locs != NULL);
  UT_ASSERT(layout.args_original != NULL);
  UT_ASSERT(layout.args_effective != NULL);
  UT_ASSERT(layout.arg_flags != NULL);

  /* Freshly (re)allocated tail must be zeroed. */
  for (int i = 0; i < layout.capacity; i++)
  {
    UT_ASSERT_EQ(layout.locs[i].kind, 0);
    UT_ASSERT_EQ(layout.arg_flags[i], 0);
  }

  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

UT_TEST(test_ensure_capacity_needed_exactly_at_boundary_uses_default_8)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 8);
  UT_ASSERT_EQ(layout.capacity, 8);
  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

UT_TEST(test_ensure_capacity_needed_over_8_doubles_until_it_fits)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 9);
  /* 8 -> 16 is the first power-of-two doubling that reaches >= 9. */
  UT_ASSERT_EQ(layout.capacity, 16);
  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

UT_TEST(test_ensure_capacity_already_sufficient_is_noop_and_preserves_data)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 4);
  UT_ASSERT_EQ(layout.capacity, 8);

  layout.locs[0].kind = TCC_ABI_LOC_REG;
  layout.locs[0].reg_base = 3;
  void *locs_ptr = layout.locs;
  void *args_orig_ptr = layout.args_original;
  void *args_eff_ptr = layout.args_effective;
  void *flags_ptr = layout.arg_flags;

  /* needed=4 <= capacity=8 and all four arrays already allocated -> no-op:
   * pointers and previously-written data must be unchanged. */
  tcc_abi_call_layout_ensure_capacity(&layout, 4);

  UT_ASSERT_EQ(layout.capacity, 8);
  UT_ASSERT(layout.locs == locs_ptr);
  UT_ASSERT(layout.args_original == args_orig_ptr);
  UT_ASSERT(layout.args_effective == args_eff_ptr);
  UT_ASSERT(layout.arg_flags == flags_ptr);
  UT_ASSERT_EQ(layout.locs[0].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(layout.locs[0].reg_base, 3);

  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

UT_TEST(test_ensure_capacity_grow_preserves_existing_data_and_zeros_tail)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 2);
  UT_ASSERT_EQ(layout.capacity, 8);

  layout.locs[1].kind = TCC_ABI_LOC_STACK;
  layout.locs[1].stack_off = 42;
  layout.arg_flags[1] = TCC_ABI_ARG_FLAG_INVISIBLE_REF;
  layout.args_original[1].size = 99;
  layout.args_effective[1].size = 77;

  tcc_abi_call_layout_ensure_capacity(&layout, 20);
  UT_ASSERT_EQ(layout.capacity, 32); /* 8 -> 16 -> 32 */

  /* Old data at index 1 survived the realloc. */
  UT_ASSERT_EQ(layout.locs[1].kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ(layout.locs[1].stack_off, 42);
  UT_ASSERT_EQ(layout.arg_flags[1], TCC_ABI_ARG_FLAG_INVISIBLE_REF);
  UT_ASSERT_EQ(layout.args_original[1].size, 99);
  UT_ASSERT_EQ(layout.args_effective[1].size, 77);

  /* New tail (from the old capacity of 8 onward) must be zeroed. */
  for (int i = 8; i < layout.capacity; i++)
  {
    UT_ASSERT_EQ(layout.locs[i].kind, 0);
    UT_ASSERT_EQ(layout.arg_flags[i], 0);
    UT_ASSERT_EQ(layout.args_original[i].size, 0);
    UT_ASSERT_EQ(layout.args_effective[i].size, 0);
  }

  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

UT_TEST(test_ensure_capacity_partial_allocation_still_regrows_all_four)
{
  /* If only some of the four arrays are allocated (capacity says "big
   * enough" but e.g. arg_flags is still NULL), the guard's compound
   * condition must fail and force a (re)alloc of every array so none stay
   * NULL. Simulate by manually allocating capacity+locs only. */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  layout.capacity = 8;
  layout.locs = (TCCAbiArgLoc *)tcc_malloc(sizeof(TCCAbiArgLoc) * 8);
  memset(layout.locs, 0, sizeof(TCCAbiArgLoc) * 8);
  /* args_original/args_effective/arg_flags intentionally left NULL. */

  tcc_abi_call_layout_ensure_capacity(&layout, 4);

  UT_ASSERT(layout.locs != NULL);
  UT_ASSERT(layout.args_original != NULL);
  UT_ASSERT(layout.args_effective != NULL);
  UT_ASSERT(layout.arg_flags != NULL);

  tcc_abi_call_layout_deinit(&layout);
  return 0;
}

/* ------------------------------------------------- tcc_abi_call_layout_deinit */

UT_TEST(test_deinit_null_layout_is_noop)
{
  /* Must not crash. */
  tcc_abi_call_layout_deinit(NULL);
  return 0;
}

UT_TEST(test_deinit_zeroed_layout_does_not_crash)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_deinit(&layout);
  /* Fully zeroed afterward too. */
  UT_ASSERT(layout.locs == NULL);
  UT_ASSERT_EQ(layout.capacity, 0);
  return 0;
}

UT_TEST(test_deinit_allocated_layout_frees_and_zeroes_struct)
{
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 4);
  UT_ASSERT(layout.locs != NULL);

  tcc_abi_call_layout_deinit(&layout);

  UT_ASSERT(layout.locs == NULL);
  UT_ASSERT(layout.args_original == NULL);
  UT_ASSERT(layout.args_effective == NULL);
  UT_ASSERT(layout.arg_flags == NULL);
  UT_ASSERT_EQ(layout.capacity, 0);
  UT_ASSERT_EQ(layout.argc, 0);
  UT_ASSERT_EQ(layout.next_reg, 0);
  UT_ASSERT_EQ(layout.next_stack_off, 0);
  return 0;
}

UT_TEST(test_deinit_then_ensure_capacity_again_works)
{
  /* deinit fully zeroes the struct, so it must be safely reusable
   * afterward (no dangling "already allocated" bookkeeping left behind). */
  TCCAbiCallLayout layout;
  layout_init(&layout);
  tcc_abi_call_layout_ensure_capacity(&layout, 4);
  tcc_abi_call_layout_deinit(&layout);

  tcc_abi_call_layout_ensure_capacity(&layout, 2);
  UT_ASSERT_EQ(layout.capacity, 8);
  UT_ASSERT(layout.locs != NULL);

  tcc_abi_call_layout_deinit(&layout);
  return 0;
}
