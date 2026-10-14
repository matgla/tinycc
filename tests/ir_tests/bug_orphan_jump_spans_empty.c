/*
 *  TCC IR - a JUMP dropped as orphaned must not let an earlier JUMP fall onto another
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* codegen ir_codegen_mark_orphaned_jumps: the backward `goto head` was marked
 * orphaned, which made the then-arm's JUMP span only dropped ops, so it was
 * dropped too and fell onto `goto out` -- also marked orphaned on the
 * assumption that the then-arm's JUMP would be threaded past it -- and on into
 * the `neg` block.  Orphans are now checked against the layout the marks
 * themselves produce, to a fixed point. */
#include <stdio.h>

typedef struct Sym
{
  int v;
} Sym;

static unsigned char armed;
static Sym syms[4] = {{1}, {2}, {3}, {4}};
int calls;

__attribute__((noinline)) Sym *unseen(int v)
{
  calls++;
  return &syms[v & 3];
}

__attribute__((noinline)) int other(int v)
{
  calls += 100;
  return v * 5;
}

/* Plain goto form. */
__attribute__((noinline)) int f_goto(int v)
{
  Sym *s, *t;
  int n = 0, r;
head:
  if (++n > 3)
    return -1;
  if (v < 0)
    goto neg;
  if (v == 4)
  {
    s = &syms[0];
    goto out;
  }
  if (armed)
  {
    t = unseen(v);
    goto join;
  }
  goto head;
join:
  s = t;
  goto out;
neg:
  r = other(v);
  return r + n;
out:
  r = s->v * 3;
  return r + 7 + n;
}

/* Statement-expression form: the ternary's dead else-value leaves the phi
 * copies at the join. */
__attribute__((noinline)) int f_stmt(int v)
{
  Sym *s;
  int n = 0, r;
head:
  if (++n > 3)
    return -1;
  if (v < 0)
    goto neg;
  if (v == 4)
  {
    s = &syms[0];
    goto out;
  }
  s = armed ? unseen(v) : ({
    goto head;
    (Sym *)0;
  });
  goto out;
neg:
  r = other(v);
  return r + n;
out:
  r = s->v * 3;
  return r + 7 + n;
}

/* Two joins in a row, each a then-arm JUMP over a backward goto. */
__attribute__((noinline)) int f_twice(int v, int w)
{
  Sym *s, *t, *u;
  int n = 0, r;
head:
  if (++n > 3)
    return -1;
  if (v < 0)
    goto neg;
  if (armed)
  {
    t = unseen(v);
    goto join;
  }
  goto head;
join:
  s = t;
  if (w > 0)
  {
    u = unseen(w);
    goto join2;
  }
  goto head;
join2:
  s = u->v > s->v ? u : s;
  goto out;
neg:
  r = other(v);
  return r + n;
out:
  r = s->v * 3;
  return r + 7 + n;
}

int main(void)
{
  for (int a = 0; a < 2; a++)
  {
    armed = a;
    calls = 0;
    for (int v = -2; v < 7; v++)
      printf("%d ", f_goto(v));
    printf(" %d\n", calls);
    calls = 0;
    for (int v = -2; v < 7; v++)
      printf("%d ", f_stmt(v));
    printf(" %d\n", calls);
    calls = 0;
    for (int v = -2; v < 7; v++)
      printf("%d ", f_twice(v, v - 2));
    printf(" %d\n", calls);
  }
  return 0;
}
