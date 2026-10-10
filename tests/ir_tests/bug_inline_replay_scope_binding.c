/*
 * Regression test: token-replay inlining must bind a callee's free names
 * (globals, struct tags, enum constants, statics) at the callee's
 * definition, not in the caller's scope.  Fix: always_inline and const-eval
 * routes run the shadow/static/__func__ checks, extended to struct tags.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

int g = 100;
struct S { int a, b; };
enum { K = 7 };

static inline __attribute__((always_inline)) int readg(void) { return g; }
static inline __attribute__((always_inline)) int counter(void) { static int n; return ++n; }
static inline __attribute__((always_inline)) int ssz(void) { return sizeof(struct S); }

static int ssz2(void) { return sizeof(struct S); }
static int kk(void) { return K; }
static int rg2(void) { return g; }

int user(void)
{
  struct S { char x; };
  struct S v;
  v.x = 0;
  return ssz2() + (int)sizeof(v);
}

int user2(void)
{
  enum { K = 3 };
  return kk() * 10 + K;
}

int user3(void)
{
  int g = 5;
  return rg2() + g;
}

int main(void)
{
  int g = 5;
  int c1 = counter();
  int c2 = counter();
  struct S { char x; } s;
  (void)s;
  printf("%d %d %d\n", readg(), c1, c2);
  printf("%d\n", ssz());
  printf("%d %d %d\n", user(), user2(), user3());
  /* unshadowed neighbours keep working */
  printf("%d %d\n", ssz2(), kk());
  return g - 5;
}
