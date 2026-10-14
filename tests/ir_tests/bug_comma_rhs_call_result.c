/*
 *  TCC Tests - The value of a comma expression whose right operand is a call
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* gexpr dropped the return value of the call ending the RIGHT operand of a
 * comma, though that value is the expression's.  It now drops only the
 * discarded left operand's call result, and only when vtop names it. */
#include <stdio.h>

unsigned nv;
int calls;
__attribute__((noinline)) int h(int v) { calls++; return v + 100; }
__attribute__((noinline)) int *ph(int *p) { calls++; return p; }
__attribute__((noinline)) int add2(int a, int b) { return a + b; }
__attribute__((noinline)) int t1(void) { return (nv = 1, h(2)); }
__attribute__((noinline)) int t2(void) { return (h(0), h(3)); }
__attribute__((noinline)) int t3(int k) { return (k++, h(k)); }
__attribute__((noinline)) int t4(void) { int r; r = (nv = 3, h(4)); return r; }
__attribute__((noinline)) int t5(int k) { return (0, h(k)); }
__attribute__((noinline)) int t6(int k) { return (k, (nv++, h(k))); }
__attribute__((noinline)) int t7(int k) { return (h(1), h(2), h(k)) + (h(9), 0); }
__attribute__((noinline)) int t8(int k) { return add2((0, h(k)), (h(1), 5)); }
__attribute__((noinline)) int t9(int c) { return c ? (nv++, h(c)) : (nv--, h(-c)); }
__attribute__((noinline)) int t10(int k) { int x; return *(x = k, ph(&x)); }
__attribute__((noinline)) int t11(int k) { int x = 0; (x = h(k), h(1)); return x; }
__attribute__((noinline)) int t12(int k) { long long r = (k, (long long)h(k) << 33); return (int)(r >> 33); }

int main(void)
{
  int a = t1(), b = t2(), c = t3(5), d = t4(), e = t5(5);
  printf("%d %d %d %d %d\n", a, b, c, d, e);
  printf("%d %d %d %d\n", t6(7), t7(8), t8(9), t9(3));
  printf("%d %d %d %d\n", t9(0), t10(42), t11(6), t12(11));
  printf("nv=%u calls=%d\n", nv, calls);
  return 0;
}
