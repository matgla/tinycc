/*
 * Regression test: ssa:dce's dead-overwrite-store scan read only src1/src2
 * of an instruction, so a frame slot read through the accumulator of a fused
 * MLA (`q.x += k * s` -> MLA with q.x as the accumulator) did not count as a
 * read: the earlier store into q.x (from `q = base`) looked overwritten and
 * was deleted, and the MLA read stale frame contents.  Seen in a GNU nested
 * function at -O1, where local forwarding stays off for the captures
 * (round-5 integration; also wrong on the base cross).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>

struct P
{
  int x, y, z;
};

__attribute__((noinline)) struct P par(int a)
{
  struct P base = {a, 2 * a, 3 * a};
  int s = 11;
  struct P mk(int k)
  {
    struct P q = base;
    q.x += k * s;
    q.z = q.z + k;
    return q;
  }
  struct P out = mk(1);
  s = 20;
  struct P out2 = mk(2);
  out.y = out2.x;
  return out;
}

__attribute__((noinline)) int acc(int a, int k)
{
  int base[2] = {a, a + 1};
  int m = 7;
  int f(void)
  {
    int q[2];
    q[0] = base[0];
    q[1] = base[1];
    q[0] += k * m;
    q[1] -= k * m;
    return q[0] * 1000 + q[1];
  }
  return f();
}

int main(void)
{
  struct P p = par(1), r = par(4);
  printf("%d %d %d / %d %d %d\n", p.x, p.y, p.z, r.x, r.y, r.z);
  printf("%d %d\n", acc(3, 2), acc(100, 1));
  return 0;
}
