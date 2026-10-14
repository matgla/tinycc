/*
 *  TCC Tests - GNU nested functions stay out of the by-token IPA caches
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The constant-result, switch-snapshot and purity caches were keyed by the
 * function's token, which nested functions of different parents and a
 * file-scope function share: at -O2 each nested q(3) folded to the first
 * cached q's result.  func_cache_token() never caches a nested function. */
#include <stdio.h>

#define NI __attribute__((noinline))

int f1(void)
{
  NI int q(int y) { return y + 1000; }
  return q(3);
}
int f2(void)
{
  NI int q(int y) { return y * 2; }
  return q(3);
}
int f3(void)
{
  NI int q(int y)
  {
    switch (y)
    {
    case 3: return 77;
    default: return 1;
    }
  }
  return q(3);
}
int f4(void)
{
  NI int q(int y) { return 5; }
  return q(3);
}
int f5(void)
{
  NI int q(int y) { return 9; }
  return q(3);
}

/* a file-scope r (constant result) and a nested r */
NI int r(int y) { return 1000000; }
NI int f7(void) { return r(2); }
int f6(void)
{
  NI int r(int y) { return y + 1000; }
  return r(2);
}

/* a file-scope switch function, then a nested one of the same name */
NI int sw(int y)
{
  switch (y)
  {
  case 1: return 11;
  case 2: return 22;
  case 3: return 33;
  default: return 0;
  }
}
NI int f8(void) { return sw(2); }
int f9(void)
{
  NI int sw(int y)
  {
    switch (y)
    {
    case 1: return 101;
    case 2: return 202;
    case 3: return 303;
    default: return 7;
    }
  }
  return sw(2) + sw(9);
}

/* the file-scope functions keep their cached facts */
NI int k(void) { return 42; }
int f10(void) { return k() + k(); }

int main(void)
{
  printf("%d %d %d %d %d %d %d\n", f1(), f2(), f3(), f4(), f5(), f6(), f7());
  printf("%d %d %d\n", f8(), f9(), f10());
  return 0;
}
