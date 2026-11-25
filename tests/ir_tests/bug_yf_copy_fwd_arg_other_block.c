/*
 *  TCC IR - -fyasc-facts copy_fwd and an argument with control flow
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `fill(&x, c ? g1() : g2())` leaves `PARAM &x` in an earlier block than
 * the call.  copy_fwd.c cfw_yf_writes looked for the call's no-escape
 * arguments only in the call's own block, so fill was taken not to write x
 * and a copy of x after `x = y; fill(...)` was forwarded to y.  The
 * call's arguments are now found wherever they are. */
#pragma yasc

int printf(const char * view fmt, ...);

struct S
{
  int v[32]; /* copied with memcpy */
};

/* y is filled through a view parameter: its address does not escape */
__attribute__((noinline)) static void init(inout struct S * view p, int k)
{
  for (int i = 0; i < 32; i++)
    p->v[i] = k + i;
}

__attribute__((noinline)) static void fill(inout struct S * view p, int v)
{
  p->v[0] = v;
  p->v[3] = v + 1;
}

__attribute__((noinline)) static int use(const struct S * view p, int v)
{
  return p->v[0] + p->v[1] + p->v[3] + v;
}

__attribute__((noinline)) static int g1(void)
{
  return 100;
}

__attribute__((noinline)) static int g2(void)
{
  return 200;
}

__attribute__((noinline)) static int written(int c, int k)
{
  struct S y = undefined;
  init(&y, k);
  struct S x = undefined;
  x = y;
  fill(&x, c ? g1() : g2());
  struct S z = undefined;
  z = x; /* a copy of x: copy_fwd forwards it while x == y */
  return z.v[0] * 1000 + z.v[1] * 100 + z.v[3];
}

/* a read-only parameter: x still equals y after it */
__attribute__((noinline)) static int read_only(int c, int k)
{
  struct S y = undefined;
  init(&y, k);
  struct S x = undefined;
  x = y;
  int r = use(&x, c ? g1() : g2());
  struct S z = undefined;
  z = x;
  return r * 1000 + z.v[0] + z.v[3];
}

int main(void)
{
  printf("written1 %d\n", written(1, 1));
  printf("written0 %d\n", written(0, 1));
  printf("read_only1 %d\n", read_only(1, 1));
  printf("read_only0 %d\n", read_only(0, 1));
  return 0;
}
