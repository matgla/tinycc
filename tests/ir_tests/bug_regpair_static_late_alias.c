/*
 *  TCC IR - r0/r1 aggregate returns: an alias declared after a direct call
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `f` is called before the declaration that makes it an alias of `g`: at -O0
 * use() is emitted first, so it must not have taken f's 8-byte aggregate in
 * r0/r1 (gfunc_regpair_ret) -- the alias is AAPCS. */

#include <stdio.h>

typedef struct { int a, b; } P;

static P __attribute__((noinline)) g(int x) { P p = {x + 1, x + 2}; return p; }
static P f(int x);
int __attribute__((noinline)) use(int x) { P r = f(x); return r.a * 100 + r.b; }
static P f(int x) __attribute__((alias("g")));
int __attribute__((noinline)) useg(int x) { P r = g(x); return r.a * 100 + r.b; }

int main(void)
{
  printf("%d %d\n", use(3), useg(3));
  return 0;
}
