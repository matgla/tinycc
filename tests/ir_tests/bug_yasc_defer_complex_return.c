/*
 *  TCC Tests - yasc defer with a _Complex return value
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `return z;` with defers to run settles the value first (Zig's order).
 * defer_settle_return_value loaded a _Complex lvalue into a scalar temp,
 * 8 bytes of a 16-byte value: it copies it to a temp local now, as a struct. */
#pragma yasc
extern int printf(const char * view, ...);

static volatile double vre = 1.5, vim = -2.25;
static volatile float fre = 3.0f, fim = 4.5f;
static int ran;

static double _Complex make_d(void)
{
  double _Complex z = vre + vim * 1.0i;
  defer ran++;
  return z;
}

static double _Complex changed_by_defer(void)
{
  double _Complex z = vre + vim * 1.0i;
  defer z = 0;
  return z;
}

static float _Complex make_f(void)
{
  float _Complex z = fre + fim * 1.0fi;
  defer { double _Complex w = vim + vre * 1.0i; ran += (int)__imag__ w; }
  return z;
}

static long double _Complex make_ld(int k)
{
  long double _Complex z = vre * k + vim * 1.0i;
  defer ran++;
  if (k > 1)
    return z * 2;
  return z;
}

static double _Complex from_call(void)
{
  defer ran++;
  return make_d();
}

static float _Complex f_product(void)
{
  float _Complex z = fre + fim * 1.0fi;
  defer z = 0;
  return z * z;
}

int main(void)
{
  double _Complex d = make_d();
  printf("d %.2f %.2f\n", __real__ d, __imag__ d);
  d = changed_by_defer();
  printf("changed %.2f %.2f\n", __real__ d, __imag__ d);
  float _Complex f = make_f();
  printf("f %.2f %.2f\n", (double)__real__ f, (double)__imag__ f);
  long double _Complex l = make_ld(1);
  printf("ld %.2f %.2f\n", (double)__real__ l, (double)__imag__ l);
  l = make_ld(2);
  printf("ld2 %.2f %.2f\n", (double)__real__ l, (double)__imag__ l);
  d = from_call();
  printf("call %.2f %.2f\n", __real__ d, __imag__ d);
  f = f_product();
  printf("fprod %.2f %.2f\n", (double)__real__ f, (double)__imag__ f);
  printf("ran %d\n", ran);
  return 0;
}
