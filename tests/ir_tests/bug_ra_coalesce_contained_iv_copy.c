/*
 *  TCC IR - loop IV copy coalesced under high register pressure regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ra_coalesce_graph's Briggs test refused `i <- i+1` when 13+ values stay live
 * across the loop, leaving `adds rT, rI, #1; mov rI, rT` per iteration.  A class
 * whose extent lies inside the other's cannot raise linear-scan pressure, so
 * that merge now skips the test.  The keep_old / exit_both loops pin
 * interfering IV copies, which must stay apart. */
#include <stdio.h>
#include <stdbool.h>

static unsigned sunk[8];
static int nsunk;

__attribute__((noinline)) void sink(unsigned v)
{
  sunk[nsunk++ & 7] = v;
}

#define PRESSURE_DECL(o, c)                                                                         \
  unsigned a0 = (c) ? (o)[0] : (o)[20], a1 = (c) ? (o)[1] : (o)[21], a2 = (c) ? (o)[2] : (o)[22];  \
  unsigned a3 = (c) ? (o)[3] : (o)[23], a4 = (c) ? (o)[4] : (o)[24], a5 = (c) ? (o)[5] : (o)[25];  \
  unsigned a6 = (c) ? (o)[6] : (o)[26], a7 = (c) ? (o)[7] : (o)[27], a8 = (c) ? (o)[8] : (o)[28];  \
  unsigned a9 = (c) ? (o)[9] : (o)[29], a10 = (c) ? (o)[10] : (o)[30];                             \
  unsigned a11 = (c) ? (o)[11] : (o)[31], a12 = (c) ? (o)[12] : (o)[32];                           \
  unsigned a13 = (c) ? (o)[13] : (o)[33]

#define PRESSURE_SUM                                                                                \
  (a0 + a1 * 3 + a2 * 5 + a3 * 7 + a4 * 9 + a5 * 11 + a6 * 13 + a7 * 15 + a8 * 17 + a9 * 19 +      \
   a10 * 21 + a11 * 23 + a12 * 25 + a13 * 27)

/* Zig C-backend shape: header test through a bool diamond, IV live past a call. */
__attribute__((noinline)) unsigned scan_fwd(const unsigned char *p, unsigned n, unsigned start,
                                            const unsigned *o, int c)
{
  PRESSURE_DECL(o, c);
  unsigned t2, t6;
  bool t9, t10;
  t2 = start;
loop:
  t9 = t2 < n;
  if (t9) {
    t9 = p[t2] != '/';
    t10 = t9;
    goto blk2;
  }
  t10 = false;
blk2:
  if (t10) {
    t6 = t2;
    t6 = t6 + 1;
    t2 = t6;
    goto loop;
  }
  sink(t2);
  return t2 * 1000 + PRESSURE_SUM;
}

/* Decrementing IV: `i <- i-1` is the same copy. */
__attribute__((noinline)) unsigned scan_back(const unsigned char *p, unsigned start, const unsigned *o,
                                             int c)
{
  PRESSURE_DECL(o, c);
  unsigned i = start, t;
loop:
  if (i == 0)
    goto done;
  if (p[i - 1] == '/')
    goto done;
  t = i;
  t = t - 1;
  i = t;
  goto loop;
done:
  sink(i);
  return i * 1000 + PRESSURE_SUM;
}

/* peekNext: skip separators, then scan the name from where the first loop stopped. */
__attribute__((noinline)) unsigned scan_two(const unsigned char *p, unsigned n, unsigned start,
                                            const unsigned *o, int c)
{
  PRESSURE_DECL(o, c);
  unsigned b = start, e, t;
l1:
  if (b < n && p[b] == '/') {
    t = b;
    t = t + 1;
    b = t;
    goto l1;
  }
  e = b;
l2:
  if (e < n && p[e] != '/') {
    t = e;
    t = t + 1;
    e = t;
    goto l2;
  }
  sink(b);
  sink(e);
  return (e - b) * 100000 + b * 1000 + PRESSURE_SUM;
}

/* The pre-increment value is read after the bump: old and new interfere. */
__attribute__((noinline)) unsigned scan_keep_old(const unsigned char *p, unsigned n, const unsigned *o,
                                                 int c)
{
  PRESSURE_DECL(o, c);
  unsigned i = 0, prev, nx, acc = 0;
loop:
  if (i >= n)
    goto done;
  prev = i;
  nx = i + 1;
  i = nx;
  acc += prev * p[prev] + i;
  goto loop;
done:
  sink(i);
  return acc * 7 + i + PRESSURE_SUM;
}

/* Exit after the bump with both the old and the new value live past the loop. */
__attribute__((noinline)) unsigned scan_exit_both(const unsigned char *p, unsigned n, const unsigned *o,
                                                  int c)
{
  PRESSURE_DECL(o, c);
  unsigned i = 0, j = 0;
  if (n == 0)
    return PRESSURE_SUM;
  for (;;) {
    j = i + 1;
    if (p[i] == '/')
      break;
    if (j >= n)
      break;
    i = j;
  }
  sink(i);
  sink(j);
  return i * 100000 + j * 1000 + PRESSURE_SUM;
}

static const char *const paths[] = {"/usr/lib/libc.so", "//a///bb//", "rel/x", "/", "", "single", "a/b/c/d/e/f/g/h"};
static const unsigned starts[] = {0, 1, 2, 4, 5, 9, 16, 40};

int main(void)
{
  unsigned o[34];
  for (unsigned k = 0; k < 34; k++)
    o[k] = k * 37u + 11u;
  for (unsigned pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
    const unsigned char *p = (const unsigned char *)paths[pi];
    unsigned n = 0;
    while (p[n])
      n++;
    printf("'%s':", paths[pi]);
    for (unsigned si = 0; si < sizeof(starts) / sizeof(starts[0]); si++) {
      unsigned s = starts[si];
      int c = (int)((pi + si) & 1);
      unsigned f = scan_fwd(p, n, s, o, c);
      unsigned b = s <= n ? scan_back(p, s, o, c) : 0;
      unsigned t = s <= n ? scan_two(p, n, s, o, c) : 0;
      printf(" %u:%u/%u/%u", s, f, b, t);
    }
    unsigned k = scan_keep_old(p, n, o, (int)(pi & 1));
    unsigned x = scan_exit_both(p, n, o, (int)(pi & 1));
    printf(" k=%u x=%u", k, x);
    printf(" sunk=%u,%u\n", sunk[(nsunk - 2) & 7], sunk[(nsunk - 1) & 7]);
  }
  return 0;
}
