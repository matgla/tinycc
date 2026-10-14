/*
 *  TCC IR - Guarded field load reuse regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

#define NOINLINE __attribute__((noinline))

NOINLINE static unsigned after_store(const unsigned *p, unsigned *q)
{
  if (*p > 1000)
    return 0;
  *q = 17;
  return *p;
}

NOINLINE static void change(unsigned *p)
{
  *p = 23;
}

NOINLINE static unsigned after_call(unsigned *p)
{
  if (*p > 1000)
    return 0;
  change(p);
  return *p;
}

NOINLINE static unsigned volatile_read(volatile unsigned *p)
{
  if (*p > 1000)
    return 0;
  *p = 29;
  return *p;
}

NOINLINE static unsigned skipped_guard(unsigned *p, int skip)
{
  if (skip)
    goto body;
  if (*p > 1000)
    return 0;
body:
  return *p ^ 7;
}

NOINLINE static unsigned scan(const unsigned *p)
{
  unsigned sum = 0;
test:
  if (*p > 1000)
    return sum;
  sum += *p;
  p++;
  goto test;
}

NOINLINE static unsigned both(const unsigned *p, const unsigned *q)
{
  if (*p > *q)
    return *p ^ *q;
  return *p + *q;
}

NOINLINE static unsigned narrow(const signed char *p)
{
  if (*p > 10)
    return 0;
  return (unsigned)*p;
}

int main(void)
{
  unsigned a = 3, b = 5;
  if (after_store(&a, &b) != 3 || b != 17)
    return 1;
  if (after_store(&a, &a) != 17)
    return 2;
  if (after_call(&a) != 23)
    return 3;
  if (volatile_read(&a) != 29)
    return 4;
  if (skipped_guard(&a, 0) != (29 ^ 7))
    return 5;
  a = 1001;
  if (skipped_guard(&a, 0) != 0 || skipped_guard(&a, 1) != (1001 ^ 7))
    return 6;
  unsigned values[] = {3, 5, 7, 1001};
  if (scan(values) != 15 || scan(values + 3) != 0)
    return 7;
  a = 13;
  b = 7;
  if (both(&a, &b) != (13 ^ 7) || both(&b, &a) != 20)
    return 8;
  signed char c = -7;
  if (narrow(&c) != (unsigned)-7)
    return 9;
  puts("PASS");
  return 0;
}
