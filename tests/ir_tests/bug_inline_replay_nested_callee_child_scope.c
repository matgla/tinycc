/*
 * Regression test: a GNU nested function `middle` that defines its own nested
 * function `inner`, token-replayed into its parent, left `inner` bound in the
 * parent after the replay, so the parent's later `inner(1)` called middle's
 * child instead of the file-scope inner (-O1+).  Fixed in the round-5
 * combination: a nested function's name binds in its defining block
 * (nested_define / prev_scope), and the replay's block end unbinds it.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

int inner(int j) { return j + 5000; }
int leaf(int j) { return j * 7; }

int t(int a)
{
  int p = 3;
  int middle(int k)
  {
    int inner(int j) { return j + p; }
    return inner(k) * 2;
  }
  int r = middle(a);
  return r + inner(1);
}

/* Two levels, the child shadowing a second file-scope name, and a sibling
 * defined after the replay using the file-scope one. */
int t2(int a)
{
  int q = 10;
  int middle(int k)
  {
    int leaf(int j) { return j + q; }
    int inner(int j) { return leaf(j) * 3; }
    return inner(k) + leaf(0);
  }
  int r = middle(a) + middle(a + 1);
  int after(int j) { return leaf(j) + inner(j); }
  return r * 100000 + after(2);
}

int main(void)
{
  volatile int one = 1;
  printf("%d %d\n", t(one), t(one + 4));
  printf("%d\n", t2(one));
  return 0;
}
