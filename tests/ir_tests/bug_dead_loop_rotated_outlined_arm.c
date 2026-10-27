/*
 * ssa:dead_loop rotated arm: loop_header_dup lays an else-if arm out past the
 * latch, so the body was not the range [header, latch] and the pure counting
 * loop survived.  The arm now takes the natural loop's blocks, wherever they sit.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

/* bench_control.c's bench_conditionals: folds to 1192, the loop must go. */
__attribute__((noinline)) int cond_chain(int iterations)
{
  int r = 0;
  for (int n = 0; n < iterations; n++)
  {
    int i = 42;
    r = 1234;
    if (i & 1)
      r += i * 3;
    else if (i % 3 == 0)
      r -= i;
    else
      r ^= i;
    if (r > 1000000)
      r = r >> 3;
    else if (r < -1000000)
      r = -r;
  }
  return r;
}

/* The outlined arm leaves the function: the trip it fires on matters. */
__attribute__((noinline)) int arm_early_exit(const int *a, int n, int key)
{
  int r = 0;
  for (int i = 0; i < n; i++)
  {
    r = 1234;
    if (a[i] > 1000000)
      r = 7;
    else if (a[i] == key)
      return -i - 1;
  }
  return r;
}

/* The outlined arm is entered from outside the loop: one trip from there is
 * not the trips that follow. */
__attribute__((noinline)) int goto_into_arm(int n, int k)
{
  int r = 0, i = 0;
  if (k)
    goto mid;
  for (; i < n; i++)
  {
    r = 5;
    if (i > 1000000)
      r = 6;
    else if (i < -1000000)
    {
    mid:
      r = 9;
    }
  }
  return r;
}

/* The outlined arm feeds a value read after the loop. */
__attribute__((noinline)) int arm_escapes(const int *a, int n)
{
  int r = 0;
  for (int i = 0; i < n; i++)
  {
    r = 1;
    if (a[i] > 100)
      r = 2;
    else if (a[i] < -100)
      r = a[i];
  }
  return r;
}

int main(void)
{
  int a[5] = {3, -500, 9, 2000000, 4};
  printf("cond_chain: %d %d %d\n", cond_chain(0), cond_chain(1), cond_chain(1000));
  printf("arm_early_exit: %d %d %d %d\n", arm_early_exit(a, 5, 9), arm_early_exit(a, 5, 4), arm_early_exit(a, 4, 77),
         arm_early_exit(a, 0, 3));
  printf("goto_into_arm: %d %d %d %d\n", goto_into_arm(0, 0), goto_into_arm(3, 0), goto_into_arm(1, 1),
         goto_into_arm(4, 1));
  printf("arm_escapes: %d %d %d\n", arm_escapes(a, 1), arm_escapes(a, 2), arm_escapes(a, 3));
  return 0;
}
