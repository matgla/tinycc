/*
 *  TCC IR - flag-setting add with a shifted operand regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `y = x + (i << 2); if (y == 0)`: the SHL folds into the ADD as a barrel
 * shift, and codegen fused the CMP #0 into a flag-setting ADD that dropped
 * the shift (`adds r2, r0, r1`), so y was x + i (found by -fsanitize=null on
 * `a[i]`, whose null test is exactly this). */
#include <stdio.h>

__attribute__((noinline)) int add_test(int x, int i)
{
  int y = x + (i << 2);
  if (y == 0)
    return 7;
  return y;
}

__attribute__((noinline)) int sub_test(int x, int i)
{
  int y = x - (i << 3);
  if (y != 0)
    return y;
  return -5;
}

__attribute__((noinline)) int *ptr_test(int *b, int i)
{
  int *p = b + i;
  if (!p)
    return 0;
  *p = 5;
  return p;
}

int arr[8];

int main(void)
{
  printf("%d %d %d\n", add_test(100, 3), add_test(-12, 3), add_test(1, 0));
  printf("%d %d %d\n", sub_test(100, 3), sub_test(24, 3), sub_test(0, 0));
  printf("%d %d\n", (int)(ptr_test(arr, 5) - arr), arr[5]);
  return 0;
}
