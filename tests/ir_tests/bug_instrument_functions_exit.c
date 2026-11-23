/*
 *  TCC Tests - -finstrument-functions: the exit hook of every return
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The exit hook was emitted only at the common return point, which a
 * `return expr;` never reaches (its RETURNVALUE returns): a function returning
 * a value called __cyg_profile_func_enter only.  Each return now calls the
 * exit hook, after its value is computed.  Built with -finstrument-functions
 * by test_instrument_functions.py. */
#include <stdio.h>

static int depth, enters, exits, maxdepth;
static void *last_exit;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
  (void)fn, (void)site;
  enters++;
  if (++depth > maxdepth)
    maxdepth = depth;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site)
{
  (void)site;
  exits++;
  depth--;
  last_exit = fn;
}

__attribute__((noinline)) int h(int x)
{
  if (x)
    return 2;
  return x + 1;
}

__attribute__((noinline)) void v(int *p) { *p = 1; }

/* the value is computed before the hook runs: it reads the hook's state */
__attribute__((noinline)) int reads_depth(void) { return depth; }

__attribute__((noinline)) long long wide(int x)
{
  if (x > 3)
    return (long long)x << 33;
  return -x;
}

__attribute__((noinline)) double fp(double d) { return d * 2; }

struct S { int a, b, c; };
__attribute__((noinline)) struct S st(int x)
{
  struct S s = {x, x + 1, x + 2};
  if (x < 0)
    return (struct S){0, 0, 0};
  return s;
}

__attribute__((noinline)) int rec(int n) { return n ? rec(n - 1) + 1 : 0; }

int main(void)
{
  int k = 0;
  int a = h(1), b = h(0);
  v(&k);
  int d = reads_depth();
  long long w = wide(5) + wide(1);
  double f = fp(1.5);
  struct S s = st(4);
  int r = rec(3);
  printf("vals %d %d %d %d %lld %d %d %d\n", a, b, k, d, w, (int)f, s.a + s.b + s.c, r);
  printf("hooks %d %d depth %d max %d last %d\n", enters, exits, depth, maxdepth, last_exit == (void *)rec);
  return 0;
}
