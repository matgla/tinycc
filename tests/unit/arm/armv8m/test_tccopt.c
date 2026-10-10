/*
 *  test_tccopt.c - white-box unit tests for the FP offset materialization
 *  cache and the TCCOptStats counters (source/opt/engine/fp_mat_cache.c)
 *  (build_tccopt/run_unit_tests_tccopt)
 *
 *  TCCIRState is built as a plain zero-initialized stack struct in every
 *  test below (`TCCIRState ir; memset(&ir, 0, sizeof(ir));`) -- the cache
 *  only ever touches ir->opt_fp_mat_cache, so no real IR constructor is
 *  needed.
 */

#include "tcc.h"
#include "tccopt.h"

#include "ut.h"

/* FP_MAT_CACHE_SIZE is a private #define inside tccopt.c (not exposed via
 * tccopt.h). Mirrored here as a hand-verified oracle constant -- if it ever
 * changes in tccopt.c, the LRU eviction test below needs to change with it. */
#define UT_FP_MAT_CACHE_SIZE 8

static void ut_topt_set_fp_offset_cache_flag(int v)
{
  tcc_state->opt_fp_offset_cache = (unsigned char)v;
}

/* ============================================================================
 * FP offset materialization cache
 * ============================================================================ */

UT_TEST(test_fp_mat_cache_init_fresh_allocates_and_zeroes)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  tcc_opt_fp_mat_cache_init(&ir);
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);

  TCCFPMatCache *cache = (TCCFPMatCache *)ir.opt_fp_mat_cache;
  UT_ASSERT(cache->entries != NULL);
  UT_ASSERT_EQ(cache->capacity, UT_FP_MAT_CACHE_SIZE);
  UT_ASSERT_EQ(cache->count, 0);
  UT_ASSERT_EQ(cache->access_count, 0);
  for (int i = 0; i < cache->capacity; i++)
    UT_ASSERT_EQ(cache->entries[i].valid, 0);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_reinit_resets_previously_recorded_entries)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  tcc_opt_fp_mat_cache_record(&ir, 0x10, 3);
  int reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 0x10, &reg), 1);
  UT_ASSERT_EQ(reg, 3);

  void *entries_before = ((TCCFPMatCache *)ir.opt_fp_mat_cache)->entries;

  /* tcc_opt_fp_mat_cache_init only allocates a new TCCFPMatCache/entries
   * array when ir->opt_fp_mat_cache/cache->entries is still NULL. Since both
   * are already set here, re-init reuses the existing allocations (no leak,
   * no double-alloc) but STILL clears every entry's `valid` flag and resets
   * count/access_count to 0 unconditionally. So a "re-init" silently
   * discards any previously cached offsets -- pin that as the actual,
   * current contract (whether that's the intended one is a separate
   * question for whoever wires this cache up more broadly). */
  tcc_opt_fp_mat_cache_init(&ir);

  UT_ASSERT(((TCCFPMatCache *)ir.opt_fp_mat_cache)->entries == entries_before);
  UT_ASSERT_EQ(((TCCFPMatCache *)ir.opt_fp_mat_cache)->count, 0);

  reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 0x10, &reg), 0);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_flag_disabled_lookup_and_record_are_noops)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_reset_stats();

  tcc_opt_fp_mat_cache_record(&ir, 4, 7); /* must be a no-op: flag is off */

  int reg = -1;
  int hit = tcc_opt_fp_mat_cache_lookup(&ir, 4, &reg);
  UT_ASSERT_EQ(hit, 0);
  UT_ASSERT_EQ(reg, -1); /* untouched */

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.fp_cache_hits, 0);
  UT_ASSERT_EQ(((TCCFPMatCache *)ir.opt_fp_mat_cache)->count, 0);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_record_then_lookup_hit_updates_stats)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);
  tcc_opt_reset_stats();

  tcc_opt_fp_mat_cache_record(&ir, 16, 5);

  int reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 16, &reg), 1);
  UT_ASSERT_EQ(reg, 5);

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.fp_cache_hits, 1);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_lookup_miss_on_unrecorded_offset)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  tcc_opt_fp_mat_cache_record(&ir, 8, 2);

  int reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 999, &reg), 0);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_record_updates_existing_entry_in_place)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  tcc_opt_fp_mat_cache_record(&ir, 32, 1);
  TCCFPMatCache *cache = (TCCFPMatCache *)ir.opt_fp_mat_cache;
  UT_ASSERT_EQ(cache->count, 1);

  tcc_opt_fp_mat_cache_record(&ir, 32, 9); /* same offset, new reg */
  UT_ASSERT_EQ(cache->count, 1);           /* still one entry, not two */

  int reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 32, &reg), 1);
  UT_ASSERT_EQ(reg, 9);

  int valid_count = 0;
  for (int i = 0; i < cache->capacity; i++)
    if (cache->entries[i].valid)
      valid_count++;
  UT_ASSERT_EQ(valid_count, 1);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_lru_eviction_picks_least_recently_used)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  /* Fill the cache to capacity (FP_MAT_CACHE_SIZE == 8): offsets
   * 0,4,8,...,28 land in slots 0..7 in order (each record call finds the
   * first empty slot, since access_count starts at 0 after init), with
   * last_use == 1..8 respectively (access_count increments on every
   * lookup/record call). */
  for (int i = 0; i < UT_FP_MAT_CACHE_SIZE; i++)
    tcc_opt_fp_mat_cache_record(&ir, i * 4, 100 + i);

  TCCFPMatCache *cache = (TCCFPMatCache *)ir.opt_fp_mat_cache;
  UT_ASSERT_EQ(cache->count, UT_FP_MAT_CACHE_SIZE);

  /* Touch offset 12 (slot 3, reg 103) so it becomes the most-recently-used
   * entry: its last_use jumps to the current (post-increment) access_count,
   * now ahead of every other slot. */
  int reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 12, &reg), 1);
  UT_ASSERT_EQ(reg, 103);

  /* Record one more, brand-new offset: the cache is full, so
   * tcc_opt_fp_mat_cache_record must evict a slot. Hand-traced oracle:
   * every slot is valid, so the eviction loop scans all of them for the
   * global minimum last_use. Slot 0 (offset 0) has the smallest last_use
   * (1) -- it was recorded first and never touched again -- so it must be
   * the one evicted, NOT slot 3 (just refreshed) and NOT any other slot. */
  tcc_opt_fp_mat_cache_record(&ir, 999, 200);

  reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 0, &reg), 0); /* evicted */

  reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 12, &reg), 1); /* survived (MRU) */
  UT_ASSERT_EQ(reg, 103);

  reg = -1;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 999, &reg), 1); /* newly recorded */
  UT_ASSERT_EQ(reg, 200);

  /* Every other original offset (i=1,2,4,5,6,7 -> offsets 4,8,16,20,24,28)
   * must still be present, untouched by the eviction. */
  static const int kept_offsets[] = {4, 8, 16, 20, 24, 28};
  static const int kept_regs[] = {101, 102, 104, 105, 106, 107};
  for (int i = 0; i < 6; i++)
  {
    reg = -1;
    UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, kept_offsets[i], &reg), 1);
    UT_ASSERT_EQ(reg, kept_regs[i]);
  }

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_invalidate_reg_clears_matching_entries_only)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  tcc_opt_fp_mat_cache_record(&ir, 100, 5);
  tcc_opt_fp_mat_cache_record(&ir, 200, 5); /* same phys_reg, different offset */
  tcc_opt_fp_mat_cache_record(&ir, 300, 6); /* different reg entirely */

  tcc_opt_fp_mat_cache_invalidate_reg(&ir, 5);

  int reg;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 100, &reg), 0);
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 200, &reg), 0);
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 300, &reg), 1);
  UT_ASSERT_EQ(reg, 6);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_clear_resets_count_keeps_array_then_free_is_safe)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);

  tcc_opt_fp_mat_cache_record(&ir, 1, 1);
  tcc_opt_fp_mat_cache_record(&ir, 2, 2);

  TCCFPMatCache *cache = (TCCFPMatCache *)ir.opt_fp_mat_cache;
  void *entries_before = cache->entries;
  UT_ASSERT(cache->count > 0);

  tcc_opt_fp_mat_cache_clear(&ir);

  UT_ASSERT_EQ(cache->count, 0);
  UT_ASSERT(cache->entries == entries_before); /* array kept, not freed */
  for (int i = 0; i < cache->capacity; i++)
    UT_ASSERT_EQ(cache->entries[i].valid, 0);

  int reg;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 1, &reg), 0);

  /* free() after clear() must not double-free cache->entries. */
  tcc_opt_fp_mat_cache_free(&ir);
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  ut_topt_set_fp_offset_cache_flag(0);
  return 0;
}

UT_TEST(test_fp_mat_cache_free_then_reinit_is_clean)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);
  tcc_opt_fp_mat_cache_record(&ir, 5, 5);

  tcc_opt_fp_mat_cache_free(&ir);
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  tcc_opt_fp_mat_cache_init(&ir);
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);
  TCCFPMatCache *cache = (TCCFPMatCache *)ir.opt_fp_mat_cache;
  UT_ASSERT_EQ(cache->count, 0);

  int reg;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 5, &reg), 0);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_null_ir_is_safe_noop)
{
  int reg = 123;
  tcc_opt_fp_mat_cache_init(NULL);
  tcc_opt_fp_mat_cache_clear(NULL);
  tcc_opt_fp_mat_cache_free(NULL);
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(NULL, 4, &reg), 0);
  UT_ASSERT_EQ(reg, 123); /* untouched */
  tcc_opt_fp_mat_cache_record(NULL, 4, 5);      /* must not crash */
  tcc_opt_fp_mat_cache_invalidate_reg(NULL, 5); /* must not crash */
  return 0;
}

UT_TEST(test_fp_mat_cache_lookup_null_phys_reg_is_safe_noop)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);
  tcc_opt_fp_mat_cache_record(&ir, 7, 7);

  int hit = tcc_opt_fp_mat_cache_lookup(&ir, 7, NULL);
  UT_ASSERT_EQ(hit, 0);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_fp_mat_cache_uninitialized_cache_is_safe_noop)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  tcc_opt_fp_mat_cache_clear(&ir); /* no crash, no alloc */
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  tcc_opt_fp_mat_cache_free(&ir); /* no crash */
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  int reg = 55;
  ut_topt_set_fp_offset_cache_flag(1);
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 1, &reg), 0);
  UT_ASSERT_EQ(reg, 55);

  tcc_opt_fp_mat_cache_record(&ir, 1, 2); /* no crash, no alloc */
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  tcc_opt_fp_mat_cache_invalidate_reg(&ir, 2); /* no crash */

  ut_topt_set_fp_offset_cache_flag(0);
  return 0;
}

/* ============================================================================
 * Statistics
 *
 * opt_stats is a single process-wide static global inside tccopt.c, NOT
 * scoped per-TCCIRState. Every test below calls tcc_opt_reset_stats() first
 * for isolation from whatever earlier tests in this binary did.
 * ============================================================================ */

UT_TEST(test_stats_reset_zeroes_all_fields)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);
  tcc_opt_fp_mat_cache_record(&ir, 1, 1);
  int reg;
  tcc_opt_fp_mat_cache_lookup(&ir, 1, &reg); /* bumps fp_cache_hits */

  tcc_opt_reset_stats();

  TCCOptStats stats;
  memset(&stats, 0xAA, sizeof(stats)); /* poison, confirm real overwrite below */
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.dce_removed, 0);
  UT_ASSERT_EQ(stats.const_folded, 0);
  UT_ASSERT_EQ(stats.cse_eliminated, 0);
  UT_ASSERT_EQ(stats.copies_propagated, 0);
  UT_ASSERT_EQ(stats.fp_cache_hits, 0);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_stats_get_stats_null_is_safe_noop)
{
  tcc_opt_reset_stats();
  tcc_opt_get_stats(NULL); /* contract-lock: guarded by `if (stats)`, must not crash */
  return 0;
}

UT_TEST(test_stats_fp_cache_hits_matches_scripted_sequence)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_opt_fp_mat_cache_init(&ir);
  ut_topt_set_fp_offset_cache_flag(1);
  tcc_opt_reset_stats();

  tcc_opt_fp_mat_cache_record(&ir, 40, 4);
  tcc_opt_fp_mat_cache_record(&ir, 44, 5);

  int reg;
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 40, &reg), 1); /* hit #1 */
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 44, &reg), 1); /* hit #2 */
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 48, &reg), 0); /* miss, no count */
  UT_ASSERT_EQ(tcc_opt_fp_mat_cache_lookup(&ir, 40, &reg), 1); /* hit #3 */

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.fp_cache_hits, 3);

  ut_topt_set_fp_offset_cache_flag(0);
  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}
