/*
 *  TCC IR - Overwritten stores to a volatile local
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* redundant_var_assign NOPed `x = c + 9` in `x = c + 9; x = c + 10;` on a
 * volatile x: a def of a volatile VAR has no lvalue operand, so the
 * volatile-access check did not see it.  The pass now also asks whether the
 * dest names a volatile VAR.  The stores are counted by
 * test_volatile_kept_codegen.py. */
#include <stdio.h>

typedef unsigned long long u64;

__attribute__((noinline)) int v_twice(int k)
{
  volatile int x = k;
  int c = x;
  x = c + 9;
  x = c + 10;
  return c + x;
}

__attribute__((noinline)) int v_char(int k)
{
  volatile char x = k;
  char c = x;
  x = c + 1;
  x = c + 2;
  return c + x;
}

__attribute__((noinline)) int v_u64(int k)
{
  volatile u64 x = k;
  u64 c = x;
  x = c + 9;
  x = c + 10;
  return (int)(c + x);
}

/* Non-volatile neighbour: the overwritten def still goes. */
__attribute__((noinline)) int p_twice(int k)
{
  int x = k;
  int c = x;
  x = c + 9;
  x = c + 10;
  return c + x;
}

int main(void)
{
  printf("%d %d %d %d\n", v_twice(1), v_char(2), v_u64(3), p_twice(4));
  return 0;
}
