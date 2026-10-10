/*
 *  TCC LICM - Invariants exposed by SSA promotion
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

__attribute__((noinline)) unsigned promoted(volatile unsigned *out, unsigned a, unsigned n)
{
  unsigned x;
  for (unsigned i = 0; i < n; ++i) {
    x = a;
    x += 17;
    x >>= 2;
    out[i] = x;
  }
  return n;
}

__attribute__((noinline)) void touch(volatile unsigned *out)
{
  *out += 1;
}

__attribute__((noinline)) unsigned promoted_call(volatile unsigned *out, unsigned a, unsigned n)
{
  unsigned x;
  for (unsigned i = 0; i < n; ++i) {
    x = a;
    x += 17;
    x >>= 2;
    out[i] = x;
    touch(&out[i]);
  }
  return n;
}

__attribute__((noinline)) unsigned retained_source(volatile unsigned *out, unsigned a, unsigned n)
{
  unsigned x;
  for (unsigned i = 0; i < n; ++i) {
    x = a;
    x >>= 2;
    out[i] = x;
    touch(&out[i]);
    out[i] += a;
  }
  return n;
}

int main(void)
{
  unsigned out[3] = {99, 99, 99};
  printf("%u ", promoted(out, 63, 0));
  printf("%u ", out[0]);
  printf("%u ", promoted(out, 63, 3));
  printf("%u %u %u\n", out[0], out[1], out[2]);
  printf("%u ", promoted_call(out, 63, 3));
  printf("%u %u %u\n", out[0], out[1], out[2]);
  printf("%u ", retained_source(out, 63, 3));
  printf("%u %u %u\n", out[0], out[1], out[2]);
  return 0;
}
