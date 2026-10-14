/*
 * Regression test: an inlined GNU nested function was replayed in the scope
 * of its call, so a name declared or re-declared between its definition and
 * the call (a later local, an inner block's local or typedef, an inlined
 * sibling's local) replaced the one it captures.  Fix: what each name of its
 * body binds to is recorded at the definition, and a call where one binds
 * otherwise stays a call; a parent that keeps such a call keeps its frame
 * pointer for the static chain.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int g = 5;
typedef char T;

/* Parent inlined into main; its local g shadows the global g. */
static int t_inl(int a)
{
  int g = 1000;
  int twice(int y) { return 100 + y + g; }
  return twice(a);
}

static inline __attribute__((always_inline)) int t_alw(int a)
{
  int g = 2000;
  int add(int y) { return y + g; }
  return add(a) + add(1);
}

/* Parent not inlined. */
__attribute__((noinline)) int t_par(int a)
{
  int g = 300;
  int ng(int y) { return g + y; }
  return ng(a);
}

/* Local typedef shadowing a file-scope one. */
static int t_typedef(int a)
{
  typedef int T;
  int nt(int y) { return y * 10 + (int)sizeof(T); }
  return nt(a);
}

/* A same-scope local declared after the nested function is not its g. */
static int t_late(int a)
{
  int lg(int y) { return y + g; }
  int g = 77;
  return lg(a) * 1000 + g;
}

/* Parent inlined into a caller whose own local shadows the name too. */
static int t_deep(int a)
{
  int g = 11;
  int dn(int y) { return y * g; }
  return dn(a) + g;
}

static int t_outer(int a)
{
  int g = 9000;
  return t_deep(a) + g;
}

/* Nested in nested. */
static int t_nest2(int a)
{
  int g = 50;
  int outer(int y)
  {
    int g2 = g + 1;
    int inner(int z) { return z + g + g2; }
    return inner(y) * 2;
  }
  return outer(a);
}

/* A parent parameter shadows the global. */
static int t_param(int g)
{
  int pick(int y) { return y + g; }
  return pick(10);
}

/* A sibling with its own g calls the one that captures the parent's. */
static int t_sibling(int a)
{
  int g = 70;
  int s1(int y) { return y + g; }
  int s2(int y) { int g = 3; return s1(y) + g; }
  return s2(a);
}

/* Called again from an inner block that re-declares the captured g. */
static int t_inner(int a)
{
  int g = 300;
  int ng(void) { return g + a; }
  int r = ng();
  { int g = 9; r += ng() + g; }
  return r;
}

__attribute__((noinline)) int t_inner_par(int a)
{
  int g = 400;
  int ng(void) { return g + a; }
  int r = 0;
  { int g = 9; r += ng() + g; }
  return r;
}

/* Same-named nested functions in sequential blocks. */
static int t_blocks(int a)
{
  int r = 0;
  { int g = 1; int q(int y) { return y + g; } r += q(a); }
  { int g = 20; int q(int y) { return y * g; } r += q(a); }
  return r;
}

/* a for-loop variable shadows the parent's nested function q */
__attribute__((noinline)) int t_for(int k)
{
  int q(int x) { return x + 1000; }
  int r = 0;
  for (int q = 0; q < 3; q++) {
    int n(void) { return q + k; }
    r += n();
  }
  return r + q(1) * 100;
}

/* a block-local q shadows the parent's nested function q */
__attribute__((noinline)) int t_block_fn(int k)
{
  int q(int x) { return x + 1000; }
  int r = 0;
  {
    int q = 5 + k;
    int n(void) { return q * 2; }
    r += n();
  }
  r += q(1) * 100;
  return r;
}

/* called inside an inlined sibling whose own local or typedef shadows */
__attribute__((noinline)) int t_sib_var(int a)
{
  int g = 300;
  int ng(void) { return g + a; }
  int n(int x) { int g = 9; return ng() + g + x; }
  return n(1);
}

__attribute__((noinline)) int t_sib_typedef(int a)
{
  typedef short U;
  int ng(void) { U u = (U)a; return u; }
  int n(int x) { typedef char U; U c = 1; return ng() + c + x; }
  return n(1);
}

/* a helper inlined into an inlined nested function reads the global g */
static int Hg(int x) { return x + g; }
__attribute__((noinline)) int t_helper(int a)
{
  int g = 1000;
  int n(int x) { return Hg(x) + g; }
  return n(a);
}

int main(void)
{
  volatile int one = 1;
  printf("inlined %d\n", t_inl(one * 3));
  printf("always %d\n", t_alw(one * 4));
  printf("parent %d\n", t_par(one * 2));
  printf("typedef %d\n", t_typedef(one * 2));
  printf("late %d\n", t_late(one * 6));
  printf("deep %d\n", t_outer(one * 3));
  printf("nest2 %d\n", t_nest2(one * 4));
  printf("param %d\n", t_param(one * 7));
  printf("sibling %d\n", t_sibling(one * 2));
  printf("blocks %d\n", t_blocks(one * 2));
  printf("inner %d\n", t_inner(one * 2));
  printf("inner parent %d\n", t_inner_par(one * 2));
  printf("for %d block fn %d\n", t_for(one - 1), t_block_fn(one - 1));
  printf("sibling var %d typedef %d helper %d\n", t_sib_var(one * 2), t_sib_typedef(one * 70000), t_helper(one));
  return 0;
}
