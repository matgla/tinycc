/*
 *  TCC Tests - Far spill reload through a pushed scratch register
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* gcc pr28982b reduced: 22 live pointers/floats over an 8 KB local put the
 * spill slots out of LDR/STR immediate range.  With every register busy, the
 * far-offset helper (th_offset_to_reg_ex) PUSHed its scratch after the
 * SP-relative offset was computed, so the access hit the slot 4 bytes low.
 * The offset is now biased by the push it makes. */
#include <stdio.h>

#define NITER 4
#define NVARS 22
#define MULTI(X) \
  X(0), X(1), X(2), X(3), X(4), X(5), X(6), X(7), X(8), X(9), X(10), \
  X(11), X(12), X(13), X(14), X(15), X(16), X(17), X(18), X(19), X(20), X(21)

#define DECLAREI(INDEX) inc##INDEX = incs[INDEX]
#define DECLAREF(INDEX) *ptr##INDEX = ptrs[INDEX], result##INDEX = 0
#define LOOP(INDEX) result##INDEX += *ptr##INDEX, ptr##INDEX += inc##INDEX
#define COPYOUT(INDEX) results[INDEX] = result##INDEX

float *ptrs[NVARS];
float results[NVARS];
int incs[NVARS];

struct big { int i[0x800]; };

void __attribute__((noinline)) bar(struct big b)
{
  incs[0] += b.i[0];
}

void __attribute__((noinline)) foo(int n)
{
  struct big b = {};
  int MULTI(DECLAREI);
  float MULTI(DECLAREF);
  while (n--)
    MULTI(LOOP);
  MULTI(COPYOUT);
  bar(b);
}

float input[NITER * NVARS];

int main(void)
{
  int i, bad = 0;
  for (i = 0; i < NVARS; i++)
    ptrs[i] = input + i, incs[i] = i;
  for (i = 0; i < NITER * NVARS; i++)
    input[i] = i;
  foo(NITER);
  for (i = 0; i < NVARS; i++)
    if (results[i] != i * NITER * (NITER + 1) / 2)
      printf("result %d: %d\n", i, (int)results[i]), bad++;
  printf("bad %d\n", bad);
  return bad != 0;
}
