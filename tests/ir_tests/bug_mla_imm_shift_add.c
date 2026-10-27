/*
 * MLA with an immediate multiplier 2^k, 2^k + 1 or 2^k - 1 is emitted as a
 * shift-add (`add/rsb rd, x, x, lsl #k`) plus the accumulator; every form,
 * accumulators that do and do not encode as an add immediate, and a
 * register accumulator must still compute x * m + c.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

#define F(name, m, c)                                                                                             \
  __attribute__((noinline)) unsigned name(unsigned x) { return x * (m) + (unsigned)(c); }
F(m2, 2, 1)
F(m3, 3, 7)
F(m5, 5, -4000)
F(m7, 7, 0)
F(m8, 8, 4095)
F(m9, 9, 100000)
F(m15, 15, -1)
F(m16, 16, 0x12340000)
F(m17, 17, 255)
F(m31, 31, -65536)
F(m255, 255, 1)
F(m257, 257, 3)
F(mbig, 1073741825, 5)
F(m6, 6, 7)

__attribute__((noinline)) unsigned with_reg(unsigned x, unsigned a) { return x * 9 + a; }

__attribute__((noinline)) unsigned loop_sum(int n)
{
  unsigned s = 0;
  for (int i = 0; i < n; i++)
  {
    int v = i * 3 + 7;
    if (i & 1)
      v++;
    s = s * 31 + (unsigned)v;
  }
  return s;
}

int main(void)
{
  unsigned xs[] = {0, 1, 0xffffffffu, 12345, 0xfffe7e33u, 0x7fffffffu, 0x80000000u};
  for (int k = 0; k < 7; k++)
  {
    unsigned x = xs[k];
    printf("%08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", x, m2(x), m3(x), m5(x), m7(x), m8(x), m9(x),
           m15(x), m16(x), m17(x), m31(x), m255(x), m257(x), mbig(x), m6(x), with_reg(x, x ^ 0x5a5au));
  }
  printf("loop_sum: %u %u\n", loop_sum(0), loop_sum(100));
  return 0;
}
