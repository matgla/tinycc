/*
 * Regression test (round-5 integration): GNU nested functions combining the
 * fixes of three branches -- static chains built from a real frame with
 * capture ranges (nested-frame), block-scoped nested names with the parent's
 * enum constants / typedefs (nested-scope), and inline replay only where a
 * nested function's names bind as at its definition (inliner-scope).  Each
 * line mixes captures (scalar, aggregate, array), shadowing, inlining of the
 * parent or the nested function, ancestor calls and trampolines.  Expected
 * output is gcc -O0's.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int g = 5;
typedef short T;
struct P
{
  int x, y, z;
};

static int apply(int (*f)(int), int v)
{
  return f(v);
}

static int idf(int x) { return x; }

/* Inlined parent; aggregate capture written by the child, read after. */
static inline int c1(int a)
{
  struct P p = {a, a + 1, a + 2};
  enum { K = 7 };
  void bump(int d) { p.y += d * K; p.z = p.x + p.y; }
  bump(1);
  bump(2);
  return p.x * 10000 + p.y * 100 + p.z;
}

/* A caller shadowing g and T, calling an inlined parent whose nested
 * function reads its own g / T. */
static inline int c2_parent(int a)
{
  int g = 40;
  typedef int T;
  int rd(int y) { return y + g + (int)sizeof(T); }
  return rd(a) + apply(rd, a + 1);
}
__attribute__((noinline)) int c2(int a)
{
  int g = 900;
  typedef char T;
  return c2_parent(a) * 1000 + g + (int)sizeof(T);
}

/* Ancestor call: a grandchild calls its grandparent's nested function, which
 * reads an array capture through an interior pointer. */
__attribute__((noinline)) int c3(int a)
{
  int arr[4] = {a, a * 2, a * 3, a * 4};
  int top(int i) { int *e = &arr[4]; return e[-1 - i]; }
  int mid(int i)
  {
    int bot(int j) { return top(j) + top(i); }
    return bot(i + 1) + apply(bot, 0);
  }
  return mid(1);
}

/* The same nested name in two blocks, each capturing a different local;
 * a later same-named local in the outer block is not the captured one. */
__attribute__((noinline)) int c4(int a)
{
  int r = 0;
  {
    int v = a;
    int f(int k) { return v * k; }
    r += f(3);
  }
  {
    int v = a + 100;
    int f(int k) { return v + k + g; }
    int g = 1;
    r += f(3) * g;
  }
  return r;
}

/* An sret nested function capturing a struct and a scalar, returned through
 * a call made after its chain was set up; parent inlined. */
static inline struct P c5_parent(int a)
{
  struct P base = {a, 2 * a, 3 * a};
  int s = 11;
  struct P mk(int k)
  {
    struct P q = base;
    q.x += k * s;
    q.z = q.z + apply(idf, k);
    return q;
  }
  struct P out = mk(1);
  s = 20;
  struct P out2 = mk(2);
  out.y = out2.x;
  return out;
}
__attribute__((noinline)) int c5(int a)
{
  struct P p = c5_parent(a);
  return p.x * 10000 + p.y * 100 + p.z;
}

/* Recursion with captured counters, nested fn used as a trampoline from a
 * deeper frame, and a volatile captured local. */
__attribute__((noinline)) int c6(int n)
{
  volatile int calls = 0;
  int acc = 0;
  int rec(int k)
  {
    calls++;
    if (k == 0)
      return acc;
    acc += k;
    return rec(k - 1) + apply(rec, 0);
  }
  return rec(n) * 100 + calls;
}

/* Inlined sibling shadowing a typedef and a local around a call to another
 * nested function that captures them. */
__attribute__((noinline)) int c7(int a)
{
  typedef int U;
  int w = a * 3;
  int leaf(int y) { return y + w + (int)sizeof(U); }
  int sib(int y)
  {
    typedef char U;
    int w = 1;
    return leaf(y) * 10 + w + (int)sizeof(U);
  }
  return sib(a) + leaf(0);
}

int main(void)
{
  printf("c1 %d %d\n", c1(1), c1(3));
  printf("c2 %d %d\n", c2(1), c2(5));
  printf("c3 %d %d\n", c3(1), c3(7));
  printf("c4 %d %d\n", c4(2), c4(9));
  printf("c5 %d %d\n", c5(1), c5(4));
  printf("c6 %d %d\n", c6(3), c6(5));
  printf("c7 %d %d\n", c7(2), c7(6));
  return 0;
}
