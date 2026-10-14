#include <stdio.h>

/* global_sl_fwd keeps `global = value` facts and must drop them at every join.
 * A join landing on a NOP (inlining leaves plenty) used to be skipped before
 * the reset, so the else arm read the then arm's stored value. */
unsigned h;
volatile int V0 = 0;
static void H(int v) { h = h * 31u + (unsigned)v; }
__attribute__((noinline)) void f(int k)
{
  if (V0) { H(k); } else { H(3); }
}

int g;
__attribute__((noinline)) int f2(int c, int a, int b)
{
  if (c)
    g = a;
  else
    g = b;
  return g * 2;
}

int main(void)
{
  h = 5;
  f(1);
  printf("%u\n", h);
  V0 = 1;
  f(7);
  printf("%u\n", h);
  printf("%d %d\n", f2(1, 3, 4), f2(0, 3, 4));
  return 0;
}
