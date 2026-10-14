/*
 *  TCC IR - a value live through a loop keeps its register while the loop spills
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The linear scan spills whole intervals: `v` (read in the first loop and after
 * the second) held a register through the second loop, which never reads it,
 * while that loop's own `out` reloaded every iteration.  ra:loop_split now
 * splits such a value before the loop (`M <- v`, the later uses read M), so M is
 * the rescan's victim and the loop keeps its values in registers.  The yasld
 * template replay loops (`entries.len` through the GOT loop) have this shape. */
#include <stdint.h>
#include <stdio.h>

/* v: read in the first loop and after the second, never in it */
__attribute__((noinline)) uint32_t live_through(const uint32_t *a, uint32_t len, uint32_t *out, uint32_t seed)
{
  uint32_t v = len * 3u + seed, s = 0, i, j;
  for (i = 0; i < len; i++)
    s += a[i] * v + (a[i] >> (v & 7));
  uint32_t c0 = seed * 3u + 0;
  uint32_t c1 = seed * 5u + 1;
  uint32_t c2 = seed * 7u + 2;
  uint32_t c3 = seed * 9u + 3;
  for (j = 0; j < len; j++) {
    uint32_t x = a[j] + s;
    out[j] = (x + c0) ^ (x & c1) ^ (x + c2) ^ (x & c3);
  }
  return s + v;
}

/* the second loop leaves early: v is read on the early exit and on one arm after */
__attribute__((noinline)) uint32_t live_through_exits(const uint32_t *a, uint32_t len, uint32_t *out, uint32_t seed,
                                                      uint32_t stop)
{
  uint32_t v = len * 5u + seed, s = 0, i, j;
  for (i = 0; i < len; i++)
    s += a[i] * v + (a[i] >> (v & 3));
  uint32_t c0 = seed * 3u + 7;
  uint32_t c1 = seed * 5u + 1;
  uint32_t c2 = seed * 7u + 2;
  for (j = 0; j < len; j++) {
    uint32_t x = a[j] + s;
    if (x == stop) {
      j += v;
      break;
    }
    out[j] = (x + c0) ^ (x & c1) ^ (x + c2);
  }
  if (j & 1)
    return s + v + j;
  return s ^ j;
}

/* the loops sit in an outer loop: v is defined in it and read after the
 * second inner loop on every outer iteration */
__attribute__((noinline)) uint32_t live_through_nested(const uint32_t *a, uint32_t len, uint32_t *out, uint32_t seed,
                                                       uint32_t rounds)
{
  uint32_t t = 0;
  for (uint32_t r = 0; r < rounds; r++) {
    uint32_t v = len * 7u + seed + r, s = r;
    for (uint32_t i = 0; i < len; i++)
      s += a[i] * v + (a[i] >> (v & 7));
    uint32_t c0 = seed * 3u + r;
    uint32_t c1 = seed * 5u + 1;
    uint32_t c2 = seed * 7u + 2;
    uint32_t c3 = seed * 9u + 3;
    for (uint32_t j = 0; j < len; j++) {
      uint32_t x = a[j] + s;
      out[j] ^= (x + c0) ^ (x & c1) ^ (x + c2) ^ (x & c3);
    }
    t += s + v;
  }
  return t;
}

static uint32_t hash(const uint32_t *p, uint32_t n)
{
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

int main(void)
{
  uint32_t a[37], out[37];
  for (uint32_t i = 0; i < 37; i++)
    a[i] = i * 2654435761u + 12345u;

  for (uint32_t len = 0; len <= 37; len += 12) {
    for (uint32_t i = 0; i < 37; i++)
      out[i] = 0;
    uint32_t r = live_through(a, len, out, 11);
    printf("live_through len=%u r=%08x out=%08x\n", (unsigned)len, (unsigned)r, (unsigned)hash(out, 37));
  }

  for (int k = 0; k < 3; k++) {
    for (uint32_t i = 0; i < 37; i++)
      out[i] = 0;
    /* stop: never, at an entry's value, at another */
    uint32_t sall = 0;
    for (uint32_t i = 0; i < 37; i++)
      sall += a[i] * (37u * 5u + 9u) + (a[i] >> ((37u * 5u + 9u) & 3));
    uint32_t stop = k == 0 ? 1u : a[k == 1 ? 6 : 9] + sall;
    uint32_t r = live_through_exits(a, 37, out, 9, stop);
    printf("live_through_exits k=%d r=%08x out=%08x\n", k, (unsigned)r, (unsigned)hash(out, 37));
  }

  for (uint32_t rounds = 0; rounds < 4; rounds += 3) {
    for (uint32_t i = 0; i < 37; i++)
      out[i] = i;
    uint32_t r = live_through_nested(a, 37, out, 5, rounds);
    printf("live_through_nested rounds=%u r=%08x out=%08x\n", (unsigned)rounds, (unsigned)r,
           (unsigned)hash(out, 37));
  }
  return 0;
}
