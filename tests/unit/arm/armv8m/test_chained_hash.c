/*
 *  test_chained_hash.c - suite for tcc-chained-hash.h
 *
 *  TCCChainedHash is a self-contained static-inline library that only
 *  needs tcc_malloc/realloc/free (supplied by stubs.c). All functions
 *  are exercised without any TCCState.
 */

#define USING_GLOBALS
#include "ir.h"

#include "tcc-chained-hash.h"
#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static TCCChainedHash ut_hash_new(uint32_t buckets, uint32_t capacity)
{
  TCCChainedHash h;
  tcc_chained_hash_init(&h, buckets, capacity);
  return h;
}

/* ------------------------------------------------------------------ tests */

UT_TEST(test_hash_init_state)
{
  TCCChainedHash h = ut_hash_new(8, 16);

  UT_ASSERT_EQ(h.bucket_count, 8);
  UT_ASSERT_EQ(h.bucket_mask, 7);
  UT_ASSERT_EQ(h.entry_capacity, 16);
  UT_ASSERT_EQ(h.hashed_count, 0);
  UT_ASSERT(h.buckets != NULL);
  UT_ASSERT(h.next != NULL);
  UT_ASSERT(h.hashes != NULL);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_insert_and_lookup_single)
{
  TCCChainedHash h = ut_hash_new(8, 16);

  /* Insert entry 0 with hash 0x05 */
  tcc_chained_hash_insert_head(&h, 0x05u, 0u);
  UT_ASSERT_EQ(h.hashed_count, 1);

  uint32_t slot = tcc_chained_hash_bucket_head(&h, 0x05u);
  UT_ASSERT_EQ(slot, 1u); /* slot = entry_index + 1 = 1 */
  UT_ASSERT_EQ(tcc_chained_hash_slot_to_index(slot), 0u);

  /* No next entry in the chain */
  UT_ASSERT_EQ(tcc_chained_hash_next_slot(&h, slot), 0u);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_insert_chain_same_bucket)
{
  TCCChainedHash h = ut_hash_new(8, 16);

  /* Two entries with the same bucket (hash & 7 == 3) */
  tcc_chained_hash_insert_head(&h, 3u, 0u);
  tcc_chained_hash_insert_head(&h, 11u, 1u); /* 11 & 7 == 3 */
  UT_ASSERT_EQ(h.hashed_count, 2);

  /* Head of bucket 3 should be the most recently inserted entry (slot 2) */
  uint32_t slot = tcc_chained_hash_bucket_head(&h, 3u);
  UT_ASSERT_EQ(tcc_chained_hash_slot_to_index(slot), 1u);

  /* Follow the chain — should reach entry 0 */
  uint32_t next = tcc_chained_hash_next_slot(&h, slot);
  UT_ASSERT_EQ(tcc_chained_hash_slot_to_index(next), 0u);

  /* No more entries */
  UT_ASSERT_EQ(tcc_chained_hash_next_slot(&h, next), 0u);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_entry_hash_roundtrip)
{
  TCCChainedHash h = ut_hash_new(8, 16);

  tcc_chained_hash_insert_head(&h, 0xDEADBEEFu, 2u);
  UT_ASSERT_EQ(tcc_chained_hash_entry_hash(&h, 2u), 0xDEADBEEFu);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_clear_resets_counts)
{
  TCCChainedHash h = ut_hash_new(8, 16);
  tcc_chained_hash_insert_head(&h, 1u, 0u);
  tcc_chained_hash_insert_head(&h, 2u, 1u);

  tcc_chained_hash_clear(&h);
  UT_ASSERT_EQ(h.hashed_count, 0);
  UT_ASSERT_EQ(tcc_chained_hash_bucket_head(&h, 1u), 0u);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_reserve_grows_capacity)
{
  TCCChainedHash h = ut_hash_new(8, 4);
  UT_ASSERT_EQ(h.entry_capacity, 4u);

  tcc_chained_hash_reserve(&h, 32u);
  UT_ASSERT_EQ(h.entry_capacity, 32u);

  /* Reserve with smaller value is a no-op */
  tcc_chained_hash_reserve(&h, 8u);
  UT_ASSERT_EQ(h.entry_capacity, 32u);

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_auto_rebuild_on_overflow)
{
  /* 2 buckets — will trigger rebuild after 5th insert (> 2*2) */
  TCCChainedHash h = ut_hash_new(2, 16);
  uint32_t initial_bc = h.bucket_count;

  for (uint32_t i = 0; i < 6; ++i)
    tcc_chained_hash_insert_head(&h, i, i);

  UT_ASSERT(h.bucket_count > initial_bc); /* must have grown */
  UT_ASSERT_EQ(h.hashed_count, 6);

  /* Verify all entries are still reachable via their stored hashes */
  for (uint32_t i = 0; i < 6; ++i)
  {
    uint32_t found = 0;
    uint32_t slot = tcc_chained_hash_bucket_head(&h, i);
    while (slot)
    {
      if (tcc_chained_hash_slot_to_index(slot) == i)
      {
        found = 1;
        break;
      }
      slot = tcc_chained_hash_next_slot(&h, slot);
    }
    UT_ASSERT(found);
  }

  tcc_chained_hash_destroy(&h);
  return 0;
}

UT_TEST(test_hash_destroy_nulls_pointers)
{
  TCCChainedHash h = ut_hash_new(4, 8);
  tcc_chained_hash_destroy(&h);

  UT_ASSERT(h.buckets == NULL);
  UT_ASSERT(h.next == NULL);
  UT_ASSERT(h.hashes == NULL);
  UT_ASSERT_EQ(h.bucket_count, 0);
  UT_ASSERT_EQ(h.hashed_count, 0);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(chained_hash)
{
  UT_RUN(test_hash_init_state);
  UT_RUN(test_hash_insert_and_lookup_single);
  UT_RUN(test_hash_insert_chain_same_bucket);
  UT_RUN(test_hash_entry_hash_roundtrip);
  UT_RUN(test_hash_clear_resets_counts);
  UT_RUN(test_hash_reserve_grows_capacity);
  UT_RUN(test_hash_auto_rebuild_on_overflow);
  UT_RUN(test_hash_destroy_nulls_pointers);
}
