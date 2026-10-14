/*
 *  TCC Tests - A folded switch-dispatcher call whose result is discarded
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ssa:switch_call_replace turned a discarded `h(1);` into an ASSIGN with no
 * destination, emitted as `movs r0, #101` over the live argument in r0, and
 * left its FUNCPARAM behind the replayed stores.  A discarded result is now
 * a NOP and every param of the call is removed. */
#include <stdio.h>

int calls;
__attribute__((noinline)) int h(int v) { calls++; return v + 100; }
__attribute__((noinline)) int sw(int v)
{
  switch (v)
  {
  case 1: calls += 10; return 7;
  case 2: return 9;
  default: return -1;
  }
}
__attribute__((noinline)) long long hl(int v) { calls++; return (long long)v << 40; }

__attribute__((noinline)) int t1(int k) { h(1); return h(k); }
__attribute__((noinline)) int t2(int k) { int r; h(1); h(2); r = h(k); h(9); return r; }
__attribute__((noinline)) int t3(int k, int j) { sw(1); return k + j; }
__attribute__((noinline)) int t4(int k) { int r = h(3); sw(1); return r + k; }
__attribute__((noinline)) int t5(int k) { hl(1); return (int)(hl(1) >> 40) + k; }
__attribute__((noinline)) int t6(int k) { h(4); return sw(2) + k; }

int main(void)
{
  int a = t1(8);
  int b = t2(8);
  int c = t3(5, 6);
  int d = t4(2);
  int e = t5(3);
  int f = t6(1);
  printf("%d %d %d %d %d %d\n", a, b, c, d, e, f);
  printf("calls=%d\n", calls);
  return 0;
}
