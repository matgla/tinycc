/*
 *  Regression test: a call argument loaded through a register (`f(..., *p)`)
 *  must read that register before another argument move overwrites it.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The argument mover counted only plain register moves as readers, so with p
 * in an argument register (a call-crossing value kept caller-saved) it emitted
 * `mov r2, r6; ldr r3, [r2]` for f(&o, c, x, *p) -- a load through x.  Six or
 * seven loop invariants push p into r0-r3 (YasOS and test configurations); the
 * argument orders put the move over p's register before the load, and form
 * the cycle `mov r2, r0` / `ldr r0, [r2]` that needs a scratch. */
#include <stdio.h>

static int lg[4][3];

__attribute__((noinline)) int f_ocxp(int *o, void *c, int x, int v)
{
  lg[0][0] += x; lg[0][1] += v; lg[0][2] += *(int *)c;
  *o = v - x;
  return v * 3 + x;
}

__attribute__((noinline)) int f_pcox(int v, void *c, int *o, int x)
{
  lg[1][0] += x; lg[1][1] += v; lg[1][2] += *(int *)c;
  *o = v + x;
  return v * 5 - x;
}

__attribute__((noinline)) int f_pxoc(int v, int x, int *o, void *c)
{
  lg[2][0] += x; lg[2][1] += v; lg[2][2] += *(int *)c;
  *o = v ^ x;
  return v * 7 + x;
}

__attribute__((noinline)) int f_xcop(int x, void *c, int *o, int v)
{
  lg[3][0] += x; lg[3][1] += v; lg[3][2] += *(int *)c;
  *o = v | x;
  return v * 11 - x;
}

__attribute__((noinline)) int g_ocxp6(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x;
    if (x > 3)
      s += f_ocxp(&o, c, x, *p);
    s += *p + o + x;
    s ^= a + b + d + e + h + j;
  }
  return s;
}

__attribute__((noinline)) int g_pcox6(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x;
    if (x > 3)
      s += f_pcox(*p, c, &o, x);
    s += *p + o + x;
    s ^= a + b + d + e + h + j;
  }
  return s;
}

__attribute__((noinline)) int g_pxoc6(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x;
    if (x > 3)
      s += f_pxoc(*p, x, &o, c);
    s += *p + o + x;
    s ^= a + b + d + e + h + j;
  }
  return s;
}

__attribute__((noinline)) int g_xcop6(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x;
    if (x > 3)
      s += f_xcop(x, c, &o, *p);
    s += *p + o + x;
    s ^= a + b + d + e + h + j;
  }
  return s;
}

__attribute__((noinline)) int g_ocxp7(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j, int l)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x + l * k;
    if (x > 3)
      s += f_ocxp(&o, c, x, *p);
    s += *p + o + x;
    s ^= a + b + d + e + h + j + l;
  }
  return s;
}

__attribute__((noinline)) int g_pcox7(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j, int l)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x + l * k;
    if (x > 3)
      s += f_pcox(*p, c, &o, x);
    s += *p + o + x;
    s ^= a + b + d + e + h + j + l;
  }
  return s;
}

__attribute__((noinline)) int g_pxoc7(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j, int l)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x + l * k;
    if (x > 3)
      s += f_pxoc(*p, x, &o, c);
    s += *p + o + x;
    s ^= a + b + d + e + h + j + l;
  }
  return s;
}

__attribute__((noinline)) int g_xcop7(void *c, int **pp, int n, int *idx, int a, int b, int d, int e, int h, int j, int l)
{
  int s = 0;
  for (int k = 0; k < n; k++) {
    int *p = pp[k];
    int x = idx[k];
    int o = 0;
    s += a * k + b * x + d * k + e * x + h * k + j * x + l * k;
    if (x > 3)
      s += f_xcop(x, c, &o, *p);
    s += *p + o + x;
    s ^= a + b + d + e + h + j + l;
  }
  return s;
}

int main(void)
{
  static int vals[6] = {100, 200, 300, 400, 500, 600};
  int *pp[6];
  int idx[6] = {5, 1, 9, 4, 2, 7};
  int cv = 1000;
  for (int i = 0; i < 6; i++)
    pp[i] = &vals[i];

  printf("ocxp6 %d\n", g_ocxp6(&cv, pp, 6, idx, 1, 2, 3, 4, 5, 6));

  printf("pcox6 %d\n", g_pcox6(&cv, pp, 6, idx, 2, 3, 4, 5, 6, 7));

  printf("pxoc6 %d\n", g_pxoc6(&cv, pp, 6, idx, 3, 4, 5, 6, 7, 8));

  printf("xcop6 %d\n", g_xcop6(&cv, pp, 6, idx, 4, 5, 6, 7, 8, 9));

  printf("ocxp7 %d\n", g_ocxp7(&cv, pp, 6, idx, 4, 5, 6, 7, 8, 9, 1));

  printf("pcox7 %d\n", g_pcox7(&cv, pp, 6, idx, 5, 6, 7, 8, 9, 1, 2));

  printf("pxoc7 %d\n", g_pxoc7(&cv, pp, 6, idx, 6, 7, 8, 9, 1, 2, 3));

  printf("xcop7 %d\n", g_xcop7(&cv, pp, 6, idx, 7, 8, 9, 1, 2, 3, 4));

  for (int i = 0; i < 4; i++)
    printf("log%d x=%d v=%d c=%d\n", i, lg[i][0], lg[i][1], lg[i][2]);
  return 0;
}
