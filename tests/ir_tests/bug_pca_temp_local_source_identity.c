/*
 *  TCC Tests - param_copy_alias renamed onto a temporary slot
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* opt:param_copy_alias renamed `w` of `union W w = (union W)b` onto the cast's
 * frontend temporary as a plain slot, dropping the temporary's vreg: then
 * dead_temp_local saw no reader of the temporary and removed its stores, and
 * `*&w.big.w[i]` read garbage.  The renamed operand keeps the source's vreg.
 * Then sl_forward's LOAD_INDEXED path forwarded a struct copy's u16 into an
 * i16 read without sign-extending it (by_half): it forwards a narrow value
 * only when the read gives it back unchanged. */
#include <stdio.h>

struct S { char a; int b; };
struct Big { int w[3]; };
union W { struct S s; struct Big big; double d; };

__attribute__((noinline)) int by_big(struct Big b)
{
  union W w = (union W)b;
  int *p = &w.big.w[0];
  if (!p)
    __builtin_trap();
  return *p;
}

__attribute__((noinline)) int by_big_last(struct Big b)
{
  union W w = (union W)b;
  int *p = &w.big.w[2];
  if (!p)
    __builtin_trap();
  return *p;
}

__attribute__((noinline)) int by_big_index(struct Big b, int i)
{
  union W w = (union W)b;
  int *p = &w.big.w[i];
  if (!p)
    __builtin_trap();
  return *p;
}

/* the member assignment keeps its copy (must not change) */
__attribute__((noinline)) int by_big2(struct Big b)
{
  union W w;
  w.big = b;
  int *p = &w.big.w[1];
  if (!p)
    __builtin_trap();
  return *p;
}

/* written through the address after the cast */
__attribute__((noinline)) int by_big_write(struct Big b)
{
  union W w = (union W)b;
  int *p = &w.big.w[1];
  if (!p)
    __builtin_trap();
  *p += 5;
  return w.big.w[0] + w.big.w[1] + w.big.w[2];
}

__attribute__((noinline)) int by_s(struct S s)
{
  union W w = (union W)s;
  int *p = &w.s.b;
  if (!p)
    __builtin_trap();
  return *p + w.s.a;
}

struct H { short a, b; };
union HW { struct H h; int w[3]; double d; };

__attribute__((noinline)) int by_half(struct H h)
{
  union HW w = (union HW)h;
  return w.h.a * 100 + w.h.b;
}

struct C { signed char a, b, c, d; };
union CW { struct C c; int w[2]; };

__attribute__((noinline)) int by_char(struct C c)
{
  union CW w = (union CW)c;
  signed char *p = &w.c.b;
  if (!p)
    __builtin_trap();
  return w.c.a * 1000 + *p * 100 + w.c.c * 10 + w.c.d;
}

int main(void)
{
  struct Big b = {{11, 22, 33}};
  struct S s = {3, 40};
  printf("big %d\n", by_big(b));
  printf("last %d\n", by_big_last(b));
  printf("index %d %d %d\n", by_big_index(b, 0), by_big_index(b, 1), by_big_index(b, 2));
  printf("big2 %d\n", by_big2(b));
  printf("write %d\n", by_big_write(b));
  printf("s %d\n", by_s(s));
  struct H h = {-3, -7};
  struct H h2 = {12, 300};
  printf("half %d %d\n", by_half(h), by_half(h2));
  struct C c = {-1, -2, 3, -4};
  printf("char %d\n", by_char(c));
  return 0;
}
