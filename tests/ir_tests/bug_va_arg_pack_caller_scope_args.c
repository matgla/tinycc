/*
 *  TCC Tests - __builtin_va_arg_pack() arguments evaluated in the caller
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The va_arg_pack clone pasted the call's variadic argument TOKENS into the
 * wrapper's body: an argument naming a local was refused (link error), and a
 * spliced name re-resolved in the wrapper's scope.  The clone now takes the
 * variadic arguments as extra parameters, parsed and evaluated at the call. */
#include <stdio.h>
#include <stdarg.h>

static const int G = 7;
enum { LK = 1000 };
int evals;

__attribute__((noinline)) int sum(int n, ...)
{
  va_list ap;
  int s = 0;
  va_start(ap, n);
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int);
  va_end(ap);
  return s;
}
static int ssum(int n, ...)
{
  va_list ap;
  int s = 0;
  va_start(ap, n);
  for (int i = 0; i < n; i++)
    s = s * 10 + va_arg(ap, int);
  va_end(ap);
  return s;
}
__attribute__((noinline)) double mix(const char *fmt, ...)
{
  va_list ap;
  double s = 0;
  va_start(ap, fmt);
  for (; *fmt; fmt++)
  {
    if (*fmt == 'i')
      s = s * 10 + va_arg(ap, int);
    else if (*fmt == 'd')
      s = s * 10 + va_arg(ap, double);
    else if (*fmt == 'l')
      s = s * 10 + (double)va_arg(ap, long long);
    else if (*fmt == 's')
      s = s * 10 + va_arg(ap, const char *)[0] - '0';
  }
  va_end(ap);
  return s;
}
__attribute__((noinline)) int next_val(int v) { evals++; return v; }

static inline __attribute__((always_inline)) int wrap(int n, ...) { return sum(n, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wraps(int n, ...) { return ssum(n, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wrapg(int n, ...)
{
  int G = 100;
  return sum(n, __builtin_va_arg_pack()) + G - 100;
}
static inline __attribute__((always_inline)) double wrapm(const char *fmt, ...) { return mix(fmt, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wrap0(int n, ...) { return sum(n, __builtin_va_arg_pack()) + 1; }

__attribute__((noinline)) int t_local(void) { int x = 7; return wrap(1, x); }
__attribute__((noinline)) int t_param(int y) { return wrap(2, y, y + 1); }
__attribute__((noinline)) int t_capt(void) { return wrapg(1, G); }
__attribute__((noinline)) int t_enum(void) { enum { LK = 42 }; return wrap(1, LK); }
__attribute__((noinline)) int t_once(int i) { int r = wrap(2, i++, 1); return r * 100 + i; }
__attribute__((noinline)) int t_order(void) { return wraps(3, next_val(1), next_val(2), next_val(3)); }
__attribute__((noinline)) double t_mix(void)
{
  char c = 4;
  short s = 5;
  float f = 6.0f;
  long long ll = 7;
  int arr[2] = { 8, 9 };
  return wrapm("iidlsi", c, s, f, ll, "3", arr[1]);
}
__attribute__((noinline)) int t_empty(void) { return wrap0(0); }
__attribute__((noinline)) int t_static(int y) { return wraps(2, y, 4); }
__attribute__((noinline)) int t_const(void) { return wrap(2, 30, 12); }

int main(void)
{
  printf("%d %d %d %d\n", t_local(), t_param(3), t_capt(), t_enum());
  int o = t_once(5);
  int ord = t_order();
  printf("%d %d evals=%d\n", o, ord, evals);
  printf("%.0f %d %d %d\n", t_mix(), t_empty(), t_static(2), t_const());
  return 0;
}
