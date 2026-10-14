/*
 *  TCC IR - copy_fwd relay with an inline asm frame operand
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* An object inline asm names only through its saved operands escapes with no
 * instruction (at -1); cfw_relay indexed block_of[-1] collecting the block's
 * accesses (gcc torture 20000804-1 under ASan).  Pinned: the overwrite after
 * the asm wins, and a value the asm passes through survives. */
#include <stdio.h>

__attribute__((noinline)) __complex__ long long pair(long long re, long long im)
{
  __complex__ long long v;
  __real__ v = re;
  __imag__ v = im;
  return v;
}

__attribute__((noinline)) __complex__ long long overwritten(void)
{
  __complex__ long long v = 0;
  v = pair(5, 6);
  __asm__("" : "+r"(v));
  v = 2;
  return v;
}

__attribute__((noinline)) __complex__ long long passed_through(void)
{
  __complex__ long long v = 0;
  v = pair(7, 8);
  __asm__("" : "+r"(v));
  return v;
}

int main(void)
{
  __complex__ long long a = overwritten(), b = passed_through();
  printf("overwritten %lld %lld\n", (long long)__real__ a, (long long)__imag__ a);
  printf("passed_through %lld %lld\n", (long long)__real__ b, (long long)__imag__ b);
  return 0;
}
