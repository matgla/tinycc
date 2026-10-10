/*
 * Inlined struct return: an earlier return copied into the slot, then a later
 * `return <local>;` redirected the slot to that local, so the caller copied it
 * over every path (block.c).  The slot may be redirected only before any return wrote it.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

typedef struct Op
{
  int a;
  int b;
  unsigned char c;
} Op;

#define OP_NONE ((Op){.a = -1, .b = 0, .c = 0})

static Op pool[8];

__attribute__((noinline)) Op get(int i) { return pool[i]; }

/* deref return, call return, then a compound-literal local last: the reported shape */
static Op slot(int s, int k)
{
  if (s == 0)
    return k ? get(k) : OP_NONE;
  if (s == 1)
    return pool[k];
  return OP_NONE;
}

/* a named local returned after a copied return */
static Op pick(int s, int k)
{
  if (s)
    return pool[k];
  Op none = {-2, 0, 0};
  return none;
}

/* local first, then a copied return: the case the first-return rule already covers */
static Op local_first(int s, int k)
{
  Op x = {k * 10, k, 1};
  if (s)
    return x;
  return get(k);
}

/* every return a local */
static Op two_locals(int s, int k)
{
  Op x = {k, 1, 2};
  Op y = {-k, 3, 4};
  if (s)
    return x;
  return y;
}

static void show(const char *name, Op o)
{
  printf("%s a=%d b=%d c=%d\n", name, o.a, o.b, o.c);
}

int main(void)
{
  for (int i = 0; i < 8; i++)
    pool[i] = (Op){i + 1, 100 + i, (unsigned char)(i * 3)};
  for (int s = 0; s < 3; s++)
    for (int k = 0; k < 3; k += 2)
    {
      Op o = slot(s, k);
      printf("slot(%d,%d) ", s, k);
      show("", o);
    }
  for (int s = 0; s < 2; s++)
  {
    show(s ? "pick1" : "pick0", pick(s, 5));
    show(s ? "local_first1" : "local_first0", local_first(s, 4));
    show(s ? "two_locals1" : "two_locals0", two_locals(s, 7));
  }
  return 0;
}
