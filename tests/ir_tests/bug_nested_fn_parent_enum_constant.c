/*
 *  TCC Tests - an out-of-line nested function sees its parent's enum constants
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Nested bodies compile after the parent's scopes are popped; the parent's
 * typedefs and struct tags were re-linked for them, block-scope enumeration
 * constants were not, so K read the file-scope K.  The nested function's
 * definition now records the visible enum constants, and only the innermost
 * binding of each name (an inner typedef T hid by an outer one). */
#include <stdio.h>

#define NI __attribute__((noinline))

typedef int T;
enum { K = 3, Z = 4 };
struct S { int a; };

NI int h(void)
{
  typedef char T;
  enum { K = 100, Z = 0 }; /* Z == 0: the constant's own Sym is freed */
  struct S { char c[40]; };
  NI int s(int y) { T z = (T)y; return z + K + Z + (int)sizeof(struct S) + (int)sizeof(T); }
  int (*volatile f)(int) = s;
  return f(1);
}

/* inner block constants, typedefs and tags shadow outer ones */
NI int g(void)
{
  enum { A = 10 };
  typedef short U;
  struct R { char c[3]; };
  {
    enum { A = 20 };
    typedef long long U;
    struct R { char c[50]; };
    NI int s(void) { return A * 1000 + (int)sizeof(U) * 100 + (int)sizeof(struct R); }
    return s();
  }
}

/* a parent local variable named like a file-scope constant is captured */
NI int v(void)
{
  int K = 55;
  NI int s(void) { return K; }
  return s();
}

/* a parent local hides an outer block's enum constant */
NI int w(void)
{
  enum { M = 7 };
  {
    int M = 70;
    NI int s(void) { return M + 1; }
    return s();
  }
}

/* two levels: the inner nested function sees the outer one's constants
 * and the parent's */
NI int two(void)
{
  enum { P = 1000 };
  NI int l1(int y)
  {
    enum { Q = 200 };
    NI int l2(int x) { return x + P + Q + K; }
    return l2(y);
  }
  return l1(5);
}

/* constants usable in constant expressions: case labels, array sizes */
NI int cx(int y)
{
  enum { C1 = 5, C2 = 9 };
  NI int s(int x)
  {
    char buf[C2];
    switch (x)
    {
    case C1: return 1 + (int)sizeof buf;
    case C2: return 2;
    default: return 3;
    }
  }
  return s(y);
}

/* a later block's enum does not reach a nested function defined before it */
NI int later(void)
{
  NI int s(void) { return K; }
  enum { K = 999 };
  return s() + K;
}

int main(void)
{
  printf("%d %d %d %d %d\n", h(), g(), v(), w(), two());
  printf("%d %d %d %d\n", cx(5), cx(9), cx(1), later());
  return 0;
}
