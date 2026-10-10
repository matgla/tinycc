/*
 *  Regression test: static init from a member of a compound literal.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* init_putv copied the member's bytes but selected and rebased relocations
 * from the literal's start.  The window and rebase now use the member offset. */
#include <stdio.h>

struct In { int *p; int q; };
struct Out { int a; struct In in; };
struct Mid { int *first; int b; struct In in; };

int x, y;
struct In g = ((struct Out){1, {&x, 2}}).in;
struct In h = ((struct Mid){&y, 7, {&x, 3}}).in;
struct In k = ((struct Mid){&y, 7, {&x, 3}}).in;

int main(void)
{
  printf("g p_ok=%d q=%d\n", g.p == &x, g.q);
  printf("h p_ok=%d q=%d\n", h.p == &x, h.q);
  printf("k p_ok=%d q=%d\n", k.p == &x, k.q);
  return 0;
}
