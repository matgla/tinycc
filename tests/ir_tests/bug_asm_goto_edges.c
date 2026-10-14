/*
 *  TCC IR - asm goto control flow and register liveness
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>
#define NI __attribute__((noinline))
#define AG(c, L) __asm__ goto("cmp %0, #0\n\tbne %l1" : : "r"(c) : "cc" : L)
struct S
{
  int a, b;
};
NI int s1(int c)
{
  int x = 1;
  int *p = &x;
  AG(c, L);
  *p = 2;
L:
  return *p;
}
NI int s2(int c)
{
  struct S s = {1, 2};
  struct S *p = &s;
  AG(c, L);
  p->a = 5;
L:
  return p->a * 10 + s.b;
}
NI int s3(int c, int *out)
{
  int x = 3;
  *out = 1;
  AG(c, L);
  *out = 2;
  x = 4;
L:
  *out += x;
  return *out;
}
NI int s4(int c)
{
  char b[8];
  memset(b, 1, 8);
  AG(c, L);
  memset(b, 2, 8);
L:
  return b[0] + b[7];
}
NI int s5(int c)
{
  int x = 1, y = 7;
  int *p = &x, *q;
  q = p;
  AG(c, L);
  q = &y;
L:
  return *q;
}
static int g;
NI int s6(int c)
{
  g = 1;
  AG(c, L);
  g = 2;
L:
  return g;
}
NI int s7(int c)
{
  volatile int k = 0;
  int x = 10;
  int *p = &x;
  AG(c, L);
  x = 20;
  p = &k;
L:
  return *p;
}
int value_cases(void)
{
  int o;
  printf("%d %d\n", s1(0), s1(1));
  printf("%d %d\n", s2(0), s2(1));
  printf("%d", s3(0, &o));
  printf(" %d\n", s3(1, &o));
  printf("%d %d\n", s4(0), s4(1));
  printf("%d %d\n", s5(0), s5(1));
  printf("%d %d\n", s6(0), s6(1));
  printf("%d %d\n", s7(0), s7(1));
  return 0;
}

struct opt
{
  unsigned payload;
  _Bool is_null;
};

__attribute__((noinline)) static struct opt next(unsigned x)
{
  struct opt r;
  r.payload = x + 1;
  r.is_null = x + 1 >= 4;
  return r;
}

__attribute__((noinline)) int loop_plain(void)
{
  int n = 0, b = 0;
top:
  if (b >= 4 || n > 50)
    return n;
  n++;
  b = b + 1;
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

__attribute__((noinline)) int loop_struct(void)
{
  int n = 0;
  struct opt b = next(0);
top:
  if (b.is_null || n > 50)
    return n;
  n++;
  b = next(b.payload);
  __asm__ goto("b %l0" : : : : top);
  return 1000;
}

NI int copy_loop(int c)
{
  int x = 1, y = 2, n = 0;
  int *t = &x;
again:
  *t += 10;
  t = &y;
  if (n++ < 2)
    AG(c, again);
  return x * 100 + y;
}

NI int multiple_labels(int c)
{
  int x = 1;
  __asm__ goto("cmp %0, #1\n\tbeq %l1\n\tcmp %0, #2\n\tbeq %l2" : : "r"(c) : "cc" : first, second);
  x = 3;
first:
  x += 4;
second:
  return x;
}

int main(void)
{
  value_cases();
  printf("plain %d struct %d\n", loop_plain(), loop_struct());
  printf("copy_loop %d %d\n", copy_loop(0), copy_loop(1));
  printf("multiple %d %d %d\n", multiple_labels(0), multiple_labels(1), multiple_labels(2));
  return 0;
}
