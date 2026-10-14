/*
 *  TCC IR - ptr_local_fwd must not resolve pointer copies across asm goto
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The CFG has no edges for `asm goto` targets, so ptr_local_fwd's per-use
 * reaching-definition walk saw only the fall-through def (`t = &y`) reach
 * `*t` and rewrote it into a store to y.  Functions with asm goto now get no
 * per-use resolution.  Controls: the same shapes with a plain branch. */

#include <stdio.h>

#define NI __attribute__((noinline))

NI int lea_two(int c)
{
  int x = 1, y = 2;
  int *t = &x;
  __asm__ goto("cmp %0, #0\n\tbne %l1" : : "r"(c) : "cc" : L);
  t = &y;
L:
  *t = 9;
  return x * 10 + y;
}

NI int copy_two(int c)
{
  int x = 1, y = 2;
  int *px = &x, *py = &y;
  int *t = px;
  __asm__ goto("cmp %0, #0\n\tbne %l1" : : "r"(c) : "cc" : L);
  t = py;
L:
  {
    int *u = t;
    *u = 9;
  }
  return x * 10 + y;
}

NI int copy_chain(int c)
{
  int x = 1, y = 2;
  int *t = &x, *u;
  u = t;
  __asm__ goto("cmp %0, #0\n\tbne %l1" : : "r"(c) : "cc" : L);
  t = &y;
  u = t;
L:
  {
    int *w = u;
    *w += 5;
  }
  return x * 10 + y;
}

NI int lea_two_branch(int c)
{
  int x = 1, y = 2;
  int *t = &x;
  if (c)
    goto L;
  t = &y;
L:
  *t = 9;
  return x * 10 + y;
}

NI int copy_two_branch(int c)
{
  int x = 1, y = 2;
  int *px = &x, *py = &y;
  int *t = px;
  if (c)
    goto L;
  t = py;
L:
  {
    int *u = t;
    *u = 9;
  }
  return x * 10 + y;
}

int main(void)
{
  printf("lea_two %d %d\n", lea_two(0), lea_two(1));
  printf("copy_two %d %d\n", copy_two(0), copy_two(1));
  printf("copy_chain %d %d\n", copy_chain(0), copy_chain(1));
  printf("lea_two_branch %d %d\n", lea_two_branch(0), lea_two_branch(1));
  printf("copy_two_branch %d %d\n", copy_two_branch(0), copy_two_branch(1));
  return 0;
}
