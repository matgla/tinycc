/*
 * Regression test: a callee inlined into an out-of-line GNU nested function
 * bound its free names to the parent locals and typedefs the nested function
 * captures (linked at scope 0, unseen by the shadow check).  Fix: a captured
 * name the callee does not declare itself keeps the call.  The nested
 * function's own child still inlines and reads the captures.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int g = 5;
int arr[3] = {10, 20, 30};

static const int K = 9;
static inline int H(void) { return g; }
static inline int Ha(int i) { return arr[i]; }
static inline int Hl(int y) { int g = y * 3; return g + 1; }
static inline int Hc(int x) { return x + g; }
static inline int Hk(int x) { return x * K; }
static inline __attribute__((always_inline)) int Hw(int x) { return x + g; }
static inline int Hn(int x) { int k(int z) { return z + g; } return k(x); }
static int cnt;
static void cl(int *p) { cnt += *p + g; }

static int f1(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { return H() + y + g; }
  return nf(a);
}

static int f2(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { int k = g; return H() + y + k; }
  return nf(a);
}

static int f3(int a)
{
  int arr = 7;
  __attribute__((noinline)) int nf(int y) { return Ha(y) + arr; }
  return nf(a);
}

static int f4(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { return Hl(y) + g; }
  return nf(a);
}

/* two levels: the inner nested function captures the outer parent's g */
static int f5(int a)
{
  int g = 400;
  __attribute__((noinline)) int mid(int y)
  {
    __attribute__((noinline)) int in(int z) { return H() + z + g; }
    return in(y) + g;
  }
  return mid(a);
}

/* constant arguments (const-evaluated), always_inline, a cleanup function */
static int f6(int a)
{
  int g = 100, K = 1000;
  __attribute__((noinline)) int nf(int y) { return Hc(1) + Hk(2) + y + g + K; }
  return nf(a);
}

static int f7(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { return Hw(y) + g; }
  return nf(a);
}

static int f8(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y)
  {
    {
      __attribute__((cleanup(cl))) int v = y;
    }
    return g;
  }
  return nf(a) + cnt;
}

/* the callee's own nested function reads the global g */
static int f9(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { return Hn(y) + g; }
  return nf(a);
}

/* a parent typedef the nested function sees; Ht means the file-scope TT */
typedef int TT;
static inline int Ht(void) { return (int)sizeof(TT); }
static int f10(int a)
{
  typedef char TT;
  __attribute__((noinline)) int nf(int y) { TT c = (TT)y; return Ht() * 10 + c; }
  return nf(a);
}

/* the nested function's own child, inlined there, reads the captured g */
static int f11(int a)
{
  int g = 100;
  __attribute__((noinline)) int nf(int y) { int ch(int z) { return z + g; } return ch(y) + ch(1); }
  return nf(a);
}

int main(void)
{
  volatile int one = 1;
  printf("f1 %d\n", f1(one));
  printf("f2 %d\n", f2(one));
  printf("f3 %d\n", f3(one));
  printf("f4 %d\n", f4(one * 2));
  printf("f5 %d\n", f5(one));
  printf("f6 %d\n", f6(one));
  printf("f7 %d\n", f7(one));
  printf("f8 %d\n", f8(one));
  printf("f9 %d\n", f9(one));
  printf("f10 %d\n", f10(one * 300));
  printf("f11 %d\n", f11(one));
  return 0;
}
