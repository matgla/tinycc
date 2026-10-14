/*
 *  TCC Tests - A discarded `c && (expr, 1)` short-circuit chain
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* expr_landor leaves `c && (x, 1)` as a VT_JMP/VT_JMPI chain on the value
 * stack; vpop backpatched only VT_CMP chains, so a discarded one kept its
 * short-circuit jump unpatched ("JMP to -1").  vpop now backpatches it. */
#include <stdio.h>

int n, m;
__attribute__((noinline)) int side(void) { n++; return 1; }
__attribute__((noinline)) void d(int a) { m += a; }
__attribute__((noinline)) void v1(int c) { c && (n++, 1); }
__attribute__((noinline)) void v2(int c) { c || (side(), 0); }
__attribute__((noinline)) void v3(int c) { (void)(c && (side(), 1)); }
__attribute__((noinline)) void v4(int c) { c && side(); }
__attribute__((noinline)) void v5(int c, int a)
{
  for (int i = 0; i < 3; d(a), i++)
    n += 10;
  c && (d(a), 1);
}
__attribute__((noinline)) void v6(int c, int e) { c && e && (n++, 1); }
__attribute__((noinline)) void v7(int c, int e) { c || e || (n++, 0); }
__attribute__((noinline)) void v8(int c) { (c && (n++, 1)), m++; }
__attribute__((noinline)) void v9(int c)
{
  for (int i = 0; i < 2; c && (n++, 1), i++)
    m++;
}
__attribute__((noinline)) int u1(int c) { return c && (n++, 1); }
__attribute__((noinline)) int u2(int c) { return c || (n++, 0); }
__attribute__((noinline)) int u3(int c) { int r = (c && (n++, 1)) + 5; return r; }
__attribute__((noinline)) int u4(int c) { if (c && (n++, 1)) return 7; return 8; }
__attribute__((noinline)) int u5(int c) { return (c && (n++, 1)) ? 3 : 4; }
__attribute__((noinline)) int u6(int c) { return c && (n++, 0); }

int main(void)
{
  v1(0); v1(1);
  printf("n %d", n); n = 10;
  v2(1); v2(0);
  printf(" %d", n); n = 20;
  v3(0); v3(1);
  printf(" %d", n); n = 30;
  v4(0); v4(1);
  printf(" %d\n", n);
  n = 0; m = 0;
  v5(0, 2); v5(1, 3);
  printf("v5 %d %d\n", n, m);
  n = 0;
  v6(0, 1); v6(1, 0); v6(1, 1);
  v7(1, 0); v7(0, 1); v7(0, 0);
  printf("v6/v7 %d\n", n);
  n = 0; m = 0;
  v8(0); v8(1);
  v9(0); v9(1);
  printf("v8/v9 %d %d\n", n, m);
  n = 0;
  int r[12] = { u1(0), u1(2), u2(0), u2(4), u3(0), u3(1),
                u4(0), u4(1), u5(0), u5(1), u6(0), u6(1) };
  printf("u");
  for (int i = 0; i < 12; i++)
    printf(" %d", r[i]);
  printf(" n=%d\n", n);
  return 0;
}
