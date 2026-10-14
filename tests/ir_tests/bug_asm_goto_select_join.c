/*
 *  TCC IR - a forward asm goto into a join
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A forward asm goto into a join from a path of its own: the value the join
 * tests must survive that edge (the RA's liveness and ra:branch_thread see the
 * edge).  Base 0b46ffac printed "select 10 jumpif 40 cas 60" at every level;
 * fixed by recording asm goto labels as CFG edges.  Also run with
 * -minline-atomics. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__arm__) || defined(__thumb__)
#define JEQ(x, k, L) __asm__ goto("cmp %0, #" #k "\n\tbeq %l[" #L "]" : : "r"(x) : "cc" : L)
#else
#define JEQ(x, k, L) __asm__ goto("cmpl $" #k ", %0\n\tje %l[" #L "]" : : "r"(x) : "cc" : L)
#endif

volatile unsigned sink;

__attribute__((noinline)) int side(int v)
{
  sink += 100;
  return v + 1;
}

/* y == 2 reaches j only through the asm goto, with a >= 7: want 20 */
__attribute__((noinline)) int ag_hidden_select(int x, int y)
{
  int a = side(x);
  if (y == 2)
    goto via_asm;
  if (a < 7)
    sink += (unsigned)a;
j:
  return a < 7 ? 10 : 20;
via_asm:
  JEQ(y, 2, j);
  return 99;
}

/* a == b through the asm goto: want 30 */
__attribute__((noinline)) int ag_hidden_jumpif(int x, int y)
{
  int a = side(x), b = side(y);
  if (x == 5)
    goto via_asm;
  if (a == b)
    sink += 1;
j:
  if (a == b)
    return 30;
  return 40;
via_asm:
  JEQ(x, 5, j);
  return 99;
}

/* the same with an inline CAS's result (correct today) */
__attribute__((noinline)) int ag_hidden_cas(uint32_t *l, uint32_t e, uint32_t d, int x)
{
  bool ok;
  if (x == 3)
  {
    ok = (e & 1) != 0;
    goto via_asm;
  }
  ok = __atomic_compare_exchange_n(l, &e, d, false, 5, 0);
  if (!ok)
    sink += e;
j:
  return ok ? 10 + (int)d : 20 + (int)e;
via_asm:
  JEQ(x, 3, j);
  return 99;
}

int main(void)
{
  uint32_t w = 40;
  int s = ag_hidden_select(7, 2);
  int t = ag_hidden_jumpif(5, 5);
  int c = ag_hidden_cas(&w, 40, 55, 3);
  printf("select %d jumpif %d cas %d\n", s, t, c);
  return 0;
}
