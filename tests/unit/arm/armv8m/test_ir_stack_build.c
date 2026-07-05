/*
 *  test_ir_stack_build.c - suite for ir/stack.c's tcc_ir_stack_build() and
 *  the offset-hash-backed slot lookups / spill-cache wrappers that
 *  test_ir_stack.c and test_ir_stack_extra.c don't reach.
 *
 *  test_ir_stack.c covers the empty-state queries, register assignment, and
 *  legacy wrappers via manually-populated TCCStackLayout structs.
 *  test_ir_stack_extra.c covers frame-size arithmetic and the 64-bit spill
 *  path. Neither drives tcc_ir_stack_build() itself (the largest function in
 *  the file, ~100 lines: the offset hash table, slot-kind/size derivation
 *  from LSLiveInterval, slot sharing across intervals at the same offset,
 *  and the IRLiveInterval::stack_slot_index back-link) nor the
 *  tcc_ir_stack_spill_cache_* IR-state wrappers (untested anywhere).
 *
 *  This file builds ir->ls.intervals[] directly via the real
 *  tcc_ls_add_live_interval() allocator entry point (ir->ls is already
 *  initialized by tcc_ir_alloc()), following the same discipline used by
 *  test_opt_promote_extra.c's utb_ls_reg()/utb_ls_spill() helpers, then
 *  calls tcc_ir_stack_build() and inspects the resulting TCCStackLayout with
 *  oracle asserts (exact slot count/offset/size/alignment/kind/vreg, and the
 *  IRLiveInterval::stack_slot_index back-link).
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

#include "tccls.h"

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/* Register vreg `vr` as stack-backed at `stack_location` with a given
 * LS_REG_TYPE_* (drives the size/alignment derivation in tcc_ir_stack_build).
 * start/end/crosses_call/addrtaken/lvalue are irrelevant to stack.c, so use
 * placeholder values (mirrors utb_ls_spill() in test_opt_promote_extra.c). */
static void ls_add_stack_backed(TCCIRState *ir, int32_t vr, int reg_type, int stack_location)
{
  tcc_ls_add_live_interval(&ir->ls, vr, 0, 1000, /*crosses_call*/ 0, /*addrtaken*/ 0,
                           reg_type, /*lvalue*/ 0, /*precolored_reg*/ -1);
  ir->ls.intervals[ir->ls.next_interval_index - 1].stack_location = (uint32_t)stack_location;
}

/* Register vreg `vr` as NOT stack-backed (stack_location stays 0, the
 * default tcc_ls_add_live_interval sets). */
static void ls_add_reg_only(TCCIRState *ir, int32_t vr)
{
  tcc_ls_add_live_interval(&ir->ls, vr, 0, 1000, 0, 0, LS_REG_TYPE_INT, 0, 3);
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: empty / no-op paths                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_no_intervals_is_noop)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_build(ir);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_null_ir_no_crash)
{
  tcc_ir_stack_build(NULL);
  return 0;
}

UT_TEST(test_build_intervals_all_reg_only_produces_no_slots)
{
  /* Every interval has stack_location == 0 (register-resident) -> the
   * estimated_slots == 0 early-return path. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_var(ir);
  ls_add_reg_only(ir, v0);
  ls_add_reg_only(ir, v1);

  tcc_ir_stack_build(ir);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: slot-kind derivation from vreg type                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_var_vreg_gets_local_kind)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -8);

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);
  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->kind, TCC_STACK_SLOT_LOCAL);
  UT_ASSERT_EQ(slot->offset, -8);
  UT_ASSERT_EQ(slot->size, 4);
  UT_ASSERT_EQ(slot->alignment, 4);
  UT_ASSERT_EQ(slot->vreg, v0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_param_vreg_gets_param_spill_kind)
{
  TCCIRState *ir = tcc_ir_alloc();
  int p0 = tcc_ir_vreg_alloc_param(ir);
  ls_add_stack_backed(ir, p0, LS_REG_TYPE_INT, -4);

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);
  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->kind, TCC_STACK_SLOT_PARAM_SPILL);
  UT_ASSERT_EQ(slot->offset, -4);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_temp_vreg_gets_spill_kind)
{
  /* TEMP (and any other vreg type) fall into the `default` branch ->
   * TCC_STACK_SLOT_SPILL. */
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  ls_add_stack_backed(ir, t0, LS_REG_TYPE_INT, -12);

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);
  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->kind, TCC_STACK_SLOT_SPILL);
  UT_ASSERT_EQ(slot->offset, -12);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: size/alignment derivation from reg_type                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_llong_gets_8byte_slot)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_LLONG, -16);

  tcc_ir_stack_build(ir);

  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->size, 8);
  UT_ASSERT_EQ(slot->alignment, 8);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_double_gets_8byte_slot)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_DOUBLE, -16);

  tcc_ir_stack_build(ir);

  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->size, 8);
  UT_ASSERT_EQ(slot->alignment, 8);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_double_soft_gets_8byte_slot)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_DOUBLE_SOFT, -16);

  tcc_ir_stack_build(ir);

  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->size, 8);
  UT_ASSERT_EQ(slot->alignment, 8);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_float_gets_4byte_slot)
{
  /* LS_REG_TYPE_FLOAT is not in the {LLONG,DOUBLE,DOUBLE_SOFT} case list ->
   * falls into `default: size = 4`. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_FLOAT, -20);

  tcc_ir_stack_build(ir);

  const TCCStackSlot *slot = tcc_ir_stack_slot_by_index(ir, 0);
  UT_ASSERT(slot != NULL);
  UT_ASSERT_EQ(slot->size, 4);
  UT_ASSERT_EQ(slot->alignment, 4);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: multiple distinct slots + back-link                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_multiple_distinct_offsets_creates_multiple_slots)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_var(ir);
  int v2 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -8);
  ls_add_stack_backed(ir, v1, LS_REG_TYPE_INT, -16);
  ls_add_stack_backed(ir, v2, LS_REG_TYPE_INT, -24);

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 3);

  /* Each vreg's IRLiveInterval::stack_slot_index must point at the slot
   * with the matching offset (the back-link tcc_ir_stack_slot_by_vreg
   * relies on). */
  const TCCStackSlot *s0 = tcc_ir_stack_slot_by_vreg(ir, v0);
  const TCCStackSlot *s1 = tcc_ir_stack_slot_by_vreg(ir, v1);
  const TCCStackSlot *s2 = tcc_ir_stack_slot_by_vreg(ir, v2);
  UT_ASSERT(s0 != NULL);
  UT_ASSERT(s1 != NULL);
  UT_ASSERT(s2 != NULL);
  UT_ASSERT_EQ(s0->offset, -8);
  UT_ASSERT_EQ(s1->offset, -16);
  UT_ASSERT_EQ(s2->offset, -24);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_stack_slot_index_backlink_set)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -8);

  tcc_ir_stack_build(ir);

  IRLiveInterval *li = tcc_ir_get_live_interval(ir, v0);
  UT_ASSERT(li != NULL);
  UT_ASSERT_EQ(li->stack_slot_index, 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: shared-offset slot reuse                               */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_two_intervals_same_offset_share_one_slot)
{
  /* Two distinct vregs stack_location'd at the same offset (regalloc slot
   * sharing / coalescing) must produce exactly ONE TCCStackSlot, and both
   * vregs' stack_slot_index must resolve back to it. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -8);
  ls_add_stack_backed(ir, v1, LS_REG_TYPE_INT, -8);

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);

  const TCCStackSlot *s0 = tcc_ir_stack_slot_by_vreg(ir, v0);
  const TCCStackSlot *s1 = tcc_ir_stack_slot_by_vreg(ir, v1);
  UT_ASSERT(s0 != NULL);
  UT_ASSERT(s1 != NULL);
  UT_ASSERT(s0 == s1);
  /* First interval processed owns the slot's primary vreg. */
  UT_ASSERT_EQ(s0->vreg, v0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_build_repeated_build_resets_previous_slots)
{
  /* A second tcc_ir_stack_build() call must fully replace the layout, not
   * accumulate: the offset hash's stale keys must be reset (via
   * tcc_ir_stack_layout_reset) so an old offset that is no longer
   * stack-backed cannot be found. */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -8);
  tcc_ir_stack_build(ir);
  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);
  UT_ASSERT(tcc_ir_stack_slot_by_offset(ir, -8) != NULL);

  /* Simulate a fresh regalloc run: clear ls intervals, add a single
   * interval at a different offset, and rebuild. */
  tcc_ls_clear_live_intervals(&ir->ls);
  int v1 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v1, LS_REG_TYPE_INT, -32);
  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), 1);
  UT_ASSERT(tcc_ir_stack_slot_by_offset(ir, -32) != NULL);
  /* The stale offset -8 must no longer resolve via the hash lookup. */
  UT_ASSERT(tcc_ir_stack_slot_by_offset(ir, -8) == NULL);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_build: growth past the initial hash/slot capacity             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_build_many_slots_grows_capacity_and_hash)
{
  /* TCC_STACK_LAYOUT_INIT_CAPACITY is 16 and the hash keeps load factor
   * <= 0.5 (initial bucket count 16). 20 distinct stack-backed intervals
   * forces both tcc_ir_stack_layout_ensure_capacity's realloc path and
   * tcc_ir_stack_layout_offset_hash_ensure_capacity's rebuild-to-64 path. */
  TCCIRState *ir = tcc_ir_alloc();
  enum { N = 20 };
  int vregs[N];
  for (int i = 0; i < N; ++i)
  {
    vregs[i] = tcc_ir_vreg_alloc_var(ir);
    ls_add_stack_backed(ir, vregs[i], LS_REG_TYPE_INT, -(4 * (i + 1)));
  }

  tcc_ir_stack_build(ir);

  UT_ASSERT_EQ(tcc_ir_stack_slot_count(ir), N);
  for (int i = 0; i < N; ++i)
  {
    const TCCStackSlot *s = tcc_ir_stack_slot_by_offset(ir, -(4 * (i + 1)));
    UT_ASSERT(s != NULL);
    UT_ASSERT_EQ(s->vreg, vregs[i]);
  }

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_slot_by_offset: positive lookups + miss                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_slot_by_offset_finds_built_slot)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  ls_add_stack_backed(ir, v0, LS_REG_TYPE_INT, -40);
  tcc_ir_stack_build(ir);

  const TCCStackSlot *s = tcc_ir_stack_slot_by_offset(ir, -40);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->offset, -40);

  /* An offset that was never built is a miss. */
  UT_ASSERT(tcc_ir_stack_slot_by_offset(ir, -41) == NULL);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_slot_by_vreg: invalid-interval / out-of-range guards          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_slot_by_vreg_no_stack_slot_returns_null)
{
  /* A valid vreg with a live interval that was never stack-built (no
   * tcc_ir_stack_build call) has stack_slot_index == -1 (its tcc_mallocz'd
   * default via ir_vreg_intervals_init). */
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  UT_ASSERT(tcc_ir_stack_slot_by_vreg(ir, v0) == NULL);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_stack_spill_cache_* wrappers (untested elsewhere)                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_spill_cache_record_and_lookup_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_spill_cache_clear(ir);

  tcc_ir_stack_spill_cache_record(ir, 3, -8);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -8), 3);
  /* An offset never recorded misses. */
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -16), -1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spill_cache_clear_forgets_entries)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_spill_cache_record(ir, 2, -4);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -4), 2);

  tcc_ir_stack_spill_cache_clear(ir);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -4), -1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spill_cache_invalidate_reg_removes_entry)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_spill_cache_clear(ir);
  tcc_ir_stack_spill_cache_record(ir, 5, -12);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -12), 5);

  tcc_ir_stack_spill_cache_invalidate_reg(ir, 5);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -12), -1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spill_cache_invalidate_offset_removes_entry)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_spill_cache_clear(ir);
  tcc_ir_stack_spill_cache_record(ir, 6, -20);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -20), 6);

  tcc_ir_stack_spill_cache_invalidate_offset(ir, -20);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -20), -1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spill_cache_record_same_reg_new_offset_invalidates_old)
{
  /* tcc_ir_spill_cache_record() invalidates any existing entry for the reg
   * (and the offset) before inserting the new mapping -- a register can
   * only cache one offset at a time. */
  TCCIRState *ir = tcc_ir_alloc();
  tcc_ir_stack_spill_cache_clear(ir);
  tcc_ir_stack_spill_cache_record(ir, 4, -8);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -8), 4);

  tcc_ir_stack_spill_cache_record(ir, 4, -16);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -16), 4);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(ir, -8), -1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_spill_cache_wrappers_null_ir_no_crash)
{
  tcc_ir_stack_spill_cache_clear(NULL);
  tcc_ir_stack_spill_cache_record(NULL, 1, -4);
  UT_ASSERT_EQ(tcc_ir_stack_spill_cache_lookup(NULL, -4), -1);
  tcc_ir_stack_spill_cache_invalidate_reg(NULL, 1);
  tcc_ir_stack_spill_cache_invalidate_offset(NULL, -4);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_stack_build)
{
  UT_COVERS("tcc_ir_stack_build");
  UT_COVERS("tcc_ir_stack_spill_cache_record");
  UT_RUN(test_build_no_intervals_is_noop);
  UT_RUN(test_build_null_ir_no_crash);
  UT_RUN(test_build_intervals_all_reg_only_produces_no_slots);
  UT_RUN(test_build_var_vreg_gets_local_kind);
  UT_RUN(test_build_param_vreg_gets_param_spill_kind);
  UT_RUN(test_build_temp_vreg_gets_spill_kind);
  UT_RUN(test_build_llong_gets_8byte_slot);
  UT_RUN(test_build_double_gets_8byte_slot);
  UT_RUN(test_build_double_soft_gets_8byte_slot);
  UT_RUN(test_build_float_gets_4byte_slot);
  UT_RUN(test_build_multiple_distinct_offsets_creates_multiple_slots);
  UT_RUN(test_build_stack_slot_index_backlink_set);
  UT_RUN(test_build_two_intervals_same_offset_share_one_slot);
  UT_RUN(test_build_repeated_build_resets_previous_slots);
  UT_RUN(test_build_many_slots_grows_capacity_and_hash);
  UT_RUN(test_slot_by_offset_finds_built_slot);
  UT_RUN(test_slot_by_vreg_no_stack_slot_returns_null);
  UT_RUN(test_spill_cache_record_and_lookup_roundtrip);
  UT_RUN(test_spill_cache_clear_forgets_entries);
  UT_RUN(test_spill_cache_invalidate_reg_removes_entry);
  UT_RUN(test_spill_cache_invalidate_offset_removes_entry);
  UT_RUN(test_spill_cache_record_same_reg_new_offset_invalidates_old);
  UT_RUN(test_spill_cache_wrappers_null_ir_no_crash);
}
