/*
 *  TCC Tests - A scalar compound literal gets its own stack slot
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* decl_initializer_alloc gave an anonymous scalar `(int){0}` no slot (a named
 * scalar gets a vreg): it lived at `loc`, the start of the local allocated last,
 * and a store through its address overwrote that local. */
#include <stdio.h>

struct S
{
  short a, r;
  int vr;
  long long c;
  void *sym;
};

__attribute__((noinline)) int size_of(int *align)
{
  *align = 0x12345678;
  return 30;
}

__attribute__((noinline)) void show(const char *what, struct S *s)
{
  printf("%s: a=%d r=%d vr=%d c=%lld\n", what, s->a, s->r, s->vr, (long long)s->c);
}

__attribute__((noinline)) void store(struct S *src_in, struct S *dst_in, int c)
{
  struct S src = *src_in;
  struct S dst = *dst_in;
  if (c)
  {
    int n = size_of(&(int){0});
    printf("n=%d\n", n);
  }
  show("dst", &dst);
  show("src", &src);
}

__attribute__((noinline)) int two(void)
{
  int *p = &(int){1};
  int *q = &(int){2};
  *p += 10;
  return *p * 100 + *q;
}

int main(void)
{
  struct S a = {1, 0x52, 7, 9, 0}, b = {2, 0x152, 8, 10, 0};
  store(&a, &b, 1);
  store(&a, &b, 0);
  printf("two=%d\n", two());
  {
    int k = 5; /* a named scalar: still a register candidate */
    int m = size_of(&k);
    printf("k=%x m=%d\n", k, m);
  }
  return 0;
}
