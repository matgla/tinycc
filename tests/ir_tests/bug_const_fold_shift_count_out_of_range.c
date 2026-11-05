/*
 *  TCC IR - A constant shift count outside [0, width) stays a shift
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Three passes folded constant integer binops with three private tables.
 * const_prop_tmp and known_bits refused a shift count outside [0, width) (the
 * program's UB: the ARM shift runs with the count's low byte, so 1 << 40 is
 * 0, where x86 would mask the count to 8); value_tracking masked the count
 * and folded `1u << 40` to 256, `0x80000000u >> 33` to 0x40000000.  One
 * shared table now, and -O2 keeps what -O0 does. */
#include <stdio.h>

__attribute__((noinline)) unsigned f(void)
{
  unsigned a = 1;
  unsigned s = 40;
  return a << s;
}

__attribute__((noinline)) unsigned g(void)
{
  unsigned a = 0x80000000u;
  unsigned s = 33;
  return a >> s;
}

__attribute__((noinline)) int h(void)
{
  int a = -8;
  int s = 35;
  return a >> s;
}

__attribute__((noinline)) unsigned k(unsigned x)
{
  unsigned s = 32;
  return (x | 1u) << s;
}

int main(void)
{
  printf("%u %u %d %u\n", f(), g(), h(), k(5));
  return 0;
}
