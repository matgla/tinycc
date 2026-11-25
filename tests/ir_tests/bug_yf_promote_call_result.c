/*
 *  TCC IR - -fyasc-facts yf_promote and a call's own result
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `x = f(&x)`: put.c coalesces the call's result into x, and yf_promote
 * (sl_forward.c) reloaded x from its slot after the call, so x held what f
 * wrote through p, not what f returned.  A call that defines the scalar
 * itself is no candidate. */
#pragma yasc

int printf(const char * view fmt, ...);

__attribute__((noinline)) static int f(inout int * view p)
{
  *p = 1;
  return 2;
}

__attribute__((noinline)) static int set(inout int * view p, int v)
{
  *p = v;
  return v + 100;
}

__attribute__((noinline)) static int g(void)
{
  int x = 0;
  x = f(&x);
  return x;
}

/* the written value still reaches a later read */
__attribute__((noinline)) static int h(int k)
{
  int x = 0;
  int r = set(&x, k);
  return r * 1000 + x;
}

/* the result kept, then overwritten through p by the next call */
__attribute__((noinline)) static int loop(int n)
{
  int x = 0;
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    x = set(&x, x + i);
    s += x;
  }
  return s * 1000 + x;
}

int main(void)
{
  printf("g %d\n", g());
  printf("h %d\n", h(7));
  printf("loop %d\n", loop(4));
  return 0;
}
