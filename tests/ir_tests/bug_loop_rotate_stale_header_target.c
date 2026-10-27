/*
 * ssa:loop_rotate kept the old header CMP's jump-target mark after the
 * back-edge left it, which hid `i = 0` from IV strength reduction.  The mark
 * is now dropped only when no branch reaches the CMP: a goto to the loop
 * head still keeps it, and the loop must not assume i starts at 0.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

__attribute__((noinline)) int sum_iv(int n)
{
  volatile int sum = 0;
  for (int i = 0; i < n; i++)
    sum += i * 7 + 13;
  return sum;
}

/* The loop head is also a goto target: entered with i = 3 or i = 0. */
__attribute__((noinline)) int sum_goto_head(int n, int skip)
{
  volatile int sum = 0;
  int i = 0;
  if (skip)
  {
    i = 3;
    goto head;
  }
  sum = 100;
head:
  while (i < n)
  {
    sum += i * 7 + 13;
    i++;
  }
  return sum;
}

int main(void)
{
  printf("sum_iv: %d %d %d\n", sum_iv(0), sum_iv(1), sum_iv(1000));
  printf("sum_goto_head: %d %d %d %d\n", sum_goto_head(10, 0), sum_goto_head(10, 1), sum_goto_head(2, 1),
         sum_goto_head(0, 0));
  return 0;
}
