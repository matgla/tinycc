/*
 * A definition adopts its prototype's function type Sym (patch_type,
 * sym_functype_adopt) only when no typedef, typeof or other declaration
 * shares it: each body binds its own parameter names, in every order.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

/* own prototype, renamed const parameters, a pointer taken before the body */
static int k(int a, int b);
static int (*pk)(int, int) = k;

/* a typedef's function type, shared by three functions */
typedef int F(int);
F f1, f2;
static F f3;

/* typeof shares m1's type with m2; defined in the other order */
static int m1(int a);
static __typeof__(m1) m2;

/* declared twice before the definition */
static int r(int);
static int r(int u);

/* typeof after the definition */
static int n1(int c);
static int n1(int const d) { return d - 1; }
static __typeof__(n1) n2;

static int k(int const y, int const x) { return y * 10 + x; }
int f1(int p) { return p + 1; }
int f2(int q) { return q * 2; }
static int f3(int z) { return z - 3; }
static int m2(int const s) { return s + 100; }
static int m1(int const t) { return t + 200; }
static int r(int const w) { return w * w; }
static int n2(int const e) { return e + 7; }

/* redeclared after the definition */
static int k(int, int);

int main(void)
{
  int (*pm[2])(int) = {m1, m2};
  printf("k %d %d\n", k(1, 2), pk(3, 4));
  printf("f %d %d %d\n", f1(10), f2(10), f3(10));
  printf("m %d %d\n", pm[0](1), pm[1](1));
  printf("r %d\n", r(9));
  printf("n %d %d\n", n1(5), n2(5));
  return 0;
}
