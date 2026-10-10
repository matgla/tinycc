/* Initialising a struct member's flexible array through a union wrote the
 * element count into the struct definition's own array Sym
 * (init/alloc.c decl_initializer_alloc), so the struct stayed fixed-size for
 * the rest of the TU and a later static FAM initializer ICEd. Fix: put the
 * size back after the initializer and bound the elements by the union size.
 *
 * Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */
#include <stdio.h>

struct F { int n; int a[]; };
union U { struct F s; int x[4]; };
struct A { int a; char b[]; };
union B { struct A a; char b[sizeof (struct A) + 31]; };

union U u1 = { .s = { 1, { 2, 3 } } };
struct F f2 = { 9, { 7, 8, 9 } };
union U u2 = { .s = { 5, { 6, 7, 8 } } };
union U u3 = { .s = { 4 } };
union U u4 = { .x = { 5, 6, 7, 8 } };
union B b1 = { { 1, "123456789012345678901234567890" } };
static struct F f3 = { 4, { 1, 2, 3, 4, 5, 6 } };
struct A a1 = { 3, "flex" };

__attribute__((noinline)) int local_union(int v)
{
  union U l = { .s = { v, { v + 1, v + 2 } } };
  return l.x[0] + l.x[1] + l.x[2];
}

int main(void)
{
  printf("u1 %d %d %d\n", u1.s.n, u1.s.a[0], u1.s.a[1]);
  printf("f2 %d %d %d %d\n", f2.n, f2.a[0], f2.a[1], f2.a[2]);
  printf("u2 %d %d %d %d\n", u2.x[0], u2.x[1], u2.x[2], u2.x[3]);
  printf("u3 %d\n", u3.s.n);
  printf("u4 %d %d %d\n", u4.s.n, u4.s.a[0], u4.s.a[2]);
  printf("b1 %d %s\n", b1.a.a, b1.a.b);
  printf("f3 %d %d %d\n", f3.n, f3.a[0], f3.a[5]);
  printf("a1 %d %s\n", a1.a, a1.b);
  printf("local %d\n", local_union(10));
  printf("sizeof F %d U %d f2 %d\n", (int)sizeof(struct F), (int)sizeof(union U), (int)sizeof f2);
  return 0;
}
