/*
 *  TCC IR - const_local_table: an address copied into a named pointer escapes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* const_local_table took `V0 <- T0 ADD #imm` for a derivation into a tracked
 * TEMP because its TEMP table is indexed by vreg position only; the local
 * went to .rodata and the write through V0 was lost.  Only a TEMP dest is a
 * derivation now; a VAR or PARAM dest is an escape. */
#include <stdio.h>

#define NI __attribute__((noinline))

struct R
{
  int x, y;
};

/* T0 = &r; V0 = T0 + 4; *V0 = 9 */
NI int interior(void)
{
  struct R r = {1, 2};
  int *p = (int *)&r + 1;
  *p = 9;
  return r.y;
}

/* one-past-end pointers merged by a branch */
NI int one_past(int c)
{
  struct R r1 = {1, 2}, r2 = {3, 4};
  struct R *e = &r1 + 1;
  if (c)
    e = &r2 + 1;
  e[-1].y = 9;
  return r1.y * 10 + r2.y;
}

/* the TEMP numbering shifted: never aliased, must stay correct */
NI int control(int k)
{
  int z = k * 3 + 1;
  z = z * 5 + k;
  struct R r = {1, 2};
  struct R *e = &r + 1;
  e[-1].y = 9;
  return r.y + z - z;
}

/* a PARAM dest at the TEMP's position */
NI int param_dest(int *p)
{
  struct R r = {1, 2};
  p = (int *)&r + 1;
  *p = 9;
  return r.y;
}

/* ADD then SUB into the pointer variable */
NI int sub_dest(void)
{
  struct R r = {1, 2};
  int *p = (int *)(&r + 1) - 1;
  *p = 9;
  return r.y;
}

/* an array written through a derived pointer, read by a dynamic index */
NI int array_dest(int i)
{
  int t[3] = {5, 6, 7};
  int *p = t + 2;
  *p = 70;
  return t[2] * 100 + t[i];
}

/* written through a pointer that walks the array in a loop */
NI int loop_dest(int n)
{
  int t[4] = {1, 2, 3, 4};
  int *p = t + 1;
  for (int k = 0; k < n; k++)
    *p++ += 10;
  return t[0] * 1000 + t[1] * 100 + t[2] * 10 + t[3];
}

/* read-only: still a constant table */
NI int lookup(int i)
{
  int t[4] = {10, 20, 30, 40};
  return t[i];
}

int main(void)
{
  int dummy = 0;
  printf("%d %d %d %d\n", interior(), one_past(0), one_past(1), control(1));
  printf("%d %d %d %d\n", param_dest(&dummy), sub_dest(), array_dest(0), array_dest(2));
  printf("%d %d %d\n", loop_dest(2), lookup(1), lookup(3));
  printf("%d\n", dummy);
  return 0;
}
