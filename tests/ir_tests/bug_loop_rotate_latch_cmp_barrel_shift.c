/*
 * ssa:loop_rotate: the rotated bottom test copied the header CMP without its
 * barrel-shift annotation, so `i < len / 4` became `i < len` in the latch.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

static uint32_t a[64];
static uint32_t b[64];
static volatile int sink;

/* trip counts are bounded by a guard index so a lost shift cannot run off the array */
#define GUARD(i, n)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((i) >= (n))                                                                                                    \
      return -1000 - (int)(i);                                                                                         \
  } while (0)

__attribute__((noinline)) int sum_quarter(const uint32_t *p, uint32_t len)
{
  int s = 0;
  for (uint32_t i = 0; i < len / 4; i++)
    s += p[i];
  return s;
}

__attribute__((noinline)) int words_equal(const uint32_t *p, const uint32_t *q, uint32_t len)
{
  for (uint32_t i = 0; i < (len - 1) / 4; i++)
    if (p[i] != q[i])
      return 0;
  return 1;
}

/* shift on the left-hand side of the compare */
__attribute__((noinline)) int count_rev(uint32_t len)
{
  int c = 0;
  for (uint32_t i = 0; len >> 3 > i; i++)
  {
    GUARD(i, 64);
    c += (int)a[i];
  }
  return c;
}

/* signed bound: ASR */
__attribute__((noinline)) int count_signed(int n)
{
  int c = 0;
  for (int i = 0; i < n >> 1; i++)
  {
    GUARD(i, 64);
    c += (int)a[i];
  }
  return c;
}

/* LSL bound */
__attribute__((noinline)) int count_lsl(uint32_t n)
{
  int c = 0;
  for (uint32_t i = 0; i < n << 2; i++)
  {
    GUARD(i, 64);
    c += (int)a[i];
  }
  return c;
}

/* a call in the body: rotation may decline, header duplication takes it */
__attribute__((noinline)) void touch(int v)
{
  sink = v;
}

__attribute__((noinline)) int count_call(uint32_t len)
{
  int c = 0;
  for (uint32_t i = 0; i < len / 8; i++)
  {
    GUARD(i, 64);
    touch((int)i);
    c++;
  }
  return c;
}

/* bound read through a pointer each trip, compare against the shifted copy */
__attribute__((noinline)) int count_ptr(const uint32_t *lenp)
{
  int c = 0;
  uint32_t i = 0;
  while (i < *lenp / 16)
  {
    GUARD(i, 64);
    c += (int)b[i];
    i++;
  }
  return c;
}

int main(void)
{
  for (int i = 0; i < 64; i++)
  {
    a[i] = (uint32_t)(i + 1);
    b[i] = i < 4 ? (uint32_t)(i + 1) : 0xdeadu;
  }
  uint32_t len64 = 64;
  printf("sum_quarter(16)=%d\n", sum_quarter(a, 16));
  printf("sum_quarter(3)=%d\n", sum_quarter(a, 3));
  printf("sum_quarter(0)=%d\n", sum_quarter(a, 0));
  printf("words_equal(17)=%d\n", words_equal(a, b, 17));
  printf("words_equal(21)=%d\n", words_equal(a, b, 21));
  printf("count_rev(40)=%d\n", count_rev(40));
  printf("count_signed(9)=%d\n", count_signed(9));
  printf("count_signed(-8)=%d\n", count_signed(-8));
  printf("count_lsl(3)=%d\n", count_lsl(3));
  printf("count_call(40)=%d\n", count_call(40));
  printf("count_ptr(64)=%d\n", count_ptr(&len64));
  return 0;
}
