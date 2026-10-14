/*
 *  TCC IR - r0/r1 aggregate returns: a GNU extern inline body replaced later
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Under -fgnu89-inline an `extern inline` body is compiled as a static
 * inline one until the real definition replaces it -- an external function
 * the other TU calls with the AAPCS sret convention.  The call compiled in
 * between must not have made f an r0/r1 function (gfunc_regpair_ret). */

#include <stdio.h>

typedef struct { int a, b; } P;

extern inline P f(int x) { P p = {x + 1, x + 2}; return p; }
int __attribute__((noinline)) use(int x) { P r = f(x); return r.a * 100 + r.b; }
P f(int x) { P p = {x + 1, x + 2}; return p; }
int __attribute__((noinline)) use2(int x) { P r = f(x); return r.a * 100 + r.b; }
int other(int x); /* bug_regpair_extern_inline+.c: calls f */

int main(void)
{
  printf("%d %d %d\n", use(3), use2(4), other(5));
  return 0;
}
