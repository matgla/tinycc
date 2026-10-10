/* GNU cast to union: (union U)x initialises the member whose type is x's
 * (lvalue-converted, unqualified) type, not always the first member
 * (expr/unary.c unary_paren). Static initialisers included.
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include <stdio.h>

struct S { short a, b; };
struct Big { int w[3]; };
union U { char c; int i; double d; struct S s; int *p; float f; unsigned long long q; };
union W { struct S s; struct Big big; double d; };
typedef union U UT;

int arr[4] = { 10, 20, 30, 40 };
int gx = 77;

/* static initialisers through the cast */
union U g_int = (union U)1000;
union U g_dbl = (union U)2.5;
union U g_ptr = (union U)&gx;
union U g_chr = (union U)(char)'z';

__attribute__((noinline)) int by_int(int x) { union U u = (union U)x; return u.i; }
__attribute__((noinline)) double by_dbl(double x) { union U u = (union U)x; return u.d; }
__attribute__((noinline)) int by_const(const int x) { UT u = (UT)x; return u.i; }
__attribute__((noinline)) int by_char(char x) { union U u = (union U)x; return u.c; }
__attribute__((noinline)) float by_flt(float x) { union U u = (union U)x; return u.f; }
__attribute__((noinline)) unsigned long long by_ull(unsigned long long x) { union U u = (union U)x; return u.q; }
__attribute__((noinline)) int by_array(void) { union U u = (union U)arr; return u.p[2]; }
__attribute__((noinline)) int by_ptr(int *p) { union U u = (union U)p; return *u.p; }
__attribute__((noinline)) int by_struct(volatile struct S s) { union U u = (union U)s; return u.s.a * 100 + u.s.b; }
__attribute__((noinline)) int by_big(struct Big b) { union W w = (union W)b; return w.big.w[0] + w.big.w[1] + w.big.w[2]; }
__attribute__((noinline)) int by_small(struct S s) { union W w = (union W)s; return w.s.a * 100 + w.s.b; }
__attribute__((noinline)) int self(union U v) { union U u = (union U)v; return u.i; }

static int take_int(union U u) { return u.i; }

int main(void)
{
  struct S s = { 3, 4 };
  struct Big b = { { 1, 2, 3 } };
  union U v;
  v.i = 4242;
  printf("int %d\n", by_int(1000));
  printf("dbl %g\n", by_dbl(2.5));
  printf("const %d\n", by_const(-123456));
  printf("char %d\n", by_char(65));
  printf("float %g\n", (double)by_flt(1.5f));
  printf("ull %llx\n", by_ull(0x123456789abcdefULL));
  printf("array %d\n", by_array());
  printf("ptr %d\n", by_ptr(&gx));
  printf("struct %d\n", by_struct(s));
  printf("big %d\n", by_big(b));
  printf("small %d\n", by_small(s));
  printf("self %d\n", self(v));
  printf("arg %d\n", take_int((union U)99999));
  printf("sizeof %d\n", (int)sizeof((union U)1));
  printf("g_int %d\n", g_int.i);
  printf("g_dbl %g\n", g_dbl.d);
  printf("g_ptr %d\n", *g_ptr.p);
  printf("g_chr %c\n", g_chr.c);
  return 0;
}
