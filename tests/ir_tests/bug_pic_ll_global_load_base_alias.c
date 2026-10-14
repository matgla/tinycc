/*
 *  TCC IR - a 64-bit global read keeps its address apart from the low word
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* tcc_gen_machine_assign_mop_ex loads a 64-bit lvalue as [base] / [base+4].
 * Under -fpic at -O0 (r9 reserved, r10 the static chain) the address scratch
 * was the low half's own register, saved around its use: `push {r0}; ldr r0,
 * <&GA[1]>; ldr r0,[r0]; ldr ip,[r0,#4]; pop {r0}` -- high word read through
 * the low word, low word popped away (HardFault on YasOS).  Fix: the address
 * scratch excludes the destination registers. */
int printf(const char *fmt, ...);

long long GA[4] = {0x100000001LL, 0x200000002LL, 0x300000003LL, 0x400000004LL};

__attribute__((noinline)) long long p(int a)
{
  long long cap = 1000;
  int ci = a;
  __attribute__((noinline)) long long nf(int y)
  {
    long long a0 = GA[0] * y, a1 = GA[1] * y, a2 = GA[2] * y, a3 = GA[3] * y;
    long long s = 0;
    for (int i = 0; i < y; i++)
    {
      s += a0 ^ a1;
      s += a2 * a3 + cap;
      s += ci;
      a0++;
      cap++;
    }
    return s + a0 + a1 + a2 + a3 + cap;
  }
  return nf(a + 3);
}

int main(void)
{
  volatile int one = 1;
  printf("p %lld\n", p(one));
  printf("GA1 %lld\n", GA[1]);
  return 0;
}
