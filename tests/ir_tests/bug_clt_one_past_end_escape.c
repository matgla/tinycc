/*
 *  TCC IR - A constant local table written through its one-past-end address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* const_local_table attributed frame addresses to objects by [lo, hi), so
 * `t + 4` of an `int t[4]` belonged to none and its escapes went unseen: the
 * table became a .rodata image and missed writes made through the end
 * pointer.  An address at an object's end now counts as that object's. */
#include <stdio.h>
#define NI __attribute__((noinline))

NI int via_var(int i)
{
  int t[4] = {-1, -2, -3, -4};
  int *e = t + 4;
  e[-2] = 1234;
  return t[2] + t[i & 3];
}

NI void back_fill(int *end, int n)
{
  while (n--)
    *--end = 70 - n;
}
NI int via_call(int i)
{
  int t[4] = {1, 2, 3, 4};
  back_fill(t + 4, 1);
  return t[i & 3];
}

NI int via_char(int i)
{
  char t[3] = {1, 2, 3};
  char *e = t + 3;
  e[-1] = 33;
  return t[i % 3];
}

struct R { int a[2]; };
NI int via_struct(int i)
{
  struct R r = {{-1, -3}};
  struct R *e = &r + 1;
  e[-1].a[1] = 1234;
  return r.a[1] + r.a[i & 1];
}

NI int range_sum(const int *b, const int *e)
{
  int s = 0;
  while (b != e)
    s += *b++;
  return s;
}
NI void range_neg(int *b, int *e)
{
  while (b != e)
    *b = -*b, b++;
}
/* [begin, end) idiom: the end pointer escapes into a writing callee */
NI int via_range(int i)
{
  int t[4] = {5, 6, 7, 8};
  range_neg(t + 2, t + 4);
  return t[i & 3] + range_sum(t, t + 4);
}

/* Read-only uses: the table may still become an image */
NI int read_only(int i)
{
  static const int c[1] = {0};
  int t[4] = {10, 20, 30, 40};
  return t[i & 3] + range_sum(t, t + 4) + c[0];
}

NI int lookup(int i)
{
  int t[4] = {3, 1, 4, 1};
  return t[i & 3];
}

int main(void)
{
  printf("%d %d %d %d\n", via_var(2), via_call(3), via_char(2), via_struct(0));
  printf("%d %d %d\n", via_range(3), read_only(1), lookup(2));
  return 0;
}
