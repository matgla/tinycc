/*
 *  TCC IR - a nested function keeps sl (the static chain) out of scratch use
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Captured reads are emitted as [sl, #off] off the live chain register, so
 * sl must never be handed out as a scratch register in a nested function.
 * Base f6d53136 loaded `big` with `ldrd ip, sl, [r1]` and then read `bb`
 * through the clobbered chain, and under register pressure the allocator
 * itself handed out sl: HardFault at every level.  Fix: R10 is out of the
 * allocator's set and the scratch baseline in any function with a chain. */
#include <stdio.h>

long long big = 0x100000000LL;
long long big2 = 0x500000003LL;
unsigned long long ubig = 0xFFFFFFFF00000002ULL;

__attribute__((noinline)) int f(int a)
{
  long long bb = 7;
  __attribute__((noinline)) int nf(int y) { return (int)(big >> 32) + y + (int)bb; }
  return nf(a);
}

/* 64-bit global loads and stores around int and 64-bit captures */
__attribute__((noinline)) long long g(int a)
{
  int k = a * 3;
  long long acc = 11;
  __attribute__((noinline)) long long nf2(int y)
  {
    long long t = big2 + big + (long long)y;
    big2 = t + k;
    acc += t;
    return (long long)(ubig >> 1) + acc + k;
  }
  long long r = nf2(a);
  r += nf2(1);
  return r + acc;
}

/* two levels: the inner function reads both levels' captures */
__attribute__((noinline)) int h(int a)
{
  long long c1 = 100;
  int c0 = a;
  int mid(int y)
  {
    long long c2 = big + 3;
    __attribute__((noinline)) int in(int z) { return (int)(big2 >> 32) + (int)c1 + (int)c2 + z + c0; }
    return in(y) + (int)(big >> 32);
  }
  return mid(a);
}

/* register pressure: the allocator must not take sl either */
long long GA[8] = {1, 2, 3, 4, 5, 6, 7, 8};
__attribute__((noinline)) long long p(int a)
{
  long long cap = 1000;
  int ci = a;
  __attribute__((noinline)) long long nf3(int y)
  {
    long long a0 = GA[0] * y, a1 = GA[1] * y, a2 = GA[2] * y, a3 = GA[3] * y;
    long long a4 = GA[4] * y, a5 = GA[5] * y, a6 = GA[6] * y, a7 = GA[7] * y;
    long long s = 0;
    for (int i = 0; i < y; i++)
    {
      s += a0 ^ a1;
      s += a2 * a3 + cap;
      s ^= a4 + a5;
      s += a6 - a7 + ci;
      a0++;
      a7--;
      cap++;
    }
    return s + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + cap;
  }
  return nf3(a + 3);
}

int main(void)
{
  volatile int one = 1;
  printf("f %d\n", f(one));
  long long r = g(one);
  printf("g %lld %lld\n", r, big2);
  printf("h %d\n", h(one));
  printf("p %lld\n", p(one));
  return 0;
}
