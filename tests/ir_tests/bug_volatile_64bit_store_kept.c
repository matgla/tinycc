/*
 *  TCC IR - Overwritten 64-bit volatile stores
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* vstore retyped a 64-bit store's destination to its bare basic type,
 * dropping VT_VOLATILE before the IR operand was built, so ptr_store_dse
 * deleted `*p = 5; *p = 6;`'s first store and ptr_local_fwd a store through
 * `(volatile u64 *)&x`.  The store now records volatile_access first.  The
 * stores are counted by test_volatile_kept_codegen.py. */
#include <stdio.h>

typedef unsigned long long u64;

__attribute__((noinline)) void s64(volatile u64 *p) { *p = 5ULL; *p = 6ULL; }
__attribute__((noinline)) void sll(volatile long long *p, long long v) { *p = v; *p = v + 1; }
__attribute__((noinline)) void sdbl(volatile double *p) { *p = 1.0; *p = 2.0; }
__attribute__((noinline)) void sidx(volatile u64 *p, int i) { p[i] = 1; p[i] = 2; }
__attribute__((noinline)) void smix(volatile u64 *p, volatile unsigned *q) { *q = 1; *p = 5ULL; *p = 6ULL; }
__attribute__((noinline)) void scopy(volatile u64 *p, volatile u64 *q) { *p = *q; *p = *q + 1; }
__attribute__((noinline)) int l64(int k)
{
  u64 x = k;
  int c = (int)x;
  *(volatile u64 *)&x = 5;
  return c;
}
/* Non-volatile neighbour: the overwritten store still goes. */
__attribute__((noinline)) void p64(u64 *p) { *p = 5ULL; *p = 6ULL; }

int main(void)
{
  u64 a = 0, b = 0, c = 7;
  long long ll = 0;
  double d = 0;
  u64 arr[3] = {0, 0, 0};
  unsigned w = 0;
  s64(&a);
  sll(&ll, -3);
  sdbl(&d);
  sidx(arr, 1);
  smix(&b, &w);
  printf("%llu %lld %d %llu %llu %u %llu\n", a, ll, (int)d, arr[1], b, w, arr[0]);
  scopy(&a, &c);
  printf("%llu %d\n", a, l64(9));
  p64(&b);
  printf("%llu\n", b);
  return 0;
}
