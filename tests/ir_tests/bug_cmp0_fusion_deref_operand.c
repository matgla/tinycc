/*
 *  TCC IR - flag-setting op standing in for a test of memory regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `p = p + 1; if (*p)`: codegen fused `adds p, #4` with the test of *p
 * (ir_codegen_cmp0_fuses rejected only a TEMP lvalue), so the branch tested
 * the pointer.  A PARAM or VAR lvalue that is not its own value is memory. */
#include <stdio.h>

static int log_v[16], log_n;
__attribute__((noinline)) void record(int v) { log_v[log_n++] = v; }

__attribute__((noinline)) void bump_test(int *p) { p = p + 1; if (*p) record(*p); }
__attribute__((noinline)) void drop_test(int *p) { p = p - 1; if (*p != 0) record(10 + *p); else record(-1); }
__attribute__((noinline)) void and_test(unsigned *p, unsigned k) { k = k & 3; if (p[k]) record(20 + (int)p[k]); }
__attribute__((noinline)) void var_test(int *p, int n)
{
  int *q = p;
  while (n--)
  {
    q = q + 1;
    if (*q == 0)
      record(30);
  }
}
/* The pointer itself: the flag-setting op may still stand in for the test. */
__attribute__((noinline)) int ptr_test(char *p) { p = p - 4; return p != 0; }
static int a[5] = {5, 0, 7, 0, 2};
static void static_bump(int *p) { p = p + 1; if (*p) { record(40 + *p); record(*p); record(0); *p = 0; } }
void bump_twice(int *p) { static_bump(p); }

int main(void)
{
  bump_test(a);
  bump_test(a + 1);
  drop_test(a + 1);
  drop_test(a + 2);
  unsigned u[4] = {0, 9, 0, 0};
  and_test(u, 4);
  and_test(u, 5);
  var_test(a, 4);
  bump_twice(a + 1);
  bump_twice(a + 1);
  static_bump(a + 3);
  printf("%d %d %d\n", ptr_test((char *)4), ptr_test((char *)8), a[2]);
  for (int i = 0; i < log_n; i++)
    printf("%d%c", log_v[i], i + 1 < log_n ? ' ' : '\n');
  return 0;
}
