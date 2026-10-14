/*
 *  TCC IR - conditional branches over a return's jump, branches to the epilogue
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `if (!c) return v;` with v already in r0 was `cbnz c, L; b <epilogue>; L:`;
 * it is `cbz c, <epilogue>` (codegen: a return whose only code is its jump is
 * hopped like a JUMP).  A single-bit test whose mask is built before the
 * word's load shifts the loaded word instead.  The neighbours pin what must
 * not change: a return that moves its value, a tail-call-only body, an
 * epilogue out of CBZ reach, a store or a volatile between mask and load. */
#include <stdint.h>
#include <stdio.h>

struct Opt { uint32_t payload; uint8_t is_null; };

__attribute__((noinline)) struct Opt lookup(uint32_t k)
{
  struct Opt o = { k * 3u, (uint8_t)(k & 1u) };
  return o;
}

/* reserve_pages' region loop: the payload is already in r0. */
__attribute__((noinline)) uint32_t first_hit(const uint32_t *keys, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    struct Opt s = lookup(keys[i]);
    if (!s.is_null)
      return s.payload;
  }
  return 0xffffffffu;
}

/* a is already in r0. */
__attribute__((noinline)) uint32_t keep_a(uint32_t a, uint32_t b)
{
  if (b == 0)
    return a;
  return a * b + 7u;
}

/* b has to move to r0 first: the return is not a lone jump. */
__attribute__((noinline)) uint32_t move_b(uint32_t a, uint32_t b)
{
  if (a == 0)
    return b;
  return a * b + 7u;
}

/* A 64-bit value already in r0:r1. */
__attribute__((noinline)) uint64_t keep64(uint64_t a, uint32_t b)
{
  if (b == 0)
    return a;
  return a * b + 1u;
}

/* A void return out of a loop: a branch straight to the epilogue. */
__attribute__((noinline)) void scan(const uint32_t *p, uint32_t n, uint32_t *out)
{
  uint32_t acc = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] == 0)
      return;
    acc += p[i];
  }
  *out = acc;
}

__attribute__((noinline)) uint32_t helper(uint32_t a, uint32_t b) { return a - b; }

/* The body is a tail call; the early return still needs an epilogue. */
__attribute__((noinline)) uint32_t tail(uint32_t a, uint32_t b)
{
  if (b == 0)
    return a;
  return helper(a, b);
}

/* The epilogue is out of CBZ reach. */
__attribute__((noinline)) uint32_t far(uint32_t a, uint32_t b, volatile uint32_t *sink)
{
  if (b == 0)
    return a;
  for (int k = 0; k < 3; k++) {
    sink[0] = a * 3u + b;
    sink[1] = a ^ (b << 3);
    sink[2] = a + (b >> 2);
    sink[3] = (a | b) - 5u;
    sink[4] = (a & b) + 9u;
    sink[5] = a * b * 7u;
    sink[6] = (a << 4) ^ b;
    sink[7] = a - (b << 1);
    sink[8] = a + b * 11u;
    sink[9] = (a >> 1) + (b >> 3);
    sink[10] = a * 13u - b;
    sink[11] = (a ^ 0x55u) + b;
    sink[12] = a * 17u + (b ^ 0x3cu);
    sink[13] = (a >> 3) ^ (b * 5u);
    sink[14] = (a + 0x1234u) * b;
    sink[15] = (a - b) ^ 0xa5a5u;
    a = sink[0] + sink[5] + sink[11] + sink[14];
  }
  return a + b;
}

/* Two returns of the same value from inside a loop and after it. */
__attribute__((noinline)) int32_t find_neg(const int32_t *p, uint32_t n, int32_t dflt)
{
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] < 0)
      return dflt;
    dflt += p[i] & 1;
  }
  return dflt * 2;
}

/* The mask is built before the word is loaded. */
__attribute__((noinline)) int mask_first(const uint32_t *p, uint32_t n) { return ((1u << n) & *p) != 0; }
__attribute__((noinline)) int mask_first_if(const uint32_t *p, uint32_t n)
{
  if ((1u << n) & *p)
    return 3;
  return 5;
}
/* A store between the mask and the load: the load must see it. */
__attribute__((noinline)) int mask_store(const uint32_t *p, uint32_t *q, uint32_t n)
{
  uint32_t m = 1u << n;
  *q = 0x10u;
  return (m & *p) != 0;
}
/* A volatile word: read once, where the source reads it. */
static volatile uint32_t vword = 0x8u;
__attribute__((noinline)) int mask_volatile(uint32_t n) { return ((1u << n) & vword) != 0; }

int main(void)
{
  uint32_t keys_a[4] = { 1, 3, 4, 5 };
  uint32_t keys_b[3] = { 2, 4, 6 };
  printf("first_hit %u %u %u\n", first_hit(keys_a, 4), first_hit(keys_b, 3), first_hit(keys_a + 2, 1));
  printf("keep_a %u %u move_b %u %u\n", keep_a(9, 0), keep_a(9, 2), move_b(0, 4), move_b(3, 4));
  printf("keep64 %llu %llu\n", (unsigned long long)keep64(0x123456789ull, 0),
         (unsigned long long)keep64(0x100000000ull, 3));
  uint32_t vals[5] = { 4, 5, 0, 7, 8 }, out = 77;
  scan(vals, 2, &out);
  printf("scan %u", out);
  out = 77;
  scan(vals, 5, &out);
  printf(" %u", out);
  scan(vals, 0, &out);
  printf(" %u\n", out);
  printf("tail %u %u\n", tail(40, 0), tail(40, 15));
  static volatile uint32_t sink[16];
  printf("far %u %u\n", far(21, 0, sink), far(21, 4, sink));
  int32_t negs[4] = { 1, 3, -2, 5 };
  printf("find_neg %d %d %d\n", find_neg(negs, 4, 10), find_neg(negs, 2, 10), find_neg(negs, 0, -4));
  uint32_t word = 0x80000004u, other = 0;
  printf("mask_first %d %d %d %d\n", mask_first(&word, 2), mask_first(&word, 3), mask_first(&word, 31),
         mask_first(&word, 30));
  printf("mask_first_if %d %d\n", mask_first_if(&word, 31), mask_first_if(&word, 0));
  printf("mask_store %d %d %d\n", mask_store(&word, &other, 4), mask_store(&other, &other, 4),
         mask_store(&other, &other, 2));
  printf("mask_volatile %d %d\n", mask_volatile(3), mask_volatile(2));
  return 0;
}
