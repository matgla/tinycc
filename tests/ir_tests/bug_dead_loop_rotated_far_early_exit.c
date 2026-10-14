/*
 * ssa:dead_loop rotated arm: a pure counting loop whose body leaves early to a
 * block past detect_loops' 50-instruction reach lost its back-edge, so only
 * j == 1 was checked.  The arm now refuses any exit but the latch fall-through.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

struct term
{
  int base, off;
};

/* The self-hosted ssa_opt_load_combine shape: an inlined helper whose
 * consecutive-offset check returns early, with a long tail after it. */
static int run_ok(struct term **bs, int n, int i, int *out)
{
  int base = bs[0]->base, off0 = bs[0]->off;
  for (int j = 1; j < n; j++)
    if (bs[j]->base != base || bs[j]->off != off0 + j)
      return 0;
  int a = out[0] * 3 + i, b = out[1] ^ a, d = out[2] - b;
  out[3] += a * b + d;
  out[4] ^= (a << 3) | (b >> 2);
  out[5] += d * 7 - a;
  out[6] -= b + d * a;
  out[7] = out[3] + out[4] * out[5];
  out[8] += out[6] ^ out[7];
  out[9] = a + b + d + i;
  return 1;
}

__attribute__((noinline)) int count_runs(struct term **bs, int n, int k, int *out)
{
  int c = 0;
  for (int i = 0; i < k; i++)
    if (run_ok(bs, n - i, i, out))
      out[10 + c++] = i;
  return c;
}

/* The same loop as a goto to a far label, without inlining. */
__attribute__((noinline)) int first_bad(struct term **bs, int n, int *out)
{
  int base = bs[0]->base, off0 = bs[0]->off;
  for (int j = 1; j < n; j++)
    if (bs[j]->base != base || bs[j]->off != off0 + j)
      goto bad;
  out[0] += n;
  out[1] ^= out[0] * 5;
  out[2] -= out[1] + n;
  out[3] += out[2] * out[1];
  out[4] = out[3] ^ (out[2] << 2);
  out[5] += out[4] - out[0];
  out[6] = out[5] * 3 + out[4];
  out[7] ^= out[6] + n;
  out[8] = out[7] - out[1];
  out[9] += out[8] * 7;
  return 1;
bad:
  return -1;
}

/* Must still go: a pure counting loop with no early exit. */
__attribute__((noinline)) int pure_tail(int n, int *out)
{
  int x = 0;
  for (int j = 0; j < n; j++)
    x = 5;
  out[0] = x;
  return n;
}

int main(void)
{
  struct term t[4] = {{7, 0}, {7, 1}, {7, 5}, {7, 3}};
  struct term u[4] = {{7, 0}, {7, 1}, {7, 2}, {8, 3}};
  struct term *p[4] = {&t[0], &t[1], &t[2], &t[3]};
  struct term *q[4] = {&u[0], &u[1], &u[2], &u[3]};
  int o[16] = {0};
  int c = count_runs(p, 4, 4, o);
  printf("count_runs t: %d [%d %d]\n", c, o[10], o[11]);
  int o2[16] = {0};
  c = count_runs(q, 4, 4, o2);
  printf("count_runs u: %d [%d %d %d]\n", c, o2[10], o2[11], o2[12]);
  int o3[16] = {0};
  printf("first_bad: %d %d %d %d\n", first_bad(p, 4, o3), first_bad(p, 2, o3), first_bad(q, 4, o3),
         first_bad(q, 3, o3));
  int o4[1] = {0};
  printf("pure_tail: %d %d\n", pure_tail(3, o4), o4[0]);
  return 0;
}
