/* A K&R definition's caller passes promoted arguments (int, double); the callee
 * converts each to its declared parameter type on entry (C11 6.9.1p10).  tcc
 * bound them as if prototyped, so a float read half a double and a signed char
 * read the int 200.  The prologue now receives the promoted type and converts.
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include <stdio.h>

enum small { SA = 1, SB = 2 };

__attribute__((noinline)) int arg(int n) { return n; }
__attribute__((noinline)) double dbl(double d) { return d; }

/* Out of line. */
__attribute__((noinline)) void kp(c, s) signed char c; short s; { printf("kp %d %d\n", c, s); }
__attribute__((noinline)) void ku(c, s) unsigned char c; unsigned short s; { printf("ku %u %u\n", c, s); }
__attribute__((noinline)) int kb(b) _Bool b; { return b + 0; }
__attribute__((noinline)) int kf(f) float f; { return f == 1.0f; }
__attribute__((noinline)) int ke(e) enum small e; { return e; }
/* Register layout: a float takes a double's two words (or d0), so i lands after it. */
__attribute__((noinline)) void kmix(f, i, c, g, d, j) float f; int i; signed char c; float g; double d; int j;
{
  printf("kmix %d %d %d %d %d %d\n", (int)(f * 4), i, c, (int)(g * 4), (int)d, j);
}
/* Stack-passed narrow parameters. */
__attribute__((noinline)) int kstk(a, b, c, d, e, f, g) int a, b, c, d; signed char e; short f; float g;
{
  return a + b + c + d + e + f + (int)(g * 2);
}
/* A parameter whose address is taken or that is written. */
__attribute__((noinline)) int kaddr(c) signed char c; { signed char *p = &c; *p += 1; return c + (int)sizeof c; }
__attribute__((noinline)) int kwr(s) short s; { s += 1; return s; }
__attribute__((noinline)) int kfaddr(f) float f; { float *p = &f; return *p == 1.0f && sizeof f == 4; }
/* Unchanged: types that do not promote. */
__attribute__((noinline)) long long kll(a, d) long long a; double d; { return a + (long long)d; }
__attribute__((noinline)) int kint(i, p) int i; char *p; { return i + p[0]; }
/* Recursion keeps converting each level's argument. */
__attribute__((noinline)) int krec(c, n) signed char c; int n; { return n ? krec(c + 100, n - 1) : c; }

/* A prototype in scope first: arguments arrive as it says. */
int pf(float);
int pg(int);
static int ph(double);
__attribute__((noinline)) int early(void) { return pf(1.5f); }
__attribute__((noinline)) int pf(x) float x; { return x == 1.5f; }
__attribute__((noinline)) int pg(c) char c; { return c; }
__attribute__((noinline)) static int ph(x) float x; { return x == 1.0f; }

/* Inlinable (static, small) and always_inline. */
static void ks(c, s) signed char c; short s; { printf("ks %d %d\n", c, s); }
static int sf(f) float f; { return f == 1.0f; }
static inline __attribute__((always_inline)) int ki(f) float f; { return f == 1.0f; }
static inline __attribute__((always_inline)) int kib(b) _Bool b; { return b + 0; }
static inline __attribute__((always_inline)) int kic(c) unsigned char c; { return c; }

int main(void)
{
  int t = arg(200), u = arg(70005);
  kp(t, u);
  kp(-t, -u);
  ku(arg(-1), arg(-1));
  printf("kb %d %d %d\n", kb(arg(256)), kb(arg(0)), kb(arg(2)));
  printf("kf %d %d\n", kf(dbl(1.0000000001)), kf(dbl(1.0)));
  printf("ke %d\n", ke(arg(258)));
  kmix(dbl(1.25), arg(7), arg(300), dbl(2.5), dbl(9.0), arg(11));
  printf("kstk %d\n", kstk(1, 2, 3, 4, arg(255), arg(65537), dbl(1.5)));
  printf("kaddr %d kwr %d kfaddr %d\n", kaddr(arg(127)), kwr(arg(32767)), kfaddr(dbl(1.0000000001)));
  printf("kll %lld kint %d\n", kll(1LL << 40, dbl(3.0)), kint(arg(5), "A"));
  printf("krec %d\n", krec(arg(1), 3));
  printf("proto %d %d %d %d\n", early(), pf(1.5f), pg(arg(300)), ph(dbl(1.0000000001)));
  ks(t, u);
  ks(-t, -u);
  printf("sf %d ki %d %d\n", sf(dbl(1.0000000001)), ki(dbl(1.0000000001)), ki(dbl(1.5)));
  printf("kib %d %d kic %d\n", kib(arg(256)), kib(arg(0)), kic(arg(511)));
  return 0;
}
