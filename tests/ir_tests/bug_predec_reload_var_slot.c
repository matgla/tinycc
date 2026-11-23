/*
 *  TCC Tests - pre-increment's reload of a variable from its own slot
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* inc() reloads a pre-incremented variable with the stored value's c.i as its
 * frame offset; a sanitized add's result carries another, so at -O0 (where a
 * variable lives in its frame slot) `t[--i]` read i from inside t.  The reload
 * takes the variable's own offset.  test_sanitize.py runs this with
 * -fsanitize=signed-integer-overflow; plain, it pins the unsanitized code. */
#include <stdio.h>

__attribute__((noinline)) void h(unsigned long long v, char *out)
{
  char t[24];
  int i = 23;
  do
    t[--i] = (char)('0' + v % 10);
  while ((v /= 10) && i > 0);
  out[0] = t[i];
  out[1] = t[22];
  out[2] = 0;
}

__attribute__((noinline)) int fi(int n)
{
  int t[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  int i = n;
  int s = 0;
  do
    s += t[--i];
  while (i > 0);
  return s;
}

__attribute__((noinline)) float ff(float x)
{
  char pad[16];
  pad[0] = 1;
  float f = x;
  float g = --f;
  return g + f + pad[0];
}

__attribute__((noinline)) double fd(double x)
{
  char pad[16];
  pad[0] = 1;
  double f = x;
  double g = --f;
  return g + f + pad[0];
}

__attribute__((noinline)) long long fl(long long x)
{
  char pad[16];
  pad[0] = 1;
  long long f = x;
  long long g = --f;
  return g + f + pad[0];
}

__attribute__((noinline)) int fp(int *q)
{
  char pad[16];
  pad[0] = 1;
  int *p = q + 2;
  int *r = --p;
  return *r + *p + pad[0];
}

__attribute__((noinline)) int fs(short x)
{
  char pad[16];
  pad[0] = 1;
  short s = x;
  int g = --s;
  return g + s + pad[0];
}

__attribute__((noinline)) int fc(unsigned char x)
{
  char pad[16];
  pad[0] = 1;
  unsigned char s = x;
  int g = ++s;
  return g + s + pad[0];
}

int main(void)
{
  char out[3];
  int a[3] = {10, 20, 30};
  h(9876, out);
  printf("h %s\n", out);
  printf("int %d\n", fi(8));
  printf("float %d double %d\n", (int)ff(5.0f), (int)fd(5.0));
  printf("llong %lld\n", fl(5));
  printf("ptr %d\n", fp(a));
  printf("short %d char %d\n", fs(-32768), fc(255));
  return 0;
}
