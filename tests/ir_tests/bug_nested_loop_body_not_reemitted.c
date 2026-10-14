/*
 * Regression test: a small GNU nested function registered as an auto-inline
 * candidate whose out-of-line compile then revoked auto-inlining (a loop
 * body, at -O2/-Os) kept its inline_fns entry, so emit_inline_functions
 * compiled the body a second time at the end of the TU -- outside its
 * parent, where a capture is undeclared ("'r4' undeclared").  A nested
 * function is only ever compiled by its parent.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

typedef struct
{
  int a, b;
} P;

__attribute__((noinline)) int f(int x)
{
  P r4 = {x, x * 2};
  int in4(int y)
  {
    int s = 0;
    for (int i = 0; i < 6; i++)
      s += r4.a * y;
    s += r4.b;
    return s;
  }
  return in4(2) + in4(x);
}

__attribute__((noinline)) int g2(int x)
{
  int k = x + 1;
  int lp(int n)
  {
    int s = 0;
    for (int i = 0; i < n; i++)
      s += k * i;
    return s;
  }
  return lp(4) * 100 + lp(x);
}

int main(void)
{
  printf("%d %d\n", f(3), f(7));
  printf("%d %d\n", g2(2), g2(5));
  return 0;
}
