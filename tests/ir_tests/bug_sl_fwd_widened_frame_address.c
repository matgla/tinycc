/*
 *  TCC IR - a frame object's address widened to 64 bits is an address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `(unsigned long long)(unsigned)a` materialises a's address as
 * `T <- Addr[StackLoc[x]] [LOAD]`; sl_forward took that for a read of the slot
 * and forwarded a[0] in its place (-O1+).  A local non-lval source is the
 * address: no memory read to answer.  The plain slot reads must still forward. */

#include <stdio.h>

#define NI __attribute__((noinline))

NI int first_of(unsigned long long packed)
{
  const int *p = (const int *)(unsigned)packed;
  return p[0] * 100 + p[1] * 10 + (int)(packed >> 32);
}

NI int via_widened(void)
{
  int a[2] = {7, 8};
  return first_of((unsigned long long)(unsigned)a | (3ULL << 32));
}

NI unsigned long long widened_struct_member(void)
{
  struct { int pad; int v[2]; } s = {1, {4, 5}};
  unsigned long long w = (unsigned long long)(unsigned)s.v;
  return (unsigned long long)(*(int *)(unsigned)w == 4) + ((unsigned long long)(*(int *)(unsigned)w + 1 == 5) << 32);
}

NI int plain_slot_forward(void)
{
  int x = 41; /* a scalar read back: still forwarded */
  x = x + 1;
  return x;
}

int main(void)
{
  unsigned long long r = widened_struct_member();
  printf("%d\n", via_widened());
  printf("%u %u\n", (unsigned)r, (unsigned)(r >> 32));
  printf("%d\n", plain_slot_forward());
  return 0;
}
