/*
 *  TCC Memory Utilities - Bitspan word-op unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/bitspan.h"

#include "ut.h"

UT_TEST(test_bitspan_zero_and_copy)
{
  uint64_t source[3] = {0x1122334455667788ull, 0xffffffffffffffffull, 0x1ull};
  uint64_t destination[3] = {~0ull, ~0ull, ~0ull};

  tcc_bitspan_zero(destination, 3);
  UT_ASSERT_EQ(destination[0], 0);
  UT_ASSERT_EQ(destination[1], 0);
  UT_ASSERT_EQ(destination[2], 0);

  tcc_bitspan_copy(destination, source, 3);
  UT_ASSERT_EQ(destination[0], (long long)source[0]);
  UT_ASSERT_EQ(destination[1], (long long)source[1]);
  UT_ASSERT_EQ(destination[2], (long long)source[2]);
  return 0;
}

UT_TEST(test_bitspan_or_accumulates)
{
  uint64_t destination[2] = {0x0f0full, 0x0ull};
  uint64_t source[2] = {0xf0f0ull, 0xaaaaull};

  tcc_bitspan_or(destination, source, 2);
  UT_ASSERT_EQ(destination[0], 0xffffull);
  UT_ASSERT_EQ(destination[1], 0xaaaaull);
  return 0;
}

UT_TEST(test_bitspan_or_andnot_computes_transfer_and_change)
{
  /* dst = gen | (in & ~kill) */
  uint64_t destination[2] = {0, 0};
  uint64_t gen[2] = {0x1ull, 0x0ull};
  uint64_t in[2] = {0x6ull, 0xffull};
  uint64_t kill[2] = {0x4ull, 0x0full};

  int changed = tcc_bitspan_or_andnot(destination, gen, in, kill, 2);
  UT_ASSERT_EQ(changed, 1);
  /* word0: 0x1 | (0x6 & ~0x4) = 0x1 | 0x2 = 0x3 */
  UT_ASSERT_EQ(destination[0], 0x3ull);
  /* word1: 0x0 | (0xff & ~0x0f) = 0xf0 */
  UT_ASSERT_EQ(destination[1], 0xf0ull);

  /* Re-running with the same inputs must report no change (fixpoint reached). */
  changed = tcc_bitspan_or_andnot(destination, gen, in, kill, 2);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(destination[0], 0x3ull);
  UT_ASSERT_EQ(destination[1], 0xf0ull);
  return 0;
}

UT_TEST(test_bitspan_and_popcount)
{
  uint64_t a[2] = {0xffull, 0xf0ull};
  uint64_t b[2] = {0x0full, 0xffull};

  /* popcount(0x0f) + popcount(0xf0) = 4 + 4 */
  UT_ASSERT_EQ(tcc_bitspan_and_popcount(a, b, 2), 8);

  uint64_t zero[2] = {0, 0};
  UT_ASSERT_EQ(tcc_bitspan_and_popcount(a, zero, 2), 0);
  return 0;
}

UT_TEST(test_bitspan_per_bit_set_test_reset)
{
  uint64_t words[3] = {0, 0, 0};

  tcc_bitspan_set(words, 0);
  tcc_bitspan_set(words, 64);
  tcc_bitspan_set(words, 129);
  UT_ASSERT(tcc_bitspan_test(words, 0));
  UT_ASSERT(tcc_bitspan_test(words, 64));
  UT_ASSERT(tcc_bitspan_test(words, 129));
  UT_ASSERT(!tcc_bitspan_test(words, 1));
  UT_ASSERT_EQ(words[0], 0x1ull);
  UT_ASSERT_EQ(words[1], 0x1ull);
  UT_ASSERT_EQ(words[2], (uint64_t)1 << 1);

  tcc_bitspan_reset(words, 64);
  UT_ASSERT(!tcc_bitspan_test(words, 64));
  UT_ASSERT_EQ(words[1], 0);
  return 0;
}

UT_TEST(test_bitspan_for_each_set_visits_ascending)
{
  uint64_t words[2] = {0, 0};
  int visited[8];
  int count = 0;

  tcc_bitspan_set(words, 0);
  tcc_bitspan_set(words, 5);
  tcc_bitspan_set(words, 63);
  tcc_bitspan_set(words, 64);
  tcc_bitspan_set(words, 100);

  tcc_bitspan_for_each_set(words, 2, 128, index) {
    visited[count++] = index;
  }

  UT_ASSERT_EQ(count, 5);
  UT_ASSERT_EQ(visited[0], 0);
  UT_ASSERT_EQ(visited[1], 5);
  UT_ASSERT_EQ(visited[2], 63);
  UT_ASSERT_EQ(visited[3], 64);
  UT_ASSERT_EQ(visited[4], 100);
  return 0;
}

UT_TEST(test_bitspan_for_each_set_respects_limit)
{
  uint64_t words[2] = {0, 0};
  int count = 0;
  int max_seen = -1;

  tcc_bitspan_set(words, 3);
  tcc_bitspan_set(words, 70);  /* >= limit, must be skipped */
  tcc_bitspan_set(words, 90);  /* >= limit, must be skipped */

  tcc_bitspan_for_each_set(words, 2, 64, index) {
    count++;
    if (index > max_seen) {
      max_seen = index;
    }
  }

  UT_ASSERT_EQ(count, 1);
  UT_ASSERT_EQ(max_seen, 3);
  return 0;
}

UT_TEST(test_bitspan_for_each_set_empty)
{
  uint64_t words[2] = {0, 0};
  int count = 0;

  tcc_bitspan_for_each_set(words, 2, 128, index) {
    (void)index;
    count++;
  }
  UT_ASSERT_EQ(count, 0);
  return 0;
}
