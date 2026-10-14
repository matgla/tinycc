/*
 *  TCC IR - captures and calls of an outer nesting level use that level's frame
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `inner` (in `outer`, in r3) reads outer's `n` and r3's `depth`, both at
 * chain offset -8, and calls `outer`, which needs r3's frame.  Base f6d53136
 * matched a captured operand by offset alone (first match wins, so `n` was
 * read two levels up) and passed inner's own incoming chain to `outer`:
 * "r3 -939528292" at every level.  Fix: captures get disjoint virtual ranges
 * in the nested function, each with its own depth, and a call passes the
 * callee's defining frame. */
#include <stdio.h>

__attribute__((noinline)) int r3(int k)
{
  int depth = 0;
  int outer(int n)
  {
    int inner(int m)
    {
      depth++;
      return m <= 0 ? k : inner(m - 1) + outer(m - 1) + n;
    }
    return n <= 0 ? 1 : inner(n - 1);
  }
  int v = outer(4);
  return v * 100 + depth;
}

/* three levels, a write at each level, same-offset locals everywhere */
__attribute__((noinline)) int r4(int a)
{
  int x = a;
  int l1(int b)
  {
    int y = b;
    int l2(int c)
    {
      int z = c;
      int l3(int d)
      {
        x += 1;
        y += 10;
        z += 100;
        return d + x * 1000 + y * 100 + z;
      }
      int r = l3(1);
      return r + z + l1(b - 1 > 0 ? -1 : -2) * 0;
    }
    return b < 0 ? y : l2(b + 1) + y;
  }
  int r = l1(2);
  return r + x;
}

/* a grandchild calls its grandparent's sibling */
__attribute__((noinline)) int r5(int k)
{
  int top(int t) { return t * 3 + k; }
  int mid(int m)
  {
    int low(int l) { return top(l + m) + 1; }
    return low(m) + low(1);
  }
  return mid(2);
}

int main(void)
{
  printf("r3 %d\n", r3(1));
  printf("r4 %d\n", r4(3));
  printf("r5 %d\n", r5(5));
  return 0;
}
