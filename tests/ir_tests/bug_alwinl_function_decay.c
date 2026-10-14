/*
 *  TCC IR - an always_inline function used as a value without `&`
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A function designator that decays to a pointer (return g, fp = g, an
 * argument, a static initializer) must emit the always_inline body out of
 * line like `&g` does; the unary identifier path now marks it. */

#include <stdio.h>

typedef int (*FP)(int);

static inline __attribute__((always_inline)) int g_ret(int x) { return x * 3 - 1; }
static inline __attribute__((always_inline)) int g_asg(int x) { return x + 100; }
static inline __attribute__((always_inline)) int g_arg(int x) { return x ^ 0x55; }
static inline __attribute__((always_inline)) int g_tab(int x) { return x << 2; }
static inline __attribute__((always_inline)) int g_cond(int x) { return x - 7; }
static inline __attribute__((always_inline)) int g_called(int x) { return x * x; }
static inline __attribute__((always_inline)) int g_paren(int x) { return x + 1; }

static const FP table[] = {g_tab};

__attribute__((noinline)) FP get_ret(void) { return g_ret; }
__attribute__((noinline)) int apply(FP f, int v) { return f(v); }
__attribute__((noinline)) FP pick(int c) { return c ? g_cond : (FP)0; }

int main(void)
{
  volatile FP fp;
  fp = g_asg;
  /* called (inlined) first, then decayed */
  int called = g_called(4);
  FP later = g_called;
  printf("%d\n", get_ret()(5));
  printf("%d\n", fp(1));
  printf("%d\n", apply(g_arg, 0x0f));
  printf("%d\n", table[0](3));
  printf("%d\n", pick(1)(10));
  printf("%d %d\n", called, apply(later, 6));
  printf("%d\n", (g_paren)(41));
  printf("%d\n", (*g_paren)(1));
  return 0;
}
