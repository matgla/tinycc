/*
 *  TCC IR - ssa:vrp and a parameter across asm goto edges
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* f2: ssa:vrp must not take `a == 0` from J's only IR predecessor (a JUMPIF):
 *     the asm goto also enters J with any a.
 * f6: a backward asm goto re-enters a block that reads parameter a.
 * Base 0b46ffac printed "10 1 20 10 / 52 61 51 51" at -O0 and
 * "10 1 10 10 / 51 61 51 51" at -O1/-O2/-Os. */
#include <stdio.h>
volatile int vsink;
__attribute__((noinline)) int f2(int a, int x)
{
  __asm__ goto("cmp %0, #3\n\tbeq %l[J]" : : "r"(x) : "cc" : J);
  vsink = 1;
  if (a == 0) goto J;
  return 1;
J:
  if (a == 0) return 10;
  return 20;
}
__attribute__((noinline)) int f6(int a, int x)
{
  int n = 0;
  int r;
again:
  n++;
  if (n > 3) return -n;
  r = a ? 1 : 0;
  if (r) return 50 + n;
  __asm__ goto("cmp %0, #3\n\tbeq %l[again]" : : "r"(x) : "cc" : again);
  return 60 + n;
}
int main(void)
{
  printf("%d %d %d %d / ", f2(0, 2), f2(1, 2), f2(1, 3), f2(0, 3));
  printf("%d %d %d %d\n", f6(0, 3), f6(0, 2), f6(1, 3), f6(1, 2));
  return 0;
}
