/*
 * Regression test (round-5 integration, second set): GNU nested functions
 * meeting the inliner's free-name check, K&R parameter conversion, volatile
 * stores, comma expressions and parent enum/typedef visibility together.
 * Expected output is gcc -O0's.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int v = 3;
typedef long long W;

/* A file-scope inline helper whose free name `v` is the global: called from
 * a nested function whose parent shadows v. */
static inline int addv(int x) { return x + v; }

__attribute__((noinline)) int d1(int a)
{
  int v = 1000;
  int n(int y) { return addv(y) + v; }
  return n(a) + addv(0);
}

/* K&R helper called from a nested function with narrow arguments. */
static int knr(c, f) char c; float f;
{
  return c * 10 + (int)(f * 2);
}
__attribute__((noinline)) int d2(int a)
{
  char cc = (char)(a + 300);
  int n(int y) { return knr(cc + y, 1.25f); }
  return n(1);
}

/* Volatile capture written by the nested function through 64-bit and narrow
 * stores; comma expressions whose right operand is the call. */
__attribute__((noinline)) int d3(int a)
{
  volatile long long big = 0;
  volatile short sm = 0;
  int n(int y) { big = (long long)y << 33; sm = (short)(y * 3); return y + 1; }
  int r = (a++, n(a));
  r = (n(r), n(r + 1));
  return r * 1000 + (int)(big >> 33) * 10 + sm;
}

/* Parent enum constants and typedefs inside an inlined parent's nested
 * function, while the caller defines its own with the same names. */
static inline int d4_parent(int a)
{
  enum { E = 5 };
  typedef short S;
  int n(int y) { S s = (S)(y * E); return s + (int)sizeof(S); }
  return n(a) + n(a + 1);
}
__attribute__((noinline)) int d4(int a)
{
  enum { E = 50 };
  typedef W S;
  return d4_parent(a) * 100 + E + (int)sizeof(S);
}

/* Sibling nested functions with a typedef shadowed only in one block. */
__attribute__((noinline)) int d5(int a)
{
  typedef int T;
  int r = 0;
  int one(int y) { return y + (int)sizeof(T); }
  {
    typedef char T;
    int two(int y) { return one(y) * 10 + (int)sizeof(T); }
    r += two(a);
  }
  int three(int y) { return one(y) + (int)sizeof(T) * 100; }
  return r + three(a);
}

int main(void)
{
  printf("d1 %d %d\n", d1(1), d1(7));
  printf("d2 %d %d\n", d2(1), d2(-5));
  printf("d3 %d %d\n", d3(1), d3(9));
  printf("d4 %d %d\n", d4(1), d4(6));
  printf("d5 %d %d\n", d5(2), d5(8));
  return 0;
}
