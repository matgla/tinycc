/*
 *  TCC IR - asm goto target survives branch threading
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

__attribute__((noinline)) int f(unsigned *p, int x)
{
  unsigned e = 0;
  if (__atomic_compare_exchange_n(p, &e, 1u, 0, 5 /* seq_cst */, 0 /* relaxed */))
    goto got;
  __asm__ goto("cmp %0, #3\n\tbeq %l[three]" : : "r"(x) : "cc" : three);
  return (int)e;
three:
  return 300 + (int)e;
got:
  return x == 3 ? 33 : 11;
}

__attribute__((noinline)) int h(unsigned *p, int x)
{
  unsigned e = *p;
  if (e == 0)
    goto got;
  __asm__ goto("cmp %0, #3\n\tbeq %l[three]" : : "r"(x) : "cc" : three);
  return (int)e;
three:
  return 300 + (int)e;
got:
  return x == 3 ? 33 : 11;
}

int main(void)
{
  for (int x = 2; x <= 3; x++)
    for (unsigned i = 0; i < 2; i++)
    {
      unsigned w = i;
      int of = f(&w, x);
      unsigned wf = w;
      w = i;
      int oh = h(&w, x);
      printf("x=%d w=%u: f %d (w=%u) h %d\n", x, i, of, wf, oh);
    }
  return 0;
}
