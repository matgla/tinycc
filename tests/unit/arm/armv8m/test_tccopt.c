/*
 *  test_tccopt.c - white-box unit tests for tccopt.c
 *  (build_tccopt/run_unit_tests_tccopt)
 *
 *  tccopt.c has two genuinely distinct halves:
 *
 *   1. Live code, actually called elsewhere: tcc_opt_get_stats/
 *      tcc_opt_reset_stats (a process-wide TCCOptStats global) and the FP
 *      offset materialization cache (tcc_opt_fp_mat_cache_init/_clear/_free/
 *      _lookup/_record/_invalidate_reg), used for real by arm-thumb-gen.c.
 *      This is where the bulk of test effort below goes.
 *
 *   2. A parallel optimization-pass registry/driver (builtin_passes[],
 *      tcc_opt_register_pass, tcc_opt_get_passes, tcc_optimize_ir,
 *      tcc_opt_run_pass, tcc_opt_get_level) that is NOT wired into the real
 *      pipeline (ir/opt_pipeline.c) -- every builtin pass body except
 *      "fp-offset-cache" is a no-op placeholder. Still real, compiled,
 *      reachable code and a latent trap for a future contributor who wires
 *      it up, so it gets lighter-touch coverage of the dispatch mechanics
 *      themselves (growth, dispatch-by-flag, dispatch-by-name, the
 *      lazy-once registration).
 *
 *  TCCIRState is built as a plain zero-initialized stack struct in every
 *  test below (`TCCIRState ir; memset(&ir, 0, sizeof(ir));`) -- tccopt.c
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

/* ============================================================================
 * Optimization pass registry / driver (dead scaffold; see file header)
 *
 * IMPORTANT ORDERING CONSTRAINT: tcc_opt_get_passes()'s `static int
 * initialized` guard is a function-local static that persists for the life
 * of the whole test-binary PROCESS, not just one test or one call. The
 * very first call to tcc_opt_get_passes/tcc_opt_register_pass/
 * tcc_opt_run_pass/tcc_optimize_ir from ANYWHERE in this binary triggers the
 * one-time registration of the 4 builtin_passes[]. This suite is the only
 * thing in this binary that touches those four functions, so as long as
 * test_pass_registry_first_call_registers_four_builtins runs before every
 * other test below (enforced by UT_RUN order in the suite function), the
 * "only registers once" behavior is deliberately observed exactly once, as
 * intended, rather than assumed.
 * ============================================================================ */

static int ut_topt_custom_pass1_calls = 0;
static int ut_topt_custom_pass1_run(TCCIRState *ir)
{
  (void)ir;
  ut_topt_custom_pass1_calls++;
  return 42;
}

static const char *ut_topt_grow_names[] = {
    "grow-test-0",  "grow-test-1",  "grow-test-2",  "grow-test-3",
    "grow-test-4",  "grow-test-5",  "grow-test-6",  "grow-test-7",
    "grow-test-8",  "grow-test-9",  "grow-test-10", "grow-test-11",
    "grow-test-12", "grow-test-13",
};
static int ut_topt_grow_run(TCCIRState *ir)
{
  (void)ir;
  return 777;
}

static int ut_topt_probe_o2_calls = 0;
static int ut_topt_probe_o2_run(TCCIRState *ir)
{
  (void)ir;
  ut_topt_probe_o2_calls++;
  return 0;
}

UT_TEST(test_pass_registry_first_call_registers_four_builtins)
{
  int count = -1;
  const TCCOptPass *passes = tcc_opt_get_passes(&count);
  UT_ASSERT(passes != NULL);
  UT_ASSERT_EQ(count, 4);

  static const char *expected_names[4] = {
      "fp-offset-cache", "dce", "const-fold", "cse"};
  for (int i = 0; i < 4; i++)
    UT_ASSERT_STREQ(passes[i].name, expected_names[i]);

  /* Spot-check flags: cse is O2|OS only, fp-offset-cache is O1|O2|OS. */
  UT_ASSERT_EQ(passes[3].flags, (unsigned)(TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS));
  UT_ASSERT_EQ(passes[0].flags,
               (unsigned)(TCC_OPT_ENABLED_O1 | TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS));

  return 0;
}

UT_TEST(test_pass_registry_second_call_does_not_reregister)
{
  int count1 = -1;
  const TCCOptPass *passes1 = tcc_opt_get_passes(&count1);
  int count2 = -1;
  const TCCOptPass *passes2 = tcc_opt_get_passes(&count2);

  UT_ASSERT_EQ(count1, 4);
  UT_ASSERT_EQ(count2, 4);        /* NOT 8 -- builtins were not re-registered */
  UT_ASSERT(passes1 == passes2); /* same underlying array; no growth happened */

  return 0;
}

UT_TEST(test_pass_run_pass_known_name_dispatches_and_side_effects)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  /* "fp-offset-cache" is the one builtin pass with any real side effect: its
   * run body calls tcc_opt_fp_mat_cache_init(ir), giving an observable proxy
   * for "did this pass actually run through the by-name dispatcher". */
  int rc = tcc_opt_run_pass(&ir, "fp-offset-cache");
  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_pass_run_pass_unknown_name_and_null_args_return_zero)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));

  UT_ASSERT_EQ(tcc_opt_run_pass(&ir, "no-such-pass-xyz"), 0);
  UT_ASSERT_EQ(tcc_opt_run_pass(NULL, "dce"), 0);
  UT_ASSERT_EQ(tcc_opt_run_pass(&ir, NULL), 0);
  UT_ASSERT(ir.opt_fp_mat_cache == NULL); /* nothing ran */

  return 0;
}

UT_TEST(test_pass_registry_register_custom_pass_appends_and_dispatches)
{
  int count_before = -1;
  tcc_opt_get_passes(&count_before);

  TCCOptPass p = {0};
  p.name = "custom-test-pass-1";
  p.description = "unit test probe pass";
  p.run = ut_topt_custom_pass1_run;
  p.flags = TCC_OPT_ENABLED_O1;
  p.should_run = NULL;
  tcc_opt_register_pass(&p);

  int count_after = -1;
  const TCCOptPass *passes = tcc_opt_get_passes(&count_after);
  UT_ASSERT_EQ(count_after, count_before + 1);
  UT_ASSERT_STREQ(passes[count_after - 1].name, "custom-test-pass-1");

  int calls_before = ut_topt_custom_pass1_calls;
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  int rc = tcc_opt_run_pass(&ir, "custom-test-pass-1");
  UT_ASSERT_EQ(rc, 42);
  UT_ASSERT_EQ(ut_topt_custom_pass1_calls, calls_before + 1);

  return 0;
}

UT_TEST(test_pass_registry_register_null_pass_is_noop)
{
  int count_before = -1;
  tcc_opt_get_passes(&count_before);

  tcc_opt_register_pass(NULL);

  int count_after = -1;
  tcc_opt_get_passes(&count_after);
  UT_ASSERT_EQ(count_after, count_before);

  return 0;
}

UT_TEST(test_pass_registry_grows_capacity_past_default_16)
{
  int count_before = -1;
  tcc_opt_get_passes(&count_before);

  /* Default registry capacity is 16 (set on the very first-ever
   * tcc_opt_register_pass call in this process, during builtin
   * registration). Registering 14 more passes here is guaranteed to push
   * the running total past 16 regardless of count_before (>= 5 at this
   * point: 4 builtins + 1 prior custom pass), forcing `capacity *= 2`
   * inside tcc_opt_register_pass partway through the loop. */
  int n = (int)(sizeof(ut_topt_grow_names) / sizeof(ut_topt_grow_names[0]));
  for (int i = 0; i < n; i++)
  {
    TCCOptPass p = {0};
    p.name = ut_topt_grow_names[i];
    p.description = "grow test pass";
    p.run = ut_topt_grow_run;
    p.flags = TCC_OPT_ENABLED_O1;
    p.should_run = NULL;
    tcc_opt_register_pass(&p);
  }

  int count_after = -1;
  const TCCOptPass *passes = tcc_opt_get_passes(&count_after);
  UT_ASSERT_EQ(count_after, count_before + n);
  UT_ASSERT(count_after > 16); /* growth definitely happened */

  /* Re-fetch the pointer AFTER the grow (a realloc may have moved the
   * backing array) and verify by name/index that nothing was dropped or
   * corrupted across the reallocation -- both the first pass registered
   * before the growth-triggering insertion and the very last one. */
  UT_ASSERT_STREQ(passes[count_before].name, "grow-test-0");
  UT_ASSERT_STREQ(passes[count_after - 1].name, "grow-test-13");

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT_EQ(tcc_opt_run_pass(&ir, "grow-test-0"), 777);
  UT_ASSERT_EQ(tcc_opt_run_pass(&ir, "grow-test-13"), 777);

  return 0;
}

UT_TEST(test_pass_registry_optimize_ir_level_zero_or_negative_is_noop)
{
  TCCIRState ir_a;
  memset(&ir_a, 0, sizeof(ir_a));
  tcc_optimize_ir(&ir_a, 0);
  UT_ASSERT(ir_a.opt_fp_mat_cache == NULL);

  TCCIRState ir_b;
  memset(&ir_b, 0, sizeof(ir_b));
  tcc_optimize_ir(&ir_b, -1);
  UT_ASSERT(ir_b.opt_fp_mat_cache == NULL);

  return 0;
}

UT_TEST(test_pass_registry_optimize_ir_null_ir_is_noop)
{
  tcc_optimize_ir(NULL, 2); /* must not crash */
  return 0;
}

UT_TEST(test_pass_registry_optimize_ir_dispatches_by_level_flags)
{
  TCCOptPass p = {0};
  p.name = "probe-o2-only";
  p.description = "unit test O2/Os-only probe pass";
  p.run = ut_topt_probe_o2_run;
  p.flags = TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS;
  p.should_run = NULL;
  tcc_opt_register_pass(&p);

  ut_topt_probe_o2_calls = 0;

  TCCIRState ir_o1;
  memset(&ir_o1, 0, sizeof(ir_o1));
  tcc_optimize_ir(&ir_o1, 1);
  UT_ASSERT_EQ(ut_topt_probe_o2_calls, 0); /* O2/Os-only pass must NOT run at O1 */
  /* fp-offset-cache (O1|O2|Os) IS enabled at O1 -- observable proxy that
   * level-1 dispatch ran at all. */
  UT_ASSERT(ir_o1.opt_fp_mat_cache != NULL);

  TCCIRState ir_o2;
  memset(&ir_o2, 0, sizeof(ir_o2));
  tcc_optimize_ir(&ir_o2, 2);
  UT_ASSERT_EQ(ut_topt_probe_o2_calls, 1); /* runs at O2 */
  UT_ASSERT(ir_o2.opt_fp_mat_cache != NULL);

  tcc_opt_fp_mat_cache_free(&ir_o1);
  tcc_opt_fp_mat_cache_free(&ir_o2);
  return 0;
}

UT_TEST(test_opt_get_level_null_tcc_state_returns_zero)
{
  TCCState *saved = tcc_state;
  tcc_state = NULL;
  UT_ASSERT_EQ(tcc_opt_get_level(), 0);
  tcc_state = saved;
  return 0;
}

UT_TEST(test_opt_get_level_maps_optimize_flag)
{
  unsigned char saved = tcc_state->optimize;

  /* tcc_opt_get_level() maps the -O<n> level in s->optimize to our internal
   * levels (0, 1, 2), with -O3+ clamped to 2. */
  tcc_state->optimize = 0;
  UT_ASSERT_EQ(tcc_opt_get_level(), 0);

  tcc_state->optimize = 1;
  UT_ASSERT_EQ(tcc_opt_get_level(), 1);

  tcc_state->optimize = 2;
  UT_ASSERT_EQ(tcc_opt_get_level(), 2);

  tcc_state->optimize = 3;
  UT_ASSERT_EQ(tcc_opt_get_level(), 2);

  tcc_state->optimize = saved;
  return 0;
}

UT_TEST(test_pass_cse_is_noop_and_null_safe)
{
  tcc_opt_reset_stats();
  UT_ASSERT_EQ(tcc_opt_cse(NULL), 0);

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT_EQ(tcc_opt_cse(&ir), 0);

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.cse_eliminated, 0);
  return 0;
}


UT_TEST(test_pass_dead_code_elimination_is_noop_and_null_safe)
{
  tcc_opt_reset_stats();
  UT_ASSERT_EQ(tcc_opt_dead_code_elimination(NULL), 0);

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT_EQ(tcc_opt_dead_code_elimination(&ir), 0);

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.dce_removed, 0);
  return 0;
}

UT_TEST(test_pass_constant_folding_is_noop_and_null_safe)
{
  tcc_opt_reset_stats();
  UT_ASSERT_EQ(tcc_opt_constant_folding(NULL), 0);

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT_EQ(tcc_opt_constant_folding(&ir), 0);

  TCCOptStats stats;
  tcc_opt_get_stats(&stats);
  UT_ASSERT_EQ(stats.const_folded, 0);
  return 0;
}

UT_TEST(test_pass_fp_offset_caching_initializes_cache)
{
  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  UT_ASSERT(ir.opt_fp_mat_cache == NULL);

  int rc = tcc_opt_fp_offset_caching(&ir);
  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_pass_registry_get_passes_null_count_returns_pointer)
{
  const TCCOptPass *passes = tcc_opt_get_passes(NULL);
  UT_ASSERT(passes != NULL);
  return 0;
}

UT_TEST(test_pass_registry_optimize_ir_level_three_maps_to_o2)
{
  ut_topt_probe_o2_calls = 0;

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_optimize_ir(&ir, 3);

  UT_ASSERT_EQ(ut_topt_probe_o2_calls, 1); /* O2/Os-only probe runs at level 3 */
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}

UT_TEST(test_pass_registry_optimize_ir_level_above_three_falls_back_to_o1)
{
  ut_topt_probe_o2_calls = 0;

  TCCIRState ir;
  memset(&ir, 0, sizeof(ir));
  tcc_optimize_ir(&ir, 99);

  /* Default branch treats unknown levels as O1, so O2-only passes must NOT run
   * but O1-enabled passes (fp-offset-cache) must still run. */
  UT_ASSERT_EQ(ut_topt_probe_o2_calls, 0);
  UT_ASSERT(ir.opt_fp_mat_cache != NULL);

  tcc_opt_fp_mat_cache_free(&ir);
  return 0;
}
