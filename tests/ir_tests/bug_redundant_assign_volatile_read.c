/*
 *  TCC IR - redundant_assign must keep a redefined VAR's volatile reads
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* redundant_assign NOPed `d = x` when d was redefined before any read, even
 * when the def itself read a volatile local, so `d = x; d = x; d = x;` did
 * one volatile load at -O1+.  A def with a volatile access is never pending
 * now.  test_codegen_asm.py counts the loads; plain copies still collapse. */

#include <stdio.h>

#define NI __attribute__((noinline))

NI int vol_scalar(int v)
{
  volatile int x = v;
  int d;
  d = x;
  d = x;
  d = x;
  return d;
}

NI int vol_ptr(int v)
{
  int b = v;
  int *volatile vp = &b;
  int *d;
  d = vp;
  d = vp;
  d = vp;
  return *d;
}

NI int vol_dest(int a)
{
  volatile int d;
  d = a;
  d = a + 1;
  d = a + 2;
  return d;
}

NI int plain_copies(int *p)
{
  int d;
  d = p[0];
  d = p[1];
  d = p[2];
  return d;
}

int main(void)
{
  int arr[3] = {4, 5, 6};
  printf("%d %d %d %d\n", vol_scalar(7), vol_ptr(8), vol_dest(9), plain_copies(arr));
  return 0;
}
