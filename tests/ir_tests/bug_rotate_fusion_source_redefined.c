/*
 * gens rotate fusion: `(v << a) | (v >> b)` became one ROR reading v at the
 * OR, so a v written between the shifts and the OR rotated the new value.
 * The fusion now needs v unchanged from the first shift to the OR.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

__attribute__((noinline)) unsigned redef_param(unsigned v, unsigned w)
{
  unsigned a = v << 3, b = v >> 29;
  v = w;
  return (a | b) ^ v;
}

__attribute__((noinline)) unsigned redef_local(unsigned x, unsigned w)
{
  unsigned v = x * 3u;
  unsigned a = v << 8, b = v >> 24;
  v = w + 1;
  return (a | b) + v;
}

/* Must still fuse: a local rotated in place, the bench_bitwise_mix shape. */
__attribute__((noinline)) unsigned mix(int n)
{
  unsigned value = 0x13579BDFu;
  for (int k = 0; k < n; k++)
  {
    value ^= value << 7;
    value ^= value >> 9;
    value += 0x9E3779B9u;
    value = (value << 3) | (value >> 29);
  }
  return value;
}

int main(void)
{
  printf("redef_param: %08x %08x\n", redef_param(0x12345678u, 0xa5a5a5a5u), redef_param(0x80000001u, 0));
  printf("redef_local: %08x %08x\n", redef_local(0x01020304u, 7), redef_local(0x55555555u, 0xfffffffeu));
  printf("mix: %08x %08x %08x\n", mix(0), mix(1), mix(1000));
  return 0;
}
