/*
 *  TCC IR - switch-call IPC folding of a callee with barrel-shifted operands
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:switch_call_replace simulated a one-parameter callee for a constant
 * argument from its final IR, but gen_function freed ir->barrel_shifts before
 * the snapshot and the simulator ignored them: `i < (n >> 4)` (shift fused
 * into the CMP) ran as `i < n`, so sum16(15) folded to 105.  Same class at
 * run time: ssa:loop_rotate cloned the header CMP into the latch without its
 * barrel annotation, so the rotated loop ran n times (sum16(16) at -O1). */

#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) uint32_t sum16(uint32_t n)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i < (n >> 4); i++)
    s += i;
  return s;
}

__attribute__((noinline)) int count_dbl(int x)
{
  int i = 0;
  while (x > (i << 1))
    i++;
  return i;
}

__attribute__((noinline)) int count_asr(int x)
{
  int i = 0;
  while (i < (x >> 2))
    i++;
  return i;
}

__attribute__((noinline)) uint32_t add_scaled(uint32_t n)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++)
    s += i << 3;
  return s;
}

__attribute__((noinline)) int sub_scaled(int n)
{
  int s = 1000;
  for (int i = 0; i < n; i++)
    s -= i << 2;
  return s;
}

__attribute__((noinline)) uint32_t wrap_add(uint32_t n)
{
  uint32_t s = 0xFFFFFFF0u;
  for (uint32_t i = 0; i < n; i++)
    s += 8;
  return s;
}

__attribute__((noinline)) uint32_t cnt3(uint32_t n)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i < (n >> 4); i++)
    s += 3;
  return s;
}

__attribute__((noinline)) int cnt_shl(int n, int k)
{
  int s = 0;
  for (int i = 0; i < (n << 2); i++)
    s += k;
  return s;
}

volatile uint32_t vn = 40;

/* A plain dispatcher: must still fold (or run) to the right constants. */
__attribute__((noinline)) int dispatch(int k)
{
  switch (k)
  {
  case 1: return 10;
  case 2: return 20;
  case 7: return 70;
  default: return -1;
  }
}

int main(void)
{
  printf("sum16(15)=%u\n", sum16(15));
  printf("sum16(16)=%u\n", sum16(16));
  printf("sum16(64)=%u\n", sum16(64));
  printf("sum16(100)=%u\n", sum16(100));
  printf("count_dbl(0)=%d\n", count_dbl(0));
  printf("count_dbl(7)=%d\n", count_dbl(7));
  printf("count_dbl(10)=%d\n", count_dbl(10));
  printf("count_asr(3)=%d\n", count_asr(3));
  printf("count_asr(17)=%d\n", count_asr(17));
  printf("count_asr(-8)=%d\n", count_asr(-8));
  printf("add_scaled(4)=%u\n", add_scaled(4));
  printf("sub_scaled(5)=%d\n", sub_scaled(5));
  printf("wrap_add(3)=%u\n", wrap_add(3));
  printf("cnt3=%u %u %u %u\n", cnt3(15), cnt3(16), cnt3(100), cnt3(vn));
  printf("cnt_shl=%d %d\n", cnt_shl(3, 2), cnt_shl(0, 5));
  printf("sum16(vn)=%u\n", sum16(vn));
  printf("dispatch=%d %d %d %d\n", dispatch(1), dispatch(2), dispatch(7), dispatch(3));
  return 0;
}
