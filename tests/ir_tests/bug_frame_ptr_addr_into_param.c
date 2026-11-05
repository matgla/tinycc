/*
 *  TCC IR - A local's address stored into a parameter variable, then passed on
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The shared frame points-to facts (opt/analysis/frame_ptr.c) once took
 * `P1 <-- T [STORE]`, a copy of &s into a PARAM vreg, for a tracked copy: a
 * parameter is no node of the facts, so the address left them unobserved and
 * dse deleted s's initialiser store under the call that read it (tinycc's own
 * decl_designator).  A copy into anything the facts do not follow is an
 * escape. */
#include <stdio.h>

struct S
{
  int a, b;
};

__attribute__((noinline)) int use(const struct S *p)
{
  return p->a * 100 + p->b;
}

__attribute__((noinline)) int pick(struct S *p, int k)
{
  struct S s;
  s.a = 7;
  s.b = k;
  if (k > 2)
    p = &s;
  return use(p);
}

/* The same through a pointer parameter that is overwritten in a loop. */
__attribute__((noinline)) int pick_loop(struct S *p, int n)
{
  struct S s;
  s.a = 9;
  s.b = 1;
  for (int i = 0; i < n; i++)
    if (i == 1)
      p = &s;
  return use(p);
}

int main(void)
{
  struct S g = {1, 2};
  printf("%d %d\n", pick(&g, 1), pick(&g, 5));
  printf("%d %d\n", pick_loop(&g, 0), pick_loop(&g, 3));
  return 0;
}
