/*
 *  TCC IR - entry_store_prop and a folded read beside an immediate
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* entry_store_prop Phase 3c forwards an entry store into a read folded
 * into arithmetic.  Its "no #a OP #b" guard covered only the first operand:
 * `5 - a[0]` (`T <- #5 SUB StackLoc`) became `#5 SUB #3`, two immediates
 * left for later folds.  Beside an immediate the result is now folded to
 * a constant there (CMP and symbols left alone). */

#include <stdio.h>

static int g;

__attribute__((noinline)) static void touch(void)
{
  g++;
}

__attribute__((noinline)) static int sub_imm(int n)
{
  int a[4];
  a[0] = 3;
  a[1] = 9;
  touch();
  for (int i = 0; i < n; i++)
    touch();
  return (5 - a[0]) * 100 + (100 - a[1]) + (7 ^ a[0]) * 1000 + (6 & a[1]) * 100000;
}

__attribute__((noinline)) static int cmp_imm(int n)
{
  int a[4];
  a[0] = 3;
  touch();
  for (int i = 0; i < n; i++)
    touch();
  return (2 < a[0]) + (4 < a[0]) * 10;
}

/* the read beside a register still takes the stored value */
__attribute__((noinline)) static int sub_reg(int n, int k)
{
  int a[4];
  a[0] = 3;
  touch();
  for (int i = 0; i < n; i++)
    touch();
  return k - a[0];
}

int main(void)
{
  printf("sub_imm %d\n", sub_imm(2));
  printf("cmp_imm %d\n", cmp_imm(1));
  printf("sub_reg %d\n", sub_reg(1, 50));
  printf("g %d\n", g);
  return 0;
}
