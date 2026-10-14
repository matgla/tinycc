/*
 * Regression test: a nested function an inlined callee defined stayed bound
 * in the caller after the replay, so the caller's later call of that name
 * (its own nested function, or the file-scope one) reached the callee's.
 * Fix: a callee whose body defines a nested function is not token-replayed.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int fq(int y) { return y + 1000; }

static inline int P(int a)
{
  int q(int y) { return y * 2; }
  return q(a);
}

/* (named w: a nested q of the functions above once still bound after its
 * parent) */
static inline int R(int a)
{
  int w(int y) { return y ? w(y - 1) + 3 : 0; }
  return w(a);
}

/* the caller's nested q is hidden by a block-local q at the call */
int t1(void)
{
  int q(int y) { return y + 1000; }
  int r = 0;
  {
    int q = 5;
    r = P(3) + q;
  }
  r += q(1);
  return r;
}

/* two calls, the second from a deeper block */
int t2(void)
{
  int q(int y) { return y + 1000; }
  int r = 0;
  {
    int q = 5;
    r = P(3) + q;
    {
      int q = 6;
      r += P(1) + q;
    }
  }
  r += q(2);
  return r;
}

/* the caller's nested q is block-scoped; a sibling block defines another */
int t3(void)
{
  int r = 0;
  {
    int q(int y) { return y + 100; }
    {
      int q = 5;
      r += P(3) + q;
    }
    r += q(1);
  }
  {
    int q(int y) { return y + 200; }
    r += q(1);
  }
  return r;
}

/* a recursive nested function in the callee */
int t4(void)
{
  int w(int y) { return y + 5000; }
  int r = 0;
  {
    int w = 7;
    r = R(4) + w;
  }
  r += w(3);
  return r;
}

/* the caller's nested q is not hidden at the call */
int t5(void)
{
  int q(int y) { return y + 700; }
  int r = P(5);
  r += q(4);
  return r;
}

/* the caller's later fq() is the file-scope fq */
static inline int PF(int a) { int fq(int y) { return y * 2; } return fq(a); }
int t6(void) { int r = PF(3); r += fq(1); return r; }

/* the caller's out-of-line nested s calls the caller's own nested q */
static inline int P2(int a)
{
  __attribute__((noinline)) int q(int y) { return y * 2; }
  __attribute__((noinline)) int p(int y) { return q(y) + 1; }
  int (*volatile fp)(int) = p;
  return fp(a);
}
int t7(void)
{
  __attribute__((noinline)) int q(int y) { return y + 1000; }
  int r = P2(3);
  __attribute__((noinline)) int s(int y) { return q(y) + 7; }
  int (*volatile sp)(int) = s;
  r += sp(1);
  return r;
}

/* the definition with attributes before its name, and an always_inline callee */
static inline __attribute__((always_inline)) int PA(int a)
{
  auto int fq(int y);
  __attribute__((unused)) int fq(int y) { return y * 3; }
  return fq(a);
}
int t8(void) { int r = PA(2); r += fq(2); return r; }

int main(void)
{
  printf("t1 %d\n", t1());
  printf("t2 %d\n", t2());
  printf("t3 %d\n", t3());
  printf("t4 %d\n", t4());
  printf("t5 %d\n", t5());
  printf("t6 %d t7 %d t8 %d\n", t6(), t7(), t8());
  return 0;
}
