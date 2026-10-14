/*
 *  TCC IR - Trailing stores to an address-taken local read through a pointer copy
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* dead_trail_addrvar tracked &x only through T->V and V->T copies, so a read
 * through a TEMP->TEMP copy (a conditional pointer's join) or pointer
 * arithmetic was not a read of x and the store before it was deleted.  Now
 * any use of &x that is not a deref or a tracked copy escapes x. */
#include <stdio.h>

__attribute__((noinline)) int f_addr(int *m, int k)
{
  int x = k;
  int *p = (k & 1) ? &x : m;
  int c1 = *p;
  x = c1 + 9;
  int c2 = *p;
  return c1 * 100 + c2;
}

/* &x is copied into another TEMP by the select. */
__attribute__((noinline)) int f_sel(int *m, int k)
{
  int x = k;
  int *p = &x;
  int *r = m;
  int c1 = *p;
  x = c1 + 9;
  int *s = k > 2 ? p : r;
  return c1 * 100 + *s;
}

/* A write through a pointer that is &x on one path only may hit *m. */
__attribute__((noinline)) int f_maybe_write(int *m, int k)
{
  int x = k;
  int *p = (k & 1) ? &x : m;
  int c1 = x;
  *p = c1 + 40;
  return c1;
}

__attribute__((noinline)) int f_arith(int k)
{
  int x[2];
  x[0] = k;
  x[1] = k + 1;
  int *p = &x[0];
  int c1 = p[0];
  x[1] = c1 + 20;
  int *q = p + (k & 1);
  return c1 * 100 + *q;
}

__attribute__((noinline)) int f_struct(int k)
{
  struct { int a, b; } s;
  s.a = k;
  s.b = 2;
  int *p = (k & 1) ? &s.b : &s.a;
  int c1 = s.a;
  s.b = c1 + 30;
  return c1 * 100 + *p;
}

/* Must stay correct (and still optimisable): a plain trailing dead store. */
__attribute__((noinline)) int f_plain(int k)
{
  int x = k;
  int *p = &x;
  int c1 = *p;
  x = c1 + 1;
  return c1;
}

int main(void)
{
  int a = 5;
  printf("%d\n", f_addr(&a, 3));
  printf("%d\n", f_addr(&a, 4));
  printf("%d\n", f_sel(&a, 3));
  printf("%d\n", f_sel(&a, 1));
  int b = 6;
  int r = f_maybe_write(&b, 3);
  printf("%d %d\n", r, b);
  r = f_maybe_write(&b, 4);
  printf("%d %d\n", r, b);
  printf("%d\n", f_arith(3));
  printf("%d\n", f_arith(4));
  printf("%d\n", f_struct(3));
  printf("%d\n", f_struct(4));
  printf("%d\n", f_plain(7));
  return 0;
}
