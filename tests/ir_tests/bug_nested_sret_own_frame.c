/*
 *  TCC IR - a struct-returning nested function keeps its own locals at home
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Base f6d53136, -O0: in1's own `q1` sat at the offset of the parent's
 * captured `r1`, so it was written through the chain, over r1, and the sret
 * pointer's home was the chain's save slot; -O1+: the parent's stores to a
 * struct only the nested function reads were deleted.  Expected "7 12" in
 * every column.  Fix: captures get offset ranges no local of the nested
 * function uses, the chain slot is reserved before the sret pointer's home,
 * and a frame shared with nested functions keeps dse out. */
#include <stdio.h>
#define NI __attribute__((noinline))

typedef struct { int a, b; } P;
typedef struct { int v[6]; } Big;

NI P sc_sret(int x)
{
  P r1 = {x, x};
  P in1(int y) { P q1 = {r1.a + y, r1.b * y}; return q1; }
  return in1(3);
}
NI P sc_sret_fp(int x)
{
  P r2 = {x, x};
  P in2(int y) { P q2 = {r2.a + y, r2.b * y}; return q2; }
  P (*fp)(int) = in2;
  return fp(3);
}
NI P ic_sret(int x)
{
  int k3 = x;
  P in3(int y) { P q3 = {k3 + y, k3 * y}; return q3; }
  return in3(3);
}

NI int ext(int v) { return v + 1; }

/* the chain is reloaded after a call: its slot must still hold it */
NI Big big_sret(int x)
{
  P r4 = {x, x * 2};
  Big in4(int y)
  {
    Big b;
    for (int i = 0; i < 6; i++)
      b.v[i] = ext(i) + r4.a * y;
    b.v[5] += r4.b;
    return b;
  }
  Big (*volatile fp)(int) = in4;
  Big t = in4(2);
  Big u = fp(1);
  t.v[0] += u.v[5];
  return t;
}

/* a whole captured struct copied in and out, own struct locals beside it */
NI int whole(int x)
{
  P r5 = {x, x + 1};
  P in5(void)
  {
    P own = {100, 200};
    P cp = r5;
    r5 = own;
    own.a += cp.a;
    own.b += cp.b;
    return own;
  }
  P a = in5();
  return a.a * 1000 + a.b + r5.a * 1000000;
}

int main(void)
{
  P a = sc_sret(4), b = sc_sret_fp(4), c = ic_sret(4);
  printf("%d %d | %d %d | %d %d\n", a.a, a.b, b.a, b.b, c.a, c.b);
  Big g = big_sret(5);
  printf("%d %d %d %d %d %d\n", g.v[0], g.v[1], g.v[2], g.v[3], g.v[4], g.v[5]);
  printf("%d\n", whole(2));
  return 0;
}
