/*
 *  TCC IR - a parent's frame layout keeps what its nested functions reach
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The parent's slots were sized and shared by the parent's own references
 * only.  Base f6d53136, every level: `v`, reached through the captured
 * pointer `p`, and a captured char/short/float parameter's home were given to
 * the next local, and an array the parent indexed only at a constant lay
 * partly below the frame (HardFault at -O0/-O1).  Fix: captures, parameters
 * included, are live for the whole body before the address-flow extension
 * runs, and the frame holds every capture. */
#include <stdio.h>
#define NI __attribute__((noinline))
NI int f(int a)
{
  int v = 50;
  int *p = &v;
  NI int nf(int y) { return y + *p; }
  int (*volatile fp)(int) = nf;
  return fp(a);
}
NI long long Hc(char c) { NI long long h(int y) { return y + c; } long long (*volatile hp)(int) = h; return hp(1); }
NI long long Hs(short s) { NI long long h(int y) { return y + s; } long long (*volatile hp)(int) = h; return hp(1); }
NI long long Hl(long long l) { NI long long h(int y) { return y + (l >> 32); } long long (*volatile hp)(int) = h; return hp(1); }
NI long long Hd(double d) { NI long long h(int y) { return y + (long long)(d * 4); } long long (*volatile hp)(int) = h; return hp(1); }
NI long long Hf(float f) { NI long long h(int y) { return y + (long long)(f * 2); } long long (*volatile hp)(int) = h; return hp(1); }
NI long long H5(char c, short s, long long l, double d, float f)
{
  NI long long h(int y) { return y + c + s + (l >> 32) + (long long)(d * 4) + (long long)(f * 2); }
  long long (*volatile hp)(int) = h;
  return hp(1);
}
NI int r5(int k)
{
  int arr[8];
  int fill(int i) { if (i >= 8) return 0; arr[i] = i * k; return arr[i] + fill(i + 1); }
  int s = fill(0);
  return s * 100 + arr[7];
}
NI int r6(int k)
{
  int arr[8];
  NI void put(int i) { arr[i] = i + k; }
  for (int i = 0; i < 8; i++)
    put(i);
  int other[4] = {k, k, k, k};
  return arr[7] * 1000 + arr[0] * 10 + other[3];
}

/* an address kept in a captured struct or array, and one the child stores */
NI int g1(int a)
{
  int v = a * 3, w = a * 5;
  struct { int *q; } s;
  int *tab[2];
  NI int rd(void) { return *s.q * 100 + *tab[1]; }
  s.q = &v;
  tab[1] = &w;
  int (*volatile fp)(void) = rd;
  int z = 77;
  return fp() + z;
}
NI int g2(int a)
{
  int *cp;
  int u = a + 1;
  NI void set(void) { cp = &u; }
  set();
  int t1 = 5, t2 = 6;
  return *cp * 100 + t1 + t2;
}

int main(void)
{
  printf("ptr %d\n", f(1));
  printf("params %lld %lld %lld %lld %lld %lld\n", Hc(5), Hs(6), Hl(0x700000000LL), Hd(1.25), Hf(2.5),
         H5(5, 6, 0x700000000LL, 1.25, 2.5));
  printf("arr %d %d\n", r5(3), r6(2));
  printf("g %d %d\n", g1(2), g2(4));
  return 0;
}
