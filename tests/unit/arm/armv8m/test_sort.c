/*
 *  TCC Memory Utilities - tcc_qsort unit tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "tcc.h"

#include "ut.h"

typedef struct SortItem
{
  int key;
  int seq;
  char pad[37];
} SortItem;

static int sort_item_cmp(const void *a, const void *b)
{
  const SortItem *x = a, *y = b;
  return (x->key > y->key) - (x->key < y->key);
}

static int sort_int_cmp(const void *a, const void *b)
{
  return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static unsigned sort_rand(unsigned *state)
{
  *state = *state * 1103515245u + 12345u;
  return *state >> 8;
}

#define SORT_MAX 4099

static SortItem got[SORT_MAX], want[SORT_MAX];

/* Fills n items with keys in [0, keys) and checks tcc_qsort against a stable insertion sort. */
static int sort_check_random(unsigned seed, int n, int keys)
{
  int failed = 0;

  for (int i = 0; i < n; i++)
  {
    got[i].key = (int)(sort_rand(&seed) % (unsigned)keys);
    got[i].seq = i;
    memset(got[i].pad, i & 0x7f, sizeof(got[i].pad));
  }
  memcpy(want, got, sizeof(SortItem) * n);
  for (int i = 1; i < n; i++)
  {
    SortItem v = want[i];
    int j = i;
    for (; j > 0 && want[j - 1].key > v.key; j--)
      want[j] = want[j - 1];
    want[j] = v;
  }

  tcc_qsort(got, (size_t)n, sizeof(SortItem), sort_item_cmp);
  for (int i = 0; i < n && !failed; i++)
    if (got[i].key != want[i].key || got[i].seq != want[i].seq || got[i].pad[0] != (char)(got[i].seq & 0x7f))
      failed = 1;
  return failed;
}

UT_TEST(test_tcc_qsort_stable_on_ties)
{
  static const int sizes[] = {0, 1, 2, 3, 19, 20, 21, 39, 40, 41, 64, 100, 257, 1000, SORT_MAX};
  static const int keys[] = {1, 2, 3, 7, 50, 100000};

  for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
      for (unsigned seed = 1; seed <= 3; seed++)
        UT_ASSERT_EQ(sort_check_random(seed * 7919u + s * 31u + k, sizes[s], keys[k]), 0);
  return 0;
}

UT_TEST(test_tcc_qsort_presorted_and_reversed)
{
  int a[300], b[300];

  for (int i = 0; i < 300; i++)
  {
    a[i] = i;
    b[i] = 299 - i;
  }
  tcc_qsort(a, 300, sizeof(int), sort_int_cmp);
  tcc_qsort(b, 300, sizeof(int), sort_int_cmp);
  for (int i = 0; i < 300; i++)
  {
    UT_ASSERT_EQ(a[i], i);
    UT_ASSERT_EQ(b[i], i);
  }
  tcc_qsort(NULL, 0, sizeof(int), sort_int_cmp);
  return 0;
}
