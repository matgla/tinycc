/*
 *  TCC Tests - Dead-loop elimination of a rotated search loop with early exits
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ssa:dead_loop's rotated arm dropped the back-edge of a pure counting loop
 * whose body leaves through `goto`s to a far label: nothing it computed was
 * read outside, but whether the exit fires depends on the trip, so only
 * element 0 was ever scanned.  A loop exit other than the fall-through now
 * keeps the loop. */
#include <stdio.h>

struct quad
{
  int a;
  int op;
  int b;
  int c;
};

/* Enough straight-line code between the loop and the exit label that loop
 * detection does not pull the label into the loop's range. */
#define PAD(o, n)                                                                                                    \
  do                                                                                                                 \
  {                                                                                                                  \
    o[0] = n * 3 + 1;  o[1] = n * 5 + 2;  o[2] = n * 7 + 3;  o[3] = n * 9 + 4;                                     \
    o[4] = n ^ 11;     o[5] = n ^ 13;     o[6] = n ^ 17;     o[7] = n ^ 19;                                         \
    o[8] = n * 3 + 5;  o[9] = n * 5 + 6;  o[10] = n * 7 + 7; o[11] = n * 9 + 8;                                    \
    o[12] = n ^ 21;    o[13] = n ^ 23;    o[14] = n ^ 27;    o[15] = n ^ 29;                                        \
    o[16] = n * 3 + 9; o[17] = n * 5 + 10; o[18] = n * 7 + 11; o[19] = n * 9 + 12;                                 \
  } while (0)

/* two early exits to the same far label (the tcc_ir_opt_dse chain-op scan) */
__attribute__((noinline)) int scan_two(const struct quad *qs, int n, int *out)
{
  for (int i = 0; i < n; i++)
  {
    const int op = qs[i].op;
    if (op == 23 || op == 67)
      goto skip;
  }
  PAD(out, n);
  return 1;
skip:
  return 0;
}

/* one early exit */
__attribute__((noinline)) int scan_one(const struct quad *qs, int n, int *out)
{
  for (int i = 0; i < n; i++)
    if (qs[i].b < 0)
      goto neg;
  PAD(out, n);
  return 100;
neg:
  return -100;
}

/* no early exit: the loop is dead and may go, the result must not change */
__attribute__((noinline)) int scan_none(const struct quad *qs, int n, int *out)
{
  for (int i = 0; i < n; i++)
  {
    const int op = qs[i].op;
    (void)op;
  }
  PAD(out, n);
  return out[0];
}

int main(void)
{
  struct quad qs[6] = {{0, 1, 1, 0}, {0, 2, 2, 0}, {0, 3, 3, 0}, {0, 67, -1, 0}, {0, 5, 5, 0}, {0, 23, 6, 0}};
  struct quad first[2] = {{0, 23, -4, 0}, {0, 1, 1, 0}};
  int out[20];

  printf("two: %d %d %d %d %d\n", scan_two(qs, 6, out), scan_two(qs, 3, out), scan_two(qs, 4, out),
         scan_two(first, 2, out), scan_two(qs, 0, out));
  printf("one: %d %d %d %d\n", scan_one(qs, 6, out), scan_one(qs, 3, out), scan_one(first, 1, out),
         scan_one(qs, 0, out));
  printf("none: %d %d\n", scan_none(qs, 6, out), scan_none(qs, 0, out));
  out[19] = 0;
  scan_two(qs, 2, out);
  printf("pad: %d %d\n", out[0], out[19]);
  return 0;
}
