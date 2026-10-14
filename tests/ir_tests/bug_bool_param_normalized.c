/*
 *  TCC Tests - Implicit conversion to prototyped _Bool parameters
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

#define NOINLINE __attribute__((noinline))

NOINLINE int take(_Bool b)
{
  return b * 5;
}

#define PASSERS(name, type) \
  NOINLINE int name(type x) { return take(x); } \
  NOINLINE int name##_indirect(int (*f)(_Bool), type x) { return f(x); } \
  NOINLINE int name##_non_tail(type x) { return take(x) + 7; }

PASSERS(pass_int, int)
PASSERS(pass_ptr, void *)
PASSERS(pass_float, float)
PASSERS(pass_double, double)
PASSERS(pass_ll, long long)

NOINLINE int take_five(_Bool a, _Bool b, _Bool c, _Bool d, _Bool e)
{
  return a + 2 * b + 4 * c + 8 * d + 16 * e;
}

NOINLINE int pass_five(int x, void *p, double d)
{
  return take_five(x, p, d, x && p, d);
}

NOINLINE int pass_five_indirect(int (*f)(_Bool, _Bool, _Bool, _Bool, _Bool),
                              int x, void *p, double d)
{
  return f(x, p, d, x && p, d);
}

NOINLINE int pass_cast(int x)
{
  return take((_Bool)x);
}

NOINLINE int pass_local(int x)
{
  _Bool b = x;
  return take(b);
}

#define CHECK(expr, expected) do { \
  int actual = (expr); \
  if (actual != (expected)) { \
    printf("%s: got %d, expected %d\n", #expr, actual, (expected)); \
    ++bad; \
  } \
} while (0)

#define CHECK_VALUE(name, value, expected) do { \
  CHECK(name(value), expected); \
  CHECK(name##_indirect(take, value), expected); \
  CHECK(name##_non_tail(value), (expected) + 7); \
} while (0)

int main(void)
{
  int bad = 0;
  CHECK_VALUE(pass_int, 300, 5);
  CHECK_VALUE(pass_int, 256, 5);
  CHECK_VALUE(pass_int, -1, 5);
  CHECK_VALUE(pass_int, 0, 0);
  CHECK_VALUE(pass_ptr, (void *)0x100, 5);
  CHECK_VALUE(pass_ptr, (void *)0, 0);
  CHECK_VALUE(pass_float, 0.0f, 0);
  CHECK_VALUE(pass_float, -0.0f, 0);
  CHECK_VALUE(pass_float, 0.5f, 5);
  CHECK_VALUE(pass_float, -0.5f, 5);
  CHECK_VALUE(pass_double, 0.0, 0);
  CHECK_VALUE(pass_double, -0.0, 0);
  CHECK_VALUE(pass_double, 0.5, 5);
  CHECK_VALUE(pass_double, -0.5, 5);
  CHECK_VALUE(pass_ll, 0LL, 0);
  CHECK_VALUE(pass_ll, 1LL << 32, 5);
  CHECK_VALUE(pass_ll, -1LL, 5);
  CHECK(pass_five(256, (void *)0x100, 0.5), 31);
  CHECK(pass_five(0, (void *)0, 0.0), 0);
  CHECK(pass_five(256, (void *)0, 0.0), 1);
  CHECK(pass_five_indirect(take_five, 256, (void *)0x100, 0.5), 31);
  CHECK(pass_five_indirect(take_five, 0, (void *)0, 0.0), 0);
  CHECK(pass_five_indirect(take_five, 0, (void *)0x100, 0.0), 2);
  CHECK(take(256), 5);
  CHECK(take(0.0), 0);
  CHECK(take(0.5), 5);
  CHECK(take((void *)0x100), 5);
  CHECK(pass_cast(256), 5);
  CHECK(pass_local(256), 5);
  (void)sizeof(take(++bad));
  printf("bool arguments: %d failures\n", bad);
  return bad != 0;
}
