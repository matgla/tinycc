/*
 *  TCC Memory Utilities - Stable in-place array sort
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "tcc.h"

/* Insertion-sorted blocks merged by SymMerge (Kim & Kutzner): stable, no allocation. */
typedef struct TccSort
{
  char *base;
  size_t size;
  int (*cmp)(const void *, const void *);
} TccSort;

static int tcc_sort_less(const TccSort *s, size_t i, size_t j)
{
  return s->cmp(s->base + i * s->size, s->base + j * s->size) < 0;
}

static void tcc_sort_swap_range(const TccSort *s, size_t a, size_t b, size_t n)
{
  char tmp[64];
  size_t bytes = n * s->size;
  char *x = s->base + a * s->size, *y = s->base + b * s->size;
  while (bytes)
  {
    size_t k = bytes < sizeof(tmp) ? bytes : sizeof(tmp);
    memcpy(tmp, x, k);
    memcpy(x, y, k);
    memcpy(y, tmp, k);
    x += k;
    y += k;
    bytes -= k;
  }
}

static void tcc_sort_rotate(const TccSort *s, size_t a, size_t m, size_t b)
{
  size_t i = m - a, j = b - m;
  while (i != j)
  {
    if (i > j)
    {
      tcc_sort_swap_range(s, m - i, m, j);
      i -= j;
    }
    else
    {
      tcc_sort_swap_range(s, m - i, m + j - i, i);
      j -= i;
    }
  }
  tcc_sort_swap_range(s, m - i, m, i);
}

static void tcc_sort_sym_merge(const TccSort *s, size_t a, size_t m, size_t b)
{
  size_t i, j, k, mid, n, start, r, end;
  if (m - a == 1)
  {
    for (i = m, j = b; i < j;)
    {
      size_t h = i + (j - i) / 2;
      if (tcc_sort_less(s, h, a))
        i = h + 1;
      else
        j = h;
    }
    for (k = a; k + 1 < i; k++)
      tcc_sort_swap_range(s, k, k + 1, 1);
    return;
  }
  if (b - m == 1)
  {
    for (i = a, j = m; i < j;)
    {
      size_t h = i + (j - i) / 2;
      if (!tcc_sort_less(s, m, h))
        i = h + 1;
      else
        j = h;
    }
    for (k = m; k > i; k--)
      tcc_sort_swap_range(s, k, k - 1, 1);
    return;
  }
  mid = a + (b - a) / 2;
  n = mid + m;
  if (m > mid)
  {
    start = n - b;
    r = mid;
  }
  else
  {
    start = a;
    r = m;
  }
  while (start < r)
  {
    size_t c = start + (r - start) / 2;
    if (!tcc_sort_less(s, n - 1 - c, c))
      start = c + 1;
    else
      r = c;
  }
  end = n - start;
  if (start < m && m < end)
    tcc_sort_rotate(s, start, m, end);
  if (a < start && start < mid)
    tcc_sort_sym_merge(s, a, start, mid);
  if (mid < end && end < b)
    tcc_sort_sym_merge(s, mid, end, b);
}

static void tcc_sort_insertion(const TccSort *s, size_t a, size_t b)
{
  for (size_t i = a + 1; i < b; i++)
    for (size_t j = i; j > a && tcc_sort_less(s, j, j - 1); j--)
      tcc_sort_swap_range(s, j, j - 1, 1);
}

ST_FUNC void tcc_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
  enum { BLOCK = 20 };
  TccSort s = {(char *)base, size, cmp};
  size_t a, b, block;
  for (a = 0; a + BLOCK <= n; a += BLOCK)
    tcc_sort_insertion(&s, a, a + BLOCK);
  if (a < n)
    tcc_sort_insertion(&s, a, n);
  for (block = BLOCK; block < n; block *= 2)
  {
    for (a = 0, b = 2 * block; b <= n; a = b, b += 2 * block)
      tcc_sort_sym_merge(&s, a, a + block, b);
    if (a + block < n)
      tcc_sort_sym_merge(&s, a, a + block, n);
  }
}
