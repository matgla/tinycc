/*
 *  TCC IR - -fyasc-facts exclusive pointers reaching a phi
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* yfa_build (lifetime.c) scanned only the instruction array; in SSA a copy
 * of a restrict / owned root that meets another pointer at a join is a phi
 * operand, so the phi's value was neither "may derive" nor checked as an
 * escape, and ssa:load_cse kept *p over a store through it.  Phis now
 * derive from their operands. */
#pragma yasc

int printf(const char * view fmt, ...);
void * owned maybe malloc(unsigned long n);
void free(drop void * owned maybe p);

static int notes;
__attribute__((noinline)) static void note(int k)
{
  notes = notes * 10 + k;
}

/* q = c ? p : o, then a store through q between two reads of *p */
__attribute__((noinline)) static int join(inout int * restrict view p, inout int * view o, int c)
{
  int * view q;
  if (c)
  {
    q = p;
    note(1);
  }
  else
  {
    q = o;
    note(2);
  }
  int a = *p;
  *q = 5;
  return a * 100 + *p;
}

/* the same with an owned call result as the root */
__attribute__((noinline)) static int join_owned(inout int * view o, int c, int k)
{
  int * owned maybe r = malloc(16);
  if (!r)
    return -1;
  r[0] = k;
  int * view q;
  if (c)
    q = r;
  else
    q = o;
  int a = r[0];
  *q = 5;
  int res = a * 100 + r[0];
  free(r);
  return res;
}

/* a loop-carried copy: q = phi(p, q + 1) */
__attribute__((noinline)) static int carried(inout int * restrict view p, int n)
{
  int * view q = p;
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    int a = *p;
    *q = i + 10;
    s = s * 100 + a + *p;
    q = q + 1;
  }
  return s;
}

int main(void)
{
  int x = 3, y = 4;
  printf("join1 %d\n", join(&x, &y, 1));
  x = 3, y = 4;
  printf("join0 %d %d\n", join(&x, &y, 0), y);
  y = 4;
  printf("owned1 %d\n", join_owned(&y, 1, 7));
  y = 4;
  printf("owned0 %d %d\n", join_owned(&y, 0, 7), y);
  int v[4] = {1, 2, 3, 4};
  printf("carried %d\n", carried(v, 3));
  printf("v %d %d %d %d\n", v[0], v[1], v[2], v[3]);
  printf("notes %d\n", notes);
  return 0;
}
