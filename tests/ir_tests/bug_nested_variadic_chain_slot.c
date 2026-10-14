/*
 *  TCC IR - a variadic nested function keeps its chain slot
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A variadic function saves r0-r3 at frame top -16..-4, and a nested
 * function saves its incoming static chain at frame top -4.  Base f6d53136:
 * r3's save overwrote the chain, and the chain reload after a call read r3,
 * so a variadic nested function reading a capture after a call faulted at
 * every level.  Fix: with a static chain the save area moves to -20..-8. */
#include <stdarg.h>
#include <stdio.h>

__attribute__((noinline)) int f(int k)
{
  int base = k * 10;
  __attribute__((noinline)) int sum(int n, ...)
  {
    va_list ap;
    va_start(ap, n);
    int s = base;
    for (int i = 0; i < n; i++)
      s += va_arg(ap, int);
    va_end(ap);
    printf("n=%d ", n);
    return s + base;
  }
  return sum(3, 1, 2, 3) + sum(5, 1, 1, 1, 1, 1);
}

/* named arguments in every register, a capture both ways, a long long vararg */
__attribute__((noinline)) long long g(int k)
{
  long long acc = k;
  __attribute__((noinline)) long long mix(int a, int b, int c, ...)
  {
    va_list ap;
    va_start(ap, c);
    long long x = va_arg(ap, long long);
    int y = va_arg(ap, int);
    va_end(ap);
    acc += a + b * 10 + c * 100;
    printf("y=%d ", y);
    return acc + x + y;
  }
  long long r = mix(1, 2, 3, 1000000000000LL, 7);
  return r + acc;
}

int main(void)
{
  int r = f(2);
  printf("f %d\n", r);
  long long q = g(5);
  printf("g %lld\n", q);
  return 0;
}
