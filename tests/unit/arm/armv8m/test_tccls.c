/*
 *  test_tccls.c - direct unit tests for tccls.c public helpers.
 */

#include <stdint.h>

#include "tcc.h"
#include "tccir.h"
#include "tccls.h"
#include "ut.h"

#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_PARAM(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, (p))

static void ls_init(LSLiveIntervalState *ls)
{
  tcc_ls_initialize(ls);
}

/* ------------------------------------------------------------------ state */

UT_TEST(test_initialize_add_and_clear_intervals)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  UT_ASSERT(ls.intervals != NULL);
  UT_ASSERT(ls.active_set != NULL);
  UT_ASSERT_EQ(ls.intervals_size, 64);
  UT_ASSERT_EQ(ls.next_interval_index, 0);
  UT_ASSERT_EQ(ls.next_active_index, 0);
  UT_ASSERT_EQ(ls.dirty_registers, 0);
  UT_ASSERT_EQ(ls.dirty_float_registers, 0);
  UT_ASSERT_EQ(ls.live_regs_by_instruction_size, 0);
  UT_ASSERT_EQ(ls.cached_instruction_idx, -1);
  UT_ASSERT_EQ(ls.cached_live_regs, 0);

  for (int i = 0; i < 70; i++)
    tcc_ls_add_live_interval(&ls, VR_TMP(i), i, i + 2, i & 1, i & 2, LS_REG_TYPE_INT, i & 1, i % 13);

  UT_ASSERT(ls.intervals_size >= 128);
  UT_ASSERT_EQ(ls.next_interval_index, 70);
  UT_ASSERT_EQ(ls.intervals[69].vreg, VR_TMP(69));
  UT_ASSERT_EQ(ls.intervals[69].start, 69);
  UT_ASSERT_EQ(ls.intervals[69].end, 71);
  UT_ASSERT_EQ(ls.intervals[69].crosses_call, 1);
  UT_ASSERT_EQ(ls.intervals[69].addrtaken, 0);
  UT_ASSERT_EQ(ls.intervals[69].reg_type, LS_REG_TYPE_INT);
  UT_ASSERT_EQ(ls.intervals[69].lvalue, 1);
  UT_ASSERT_EQ(ls.intervals[69].r0, 4);
  UT_ASSERT_EQ(ls.intervals[69].r1, -1);
  UT_ASSERT_EQ(ls.intervals[69].stack_location, 0);

  tcc_ls_add_live_interval(&ls, VR_PARAM(0), 10, 10, 0, 0, LS_REG_TYPE_INT, 0, -1);
  UT_ASSERT(ls.intervals[70].sort_key < ls.intervals[69].sort_key);

  ls.live_regs_by_instruction = tcc_malloc(sizeof(uint32_t) * 2);
  ls.live_regs_by_instruction[0] = 1u;
  ls.live_regs_by_instruction[1] = 2u;
  ls.live_regs_by_instruction_size = 2;
  ls.next_active_index = 5;
  ls.cached_instruction_idx = 7;
  ls.cached_live_regs = 0x55;

  tcc_ls_clear_live_intervals(&ls);
  UT_ASSERT_EQ(ls.next_interval_index, 0);
  UT_ASSERT_EQ(ls.next_active_index, 0);
  UT_ASSERT_EQ(ls.live_regs_by_instruction_size, 0);
  UT_ASSERT_EQ(ls.cached_instruction_idx, -1);
  UT_ASSERT_EQ(ls.cached_live_regs, 0);
  UT_ASSERT(ls.live_regs_by_instruction == NULL);

  tcc_ls_deinitialize(&ls);
  return 0;
}

/* --------------------------------------------------------------- stack map */

UT_TEST(test_compact_stack_locations_preserves_shared_slots_and_alignment)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  tcc_ls_add_live_interval(&ls, VR_TMP(0), 0, 1, 0, 0, LS_REG_TYPE_INT, 0, -1);
  tcc_ls_add_live_interval(&ls, VR_TMP(1), 0, 1, 0, 0, LS_REG_TYPE_LLONG, 0, -1);
  tcc_ls_add_live_interval(&ls, VR_TMP(2), 0, 1, 0, 0, LS_REG_TYPE_COMPLEX_DOUBLE, 0, -1);
  tcc_ls_add_live_interval(&ls, VR_TMP(3), 0, 1, 0, 0, LS_REG_TYPE_FLOAT, 0, -1);

  ls.intervals[0].stack_location = 100;
  ls.intervals[1].stack_location = 100;
  ls.intervals[2].stack_location = 200;

  tcc_ls_compact_stack_locations(&ls, 12);

  UT_ASSERT_EQ((int)ls.intervals[0].stack_location, -8);
  UT_ASSERT_EQ((int)ls.intervals[1].stack_location, -8);
  UT_ASSERT_EQ((int)ls.intervals[2].stack_location, -32);
  UT_ASSERT_EQ((int)ls.intervals[3].stack_location, 0);

  tcc_ls_deinitialize(&ls);
  return 0;
}

UT_TEST(test_compact_stack_locations_keeps_negative_spill_base)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  tcc_ls_add_live_interval(&ls, VR_TMP(0), 0, 1, 0, 0, LS_REG_TYPE_DOUBLE, 0, -1);
  ls.intervals[0].stack_location = 44;

  tcc_ls_compact_stack_locations(&ls, -16);

  UT_ASSERT_EQ((int)ls.intervals[0].stack_location, -24);

  tcc_ls_deinitialize(&ls);
  return 0;
}

/* -------------------------------------------------------------- liveness */

UT_TEST(test_compute_live_regs_counts_integer_intervals_only)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  tcc_ls_add_live_interval(&ls, VR_TMP(0), 1, 3, 0, 0, LS_REG_TYPE_INT, 0, 0);
  tcc_ls_add_live_interval(&ls, VR_TMP(1), 2, 4, 0, 0, LS_REG_TYPE_LLONG, 0, 2);
  tcc_ls_add_live_interval(&ls, VR_TMP(2), 2, 4, 0, 0, LS_REG_TYPE_FLOAT, 0, 1);
  ls.intervals[1].r1 = 3;
  ls.intervals[2].r1 = 4;

  UT_ASSERT_EQ(tcc_ls_compute_live_regs(&ls, 0), 0);
  UT_ASSERT_EQ(tcc_ls_compute_live_regs(&ls, 2), (1u << 0) | (1u << 2) | (1u << 3));
  UT_ASSERT_EQ(tcc_ls_compute_live_regs(&ls, 4), (1u << 2) | (1u << 3));

  tcc_ls_deinitialize(&ls);
  return 0;
}

UT_TEST(test_recompute_dirty_registers_prunes_unused_callee_saved_only)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  ls.live_regs_by_instruction = tcc_malloc(sizeof(uint32_t) * 3);
  ls.live_regs_by_instruction[0] = (1u << 4);
  ls.live_regs_by_instruction[1] = (1u << 7);
  ls.live_regs_by_instruction[2] = 0;
  ls.live_regs_by_instruction_size = 3;
  ls.dirty_registers = (1ull << 0) | (1ull << 4) | (1ull << 5) | (1ull << 7) | (1ull << 12);

  tcc_ls_recompute_dirty_registers(&ls);

  UT_ASSERT_EQ(ls.dirty_registers, (1ull << 0) | (1ull << 4) | (1ull << 7) | (1ull << 12));

  tcc_ls_deinitialize(&ls);
  return 0;
}

/* --------------------------------------------------------------- scratch */

UT_TEST(test_find_free_scratch_reg_uses_interval_scan_and_cache)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  tcc_ls_add_live_interval(&ls, VR_TMP(0), 5, 8, 0, 0, LS_REG_TYPE_INT, 0, 1);
  ls.intervals[0].stack_location = 40;
  ls.live_regs_by_instruction = tcc_malloc(sizeof(uint32_t) * 10);
  for (int i = 0; i < 10; i++)
    ls.live_regs_by_instruction[i] = 0;
  ls.live_regs_by_instruction[6] = 1u << 2;
  ls.live_regs_by_instruction_size = 10;

  UT_ASSERT_EQ(tcc_ls_find_free_scratch_reg(&ls, 6, 1u << 0, 1), 3);
  UT_ASSERT_EQ(ls.cached_instruction_idx, 6);
  UT_ASSERT_EQ(ls.cached_live_regs, 1u << 1);

  tcc_ls_deinitialize(&ls);
  return 0;
}

UT_TEST(test_find_free_scratch_reg_falls_back_to_ip_lr_then_none)
{
  LSLiveIntervalState ls;
  ls_init(&ls);

  UT_ASSERT_EQ(tcc_ls_find_free_scratch_reg(&ls, 0, 0x0f, 1), 12);
  tcc_ls_reset_scratch_cache(&ls);
  UT_ASSERT_EQ(tcc_ls_find_free_scratch_reg(&ls, 0, 0x100f, 0), 14);
  tcc_ls_reset_scratch_cache(&ls);
  UT_ASSERT_EQ(tcc_ls_find_free_scratch_reg(&ls, 0, 0x500f, 0), PREG_NONE);

  tcc_ls_deinitialize(&ls);
  return 0;
}
