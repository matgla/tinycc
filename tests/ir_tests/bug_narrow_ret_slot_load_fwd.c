/*
 *  TCC IR - narrow return-slot VAR read after its other arm folded
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* An inlined two-return u8 helper leaves a VAR return slot; once one arm
 * folds away, `V <- T [STORE]; T' <- V [LOAD]` stayed: a register move and a
 * UXTB of a CLZ result.  ssa:load_cse now forwards the stored TEMP into the
 * narrow LOAD (same width and signedness), and ra:known_ext turns a narrow
 * register LOAD whose value fits into a copy. */

#include <stdint.h>
#include <stdio.h>

static uint8_t ctz32(uint32_t v)
{
  if (v == 0)
    return 32;
  return (uint8_t)__builtin_ctz(v);
}

/* zig_ctz_u32 under the `bits != 0` test of a bitmap scan. */
__attribute__((noinline)) uint32_t first_set(const uint32_t *w, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t b = w[i];
    if (b != 0)
      return i * 32 + ctz32(b);
  }
  return n * 32;
}

/* The forwarded value does not fit a byte: the truncation must stay. */
static uint8_t wrap8(uint32_t v, uint32_t k)
{
  if (v == 0)
    return 7;
  return (uint8_t)(v + k);
}

__attribute__((noinline)) uint32_t wrap_sum(const uint32_t *w, uint32_t n, uint32_t k)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t b = w[i];
    if (b != 0)
      s += wrap8(b, k);
  }
  return s;
}

/* A signed slot: the value read back is sign-extended from the byte. */
static int8_t sbyte(uint32_t v, uint32_t k)
{
  if (v == 0)
    return -3;
  return (int8_t)(v + k);
}

__attribute__((noinline)) int32_t sbyte_sum(const uint32_t *w, uint32_t n, uint32_t k)
{
  int32_t s = 0;
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t b = w[i];
    if (b != 0)
      s += sbyte(b, k);
  }
  return s;
}

/* A u16 slot holding a CLZ fits; one holding a shifted word does not. */
static uint16_t clz16(uint32_t v, uint32_t sh)
{
  if (v == 0)
    return 99;
  return (uint16_t)((uint32_t)__builtin_clz(v) + (v << sh));
}

__attribute__((noinline)) uint32_t clz16_sum(const uint32_t *w, uint32_t n, uint32_t sh)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++)
  {
    uint32_t b = w[i];
    if (b != 0)
      s += clz16(b, sh);
  }
  return s;
}

int main(void)
{
  uint32_t a[3] = {0, 0, 0x100};
  uint32_t b[2] = {0, 0};
  uint32_t c[1] = {0x80000000u};
  uint32_t d[4] = {0xF0u, 0, 0x1u, 0x7Fu};
  printf("first_set %u %u %u %u\n", (unsigned)first_set(a, 3), (unsigned)first_set(b, 2), (unsigned)first_set(c, 1),
         (unsigned)first_set(a, 0));
  printf("wrap_sum %u %u\n", (unsigned)wrap_sum(d, 4, 0x20u), (unsigned)wrap_sum(d, 4, 0x1F0u));
  printf("sbyte_sum %d %d\n", (int)sbyte_sum(d, 4, 0x10u), (int)sbyte_sum(d, 4, 0x1u));
  printf("clz16_sum %u %u\n", (unsigned)clz16_sum(d, 4, 0), (unsigned)clz16_sum(d, 4, 12));
  return 0;
}
