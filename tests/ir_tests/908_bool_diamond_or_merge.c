/*
 *  TCC Regression - constant-arm edges jumping into a merged bool test
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/* The Zig C backend materializes `c1 || c2` as a diamond whose constant
 * arm jumps INTO the merge test while the compare arm falls through it:
 *
 *     if (c1) { t = 1; goto merge; }
 *     t = a < b;
 *   merge:
 *     if (t) ...
 *
 * (kernel-zig-version-load-path-gap: parser_validate, reserve_pages and
 * every relocation-check loop of the yaff validator carry one of these per
 * check).  bool_diamond_branch's mirrored orientation redirects the
 * constant edge straight to its branch outcome, which leaves the compare
 * arm alone at the test -- the shape setif fusion then collapses to a
 * flags branch.  The `&&` form mirrors it with the constant arm adjacent
 * to the test.  These execute every shape and pin the branch outcomes. */

#include <stdint.h>
#include <stdio.h>

/* two-check or-chain, the yaff section-table shape */
__attribute__((noinline))
static int or_merge(uint32_t off, uint32_t limit, uint32_t span)
{
  int t;
  if (off > limit)
    t = 1;
  else
    t = off > limit - span;
  if (t)
    return -1;
  return (int)(off + span);
}

/* three-check chain: constant arms of the first two checks jump into
 * later merge tests (the validate entry-check cascade) */
__attribute__((noinline))
static int or3_chain(int a, int b, int c)
{
  int t1 = a < 0;
  int t2;
  if (t1)
    t2 = 1;
  else
    t2 = b < 0;
  int t3;
  if (t2)
    t3 = 1;
  else
    t3 = c < 0;
  if (t3)
    return -1;
  return a + b + c;
}

/* and-form: constant #0 arm adjacent to the test, compare arm jumping in */
__attribute__((noinline))
static int and_merge(int a, int b)
{
  int t;
  if (a < 0)
    t = b < 0;
  else
    t = 0;
  if (t)
    return 1;
  return 0;
}

/* the bool also read as a VALUE after the merge: the redirect may not
 * skip anything the later read observes */
__attribute__((noinline))
static int or_value_read(uint32_t x, uint32_t y)
{
  int t;
  if (x == 0)
    t = 1;
  else
    t = y < 4;
  int err = t;
  if (t)
    return 100 + err;
  return err;
}

/* or-chain inside a loop: the relocation-check shape (3,072 iterations
 * on the real workload) */
__attribute__((noinline))
static int loop_or_chain(const uint32_t *tab, uint32_t n, uint32_t limit)
{
  int bad = 0;
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t v = tab[i];
    int t;
    if (v >= limit)
      t = 1;
    else
      t = v > limit - 1 - i;
    if (t)
    {
      bad++;
      continue;
    }
    bad += (int)(v & 1);
  }
  return bad;
}

int main(void)
{
  if (or_merge(5, 10, 3) != 8)
    return 1;
  if (or_merge(11, 10, 3) != -1)
    return 2;
  if (or_merge(8, 10, 3) != -1)
    return 3;
  if (or3_chain(1, 2, 3) != 6)
    return 4;
  if (or3_chain(-1, 2, 3) != -1)
    return 5;
  if (or3_chain(1, -2, 3) != -1)
    return 6;
  if (or3_chain(1, 2, -3) != -1)
    return 7;
  if (and_merge(-1, -2) != 1)
    return 8;
  if (and_merge(-1, 2) != 0)
    return 9;
  if (and_merge(1, -2) != 0)
    return 10;
  if (or_value_read(0, 9) != 101)
    return 11;
  if (or_value_read(3, 2) != 101)
    return 12;
  if (or_value_read(3, 9) != 0)
    return 13;

  static const uint32_t tab[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  if (loop_or_chain(tab, 10, 10) != 7)
    return 14;
  if (loop_or_chain(tab, 10, 8) != 8)
    return 15;

  printf("or %d %d %d\n", or_merge(5, 10, 3), or_merge(11, 10, 3), or_merge(8, 10, 3));
  printf("or3 %d %d\n", or3_chain(1, 2, 3), or3_chain(-1, 2, 3));
  printf("and %d %d\n", and_merge(-1, -2), and_merge(1, 2));
  printf("val %d %d\n", or_value_read(0, 9), or_value_read(3, 2));
  printf("loop %d %d\n", loop_or_chain(tab, 10, 10), loop_or_chain(tab, 10, 8));
  return 0;
}
