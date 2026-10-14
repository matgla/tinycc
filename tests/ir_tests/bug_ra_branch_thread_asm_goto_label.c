/*
 *  TCC IR - ra:branch_thread must keep blocks only an asm goto reaches
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* With -minline-atomics, ra:branch_thread threaded the source's re-test of
 * an inline CAS's flag, then its unreachable sweep NOPed the `three:` block,
 * which only the asm goto enters (the IR records no edge for it), so the
 * label landed on `got:`.  The pass now skips functions with asm goto, as
 * dce, SSA and codegen do.  Controls: a plain load, no asm goto. */
#include <stdio.h>

/* the report's shape: the CAS's success path skips the asm goto */
__attribute__((noinline)) int cas_label(unsigned *p, int x)
{
  unsigned e = 0;
  if (__atomic_compare_exchange_n(p, &e, 1u, 0, 5, 0))
    goto got;
  __asm__ goto("cmp %0, #3\n\tbeq %l[three]" : : "r"(x) : "cc" : three);
  return (int)e;
three:
  return 300 + (int)e;
got:
  return x == 3 ? 33 : 11;
}

/* the label block also entered by a plain goto */
__attribute__((noinline)) int cas_label_shared(unsigned *p, int x)
{
  unsigned e = 0;
  if (__atomic_compare_exchange_n(p, &e, 2u, 0, 5, 0))
    goto got;
  if (x == 9)
    goto three;
  __asm__ goto("cmp %0, #3\n\tbeq %l[three]" : : "r"(x) : "cc" : three);
  return (int)e;
three:
  *p += 10;
  return 400 + (int)e;
got:
  return x == 3 ? 44 : 22;
}

/* two labels, one of them after the success path */
__attribute__((noinline)) int cas_two_labels(unsigned *p, int x)
{
  unsigned e = 0;
  if (__atomic_compare_exchange_n(p, &e, 3u, 0, 5, 0))
    goto got;
  __asm__ goto("cmp %0, #3\n\tbeq %l[three]\n\tcmp %0, #4\n\tbeq %l[four]" : : "r"(x) : "cc" : three, four);
  return (int)e;
three:
  return 500 + (int)e;
got:
  return x == 3 ? 55 : 66;
four:
  return 600 + (int)e;
}

/* control: the same CFG with a plain load and compare */
__attribute__((noinline)) int plain_label(unsigned *p, int x)
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

/* control: no asm goto, the CAS re-test still threads */
__attribute__((noinline)) int cas_no_asm(unsigned *p, int x)
{
  unsigned e = 0;
  if (__atomic_compare_exchange_n(p, &e, 1u, 0, 5, 0))
    goto got;
  if (x == 3)
    return 300 + (int)e;
  return (int)e;
got:
  return x == 3 ? 33 : 11;
}

int main(void)
{
  for (int x = 2; x <= 4; x++)
    for (unsigned i = 0; i < 2; i++)
    {
      unsigned w = i;
      int a = cas_label(&w, x);
      unsigned wa = w;
      w = i;
      int b = cas_label_shared(&w, x);
      unsigned wb = w;
      w = i;
      int c = cas_two_labels(&w, x);
      unsigned wc = w;
      w = i;
      int d = plain_label(&w, x);
      w = i;
      int e = cas_no_asm(&w, x);
      printf("x=%d w=%u: %d(%u) %d(%u) %d(%u) %d %d\n", x, i, a, wa, b, wb, c, wc, d, e);
    }
  unsigned w = 1;
  int s = cas_label_shared(&w, 9);
  printf("shared goto: %d w=%u\n", s, w);
  return 0;
}
