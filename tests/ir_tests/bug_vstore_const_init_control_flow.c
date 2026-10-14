/*
 * vstore copied the source's const_init_data into the destination after an
 * aggregate assignment regardless of control flow (conditional, dead or
 * looped stores), so later vector folds used the wrong constants.  A store
 * now only invalidates (propagation is limited to a local's own initialiser),
 * and loop heads and labels forget all captured contents.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

typedef int v4 __attribute__((vector_size(16)));

__attribute__((noinline)) int cond(int x)
{
  v4 a = {1, 2, 3, 4};
  v4 b = {5, 6, 7, 8};
  if (x)
    b = a;
  v4 c = b + b;
  return c[0];
}

__attribute__((noinline)) int dead(void)
{
  v4 a = {1, 2, 3, 4};
  v4 b = {5, 6, 7, 8};
  if (0)
    b = a;
  v4 c = b + b;
  return c[0];
}

/* Destination without an initialiser: the copy must not attach the
 * source's bytes to b's frame slot either. */
__attribute__((noinline)) int cond_uninit(int x, v4 d)
{
  v4 a = {1, 2, 3, 4};
  v4 b;
  b = d;
  if (x)
    b = a;
  v4 c = b + b;
  return c[1];
}

__attribute__((noinline)) int ternary(int x)
{
  v4 a = {1, 2, 3, 4};
  v4 b = {5, 6, 7, 8};
  v4 c = (x ? a : b) + a;
  return c[2];
}

/* Neighbouring behaviour: an accumulator loop. */
__attribute__((noinline)) int loop(int n)
{
  v4 b = {1, 2, 3, 4};
  v4 s = {0, 0, 0, 0};
  for (int i = 0; i < n; i++)
    s = s + b;
  return s[0] * 1000 + s[3];
}

/* A loop whose body copies the accumulator in an initialiser. */
__attribute__((noinline)) int loop_init_copy(int n)
{
  v4 b = {1, 2, 3, 4};
  v4 s = {0, 0, 0, 0};
  for (int i = 0; i < n; i++)
  {
    v4 t = s;
    s = t + b;
  }
  return s[1];
}

/* A while loop reads the accumulator before the store that loops back. */
__attribute__((noinline)) int while_loop(int n)
{
  v4 b = {1, 2, 3, 4};
  v4 s = {0, 0, 0, 0};
  while (n--)
    s = s + b;
  return s[3];
}

/* A backward goto: the label is re-entered after the store. */
__attribute__((noinline)) int goto_loop(int n)
{
  v4 b = {1, 2, 3, 4};
  v4 s = {0, 0, 0, 0};
again:
  if (n-- > 0)
  {
    s = s + b;
    goto again;
  }
  return s[2];
}

/* Straight-line assignment of a folded constant. */
__attribute__((noinline)) int straight(void)
{
  v4 a = {1, 2, 3, 4};
  v4 b = {5, 6, 7, 8};
  b = a;
  v4 c = b + a;
  return c[3];
}

int main(void)
{
  v4 d = {50, 60, 70, 80};
  printf("cond0=%d (10)\n", cond(0));
  printf("cond1=%d (2)\n", cond(1));
  printf("dead=%d (10)\n", dead());
  printf("cond_uninit0=%d (120)\n", cond_uninit(0, d));
  printf("cond_uninit1=%d (4)\n", cond_uninit(1, d));
  printf("ternary0=%d (10)\n", ternary(0));
  printf("ternary1=%d (6)\n", ternary(1));
  printf("loop=%d (5020)\n", loop(5));
  printf("loop_init_copy=%d (6)\n", loop_init_copy(3));
  printf("while_loop=%d (12)\n", while_loop(3));
  printf("goto_loop=%d (9)\n", goto_loop(3));
  printf("straight=%d (8)\n", straight());
  return 0;
}
