/* An old-style definition redeclared by a compatible prototype: the prototype
 * replaced the definition's parameter list, so an inline replay of the body
 * bound the prototype's names (`c` fell through to a global, or was
 * undeclared).  The symbol now keeps the definition's parameters; calls take
 * the prototype.
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include <stdio.h>

int c = 7;
__attribute__((noinline)) int arg(int n) { return n; }
__attribute__((noinline)) double dbl(double d) { return d; }

static int gk(c) signed char c; { return c * 2; }
static int gk(int d);
static int gu(c) signed char c; { return c < 0; }
static int gu(int);
/* No global of the parameter's name: the replay must still bind it. */
static int gn(q) signed char q; { return q < 0; }
static int gn(int);
static int gf(f) float f; { return f == 1.0f; }
static int gf(double);
static inline __attribute__((always_inline)) int ga(s) short s; { return s; }
static int ga(int x);
/* The prototype converts the argument: a long long parameter gets 5LL. */
static long long gl(v, w) long long v; unsigned char w; { return v + w; }
static long long gl(long long, int);
__attribute__((noinline)) static int go(c) signed char c; { return c + 1; }
static int go(int);
/* A constant argument folds through the definition's parameter. */
static int gc(c) signed char c; { return c + 1; }
static int gc(int);
/* A prototyped definition redeclared under another name stays right. */
static int pn(int c) { return c * 3; }
static int pn(int e);

int main(void)
{
  int t = arg(200);
  int (*fp)(int) = gk;
  printf("%d %d %d\n", gk(t), gu(t), gn(t));
  printf("%d %d %d\n", gk(200), gu(200), gn(-1));
  printf("%d %d %d\n", gf(dbl(1.0000000001)), ga(arg(70005)), (int)gl(arg(5), arg(258)));
  printf("%d %d %d %d\n", go(t), gc(200), pn(arg(4)), fp(t));
  return 0;
}
