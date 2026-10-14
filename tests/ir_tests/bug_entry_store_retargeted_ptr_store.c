/*
 *  TCC Tests - entry_store_prop: stores through a retargeted pointer copy
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* entry_store_prop forwarded r2's initializer into the final r2.y read: the
 * stores through `t` (a VAR copied from p1 and from p2) were never resolved to
 * a frame object, and an unresolved pointer store was assumed to hit none.  A
 * store through a pointer that may hold a frame address now kills the entry
 * stores of every object it may point into, and a pinned one every entry it
 * overlaps (a u64 store also kills the zero-init of its high word). */
#include <stdint.h>
#include <stdio.h>
#define NI __attribute__((noinline))
struct R { int x, y; };

NI int g16(int n)
{
  struct R r1 = {1, 2}, r2 = {3, 4};
  struct R *p1 = &r1, *p2 = &r2, *t;
  int s = 0;
  for (int i = 0; i < n; i++) { t = p1; s += t->x + t->y; t->x++; }
  for (int i = 0; i < n; i++) { t = p2; s += t->x * t->y; t->y++; }
  return s * 100 + r1.x + r2.y;
}

NI int g17(int n)
{
  struct R r1 = {1, 2}, r2 = {3, 4};
  struct R *p1 = &r1, *p2 = &r2, *t = p1;
  int s = 0;
  for (int i = 0; i < n; i++) { s += t->x; if (i == 1) t = p2; t->y += 1; }
  return s * 100 + r1.y * 10 + r2.y;
}

/* The pointer picks its target by a runtime condition, no loop. */
NI int g18(int c)
{
  struct R r1 = {1, 2}, r2 = {3, 4};
  struct R *t = &r1;
  if (c)
    t = &r2;
  t->y = 9;
  return r1.y * 10 + r2.y;
}

/* Two targets reached through a TEMP-only address chain. */
NI int g19(int c)
{
  int a[2] = {5, 6}, b[2] = {7, 8};
  int *t = c ? a : b;
  t[1] = 0;
  return a[1] * 10 + b[1];
}

/* A single-target pointer: the untouched field still reads its initializer. */
NI int g20(int v)
{
  struct R r = {1, 2};
  struct R *t = &r;
  t->x = v;
  return r.x * 10 + r.y;
}

/* A seqlock-style retry loop over a zero-initialized record of u64 counters,
 * each written through a pointer that names one field after another. */
struct S { uint64_t hit, acc; uint32_t sat; };
uint64_t g_hit = 0x100000002ull, g_acc = 0x300000004ull;
uint32_t g_sat = 5;
NI int seq(void) { static int n; return n++ & ~1; }
NI uint32_t g21(void)
{
  struct S s = {0};
  for (uint8_t i = 0; i < 8; i++)
  {
    int a = seq();
    if (a & 1)
      continue;
    uint64_t *p = &s.hit;
    *p = g_hit;
    p = &s.acc;
    *p = g_acc;
    uint32_t *q = &s.sat;
    *q = g_sat;
    if (seq() == a + 2)
      break;
  }
  struct S c = s;
  return (uint32_t)(c.hit >> 32) * 1000 + (uint32_t)(c.acc >> 32) * 100 + (uint32_t)c.hit * 10 + c.sat;
}

int main(void)
{
  printf("%d %d\n", g16(0), g16(3));
  printf("%d %d %d\n", g17(0), g17(1), g17(4));
  printf("%d %d\n", g18(0), g18(1));
  printf("%d %d\n", g19(0), g19(1));
  printf("%d\n", g20(7));
  printf("%u\n", g21());
  return 0;
}
