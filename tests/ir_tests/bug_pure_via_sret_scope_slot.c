/*
 *  TCC IR - ipa:pure_via_sret must not write a global Sym's sym_scope
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The pure_via_sret flag lived in Sym.f, which is the sym_scope word of a
 * file-scope Sym: f looked block-scoped, so an alias of it was "undefined"
 * and a nested function of the same name was pushed below it.  The flag is
 * now in Sym.a. */

#include <stdio.h>

struct s { int a, b; };
struct t { int a, b, c; };

/* An alias of a static aggregate-returning (pure via sret) function. */
static struct s f(int x) { struct s r = {x, 1}; return r; }
struct s g(int) __attribute__((alias("f")));

static struct t f3(int x) { struct t r = {x, 2, 3}; return r; }
struct t g3(int) __attribute__((alias("f3")));

/* Declared after the alias, still the same function. */
static struct s f(int x);

/* A nested function named like a pure-via-sret file-scope one. */
struct P { int a, b, c; };
static __attribute__((noinline)) struct P mk(int x) { struct P p = {x, x, x}; return p; }
static __attribute__((noinline)) int outer_use(int v) { return mk(v).a; }
static __attribute__((noinline)) int user(int y)
{
  struct P mk(int z) { struct P p = {z + 100, 0, 0}; return p; }
  return mk(y).a;
}

int main(void)
{
  volatile int one = 1;
  printf("%d %d\n", g(3).a, g(3).b);
  printf("%d %d %d\n", g3(4).a, g3(4).b, g3(4).c);
  printf("%d\n", f(5).a + f(5).b);
  printf("%d %d\n", outer_use(one), user(one));
  return 0;
}
