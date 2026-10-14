/*
 *  TCC IR - dominating compare-with-constant facts
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:cmp_eq_prop recorded only TEMP-vs-TEMP facts, so zig.h's ctz zero guard
 * (`if (x == 0) return 32;`) survived under a loop test that had already
 * proved x != 0.  It now records `x == k` / `x != k` facts too: x==c decides
 * every x==k, x!=c only x==c (cmp_eq.c).  The entry block, its own idom,
 * takes no fact from a lone backedge. */

#include <stdint.h>
#include <stdio.h>

static uint8_t ctz32(uint32_t v)
{
  if (v == 0)
    return 32;
  return (uint8_t)__builtin_ctz(v);
}

/* zig_ctz_u32 under the `bits != 0` test of a bitmap scan. */
__attribute__((noinline)) uint32_t first_set(const uint32_t *w, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t b = w[i];
    if (b != 0)
      return i * 32 + ctz32(b);
  }
  return n * 32;
}

/* x != 5 says nothing about x == 7, and x == 5 decides both. */
__attribute__((noinline)) int classify(uint32_t x)
{
  int r = 0;
  if (x != 5)
  {
    if (x == 7)
      r |= 1;
    if (x != 7)
      r |= 2;
    if (x == 5)
      r |= 4; /* dead */
  }
  else
  {
    if (x == 5)
      r |= 8;
    if (x == 7)
      r |= 16; /* dead */
    if (x != 9)
      r |= 32;
  }
  return r;
}

/* A constant on the left, and a fact that must not cross a redefinition. */
__attribute__((noinline)) int left_const(int x, int y)
{
  int r = 0;
  if (0 == x)
  {
    if (x != 0)
      r |= 1; /* dead */
    x = y;
    if (x == 0)
      r |= 2;
  }
  if (x == 0)
    r |= 4;
  return r;
}

/* A one-block loop at function entry: the entry block is its own idom, and
 * its lone CFG pred (the backedge) is not the only way in (20021024-1). */
static volatile int spins;

__attribute__((noinline)) unsigned entry_loop_const(unsigned rop)
{
top:
  spins++;
  if (!(rop & 0x1ff))
    goto top;
  return rop & 0x1ff;
}

__attribute__((noinline)) unsigned entry_loop_pair(unsigned rop)
{
top:
  spins++;
  if ((rop & 0x1ff) == (rop >> 23))
    goto top;
  return rop >> 23;
}

int main(void)
{
  uint32_t a[3] = {0, 0, 0x100};
  uint32_t b[2] = {0, 0};
  uint32_t c[1] = {0x80000000u};
  printf("first_set %u %u %u %u\n", (unsigned)first_set(a, 3), (unsigned)first_set(b, 2), (unsigned)first_set(c, 1),
         (unsigned)first_set(a, 0));
  printf("classify %d %d %d %d\n", classify(5), classify(7), classify(9), classify(0));
  printf("left_const %d %d %d\n", left_const(0, 0), left_const(0, 3), left_const(4, 0));
  printf("entry_loop %u %u %d\n", entry_loop_const(15), entry_loop_pair((8u << 23) | 15), spins);
  return 0;
}
