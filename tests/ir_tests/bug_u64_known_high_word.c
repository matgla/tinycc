/*
 *  TCC IR - a 64-bit value's known high word folds before allocation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:fold walks 64-bit AND/OR/XOR/shift/widening chains for per-bit known
 * values, so `((uint64_t)u32 ^ C) >> 32` is the constant C.hi and a product of
 * that word with a constant multiplier folds before the UMAAL fusion
 * (Wyhash's mum after IntegerContext(u16).hash's key expansion).  Pinned: a
 * sign-extended widening, a phi, a SAR fill and a still-unknown word. */

#include <stdint.h>

extern int printf(const char *, ...);

#define NI __attribute__((noinline))

#define C1 0xe7037ed1a0b428dbull
#define M1 0x1ff5c2923a788d2cull

NI uint64_t hi_of_xor(uint32_t x) { return ((uint64_t)x ^ C1) >> 32; }
NI uint32_t hi32_of_or(uint16_t k) { return (uint32_t)(((uint64_t)k | 0x1234567800000001ull) >> 32); }
NI uint64_t hi_of_and(uint32_t x) { return ((uint64_t)x | C1) & 0xffff000000000000ull; }
NI uint64_t shr40_of_xor(uint32_t x) { return ((uint64_t)x ^ C1) >> 40; }
NI int64_t sar40_of_xor(uint32_t x) { return (int64_t)((uint64_t)x ^ C1) >> 40; }
NI uint64_t shl_keeps_low(uint32_t x) { return (((uint64_t)x ^ C1) << 4) >> 32; }

NI uint64_t expand(uint16_t k)
{
  uint64_t n = k;
  return ((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8);
}

NI uint64_t hi_of_expand(uint16_t k)
{
  uint64_t n = k;
  uint64_t e = ((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8);
  return (e ^ C1) >> 32;
}

/* The schoolbook 64x64->128 product (try_inline_multi3's carry chain), low ^ high. */
#define MUM(a, b)                                                              \
  do {                                                                         \
    uint32_t a0 = (uint32_t)(a), a1 = (uint32_t)((a) >> 32);                   \
    uint32_t b0 = (uint32_t)(b), b1 = (uint32_t)((b) >> 32);                   \
    uint64_t t = (uint64_t)a0 * b0;                                            \
    uint32_t l0 = (uint32_t)t;                                                 \
    t = (uint64_t)a1 * b0 + (t >> 32);                                         \
    uint32_t w1 = (uint32_t)t, w2 = (uint32_t)(t >> 32);                       \
    t = (uint64_t)a0 * b1 + w1;                                                \
    uint64_t lo = ((uint64_t)(uint32_t)t << 32) | l0;                          \
    uint64_t hi = (uint64_t)a1 * b1 + w2 + (t >> 32);                          \
    r = lo ^ hi;                                                               \
  } while (0)

NI uint64_t mum_const_hi(uint32_t x)
{
  uint64_t a = (uint64_t)x ^ C1, r;
  MUM(a, M1);
  return r;
}

NI uint64_t u16_key_hash(uint16_t k)
{
  uint64_t n = k;
  uint64_t a = (((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8)) ^ C1, r;
  MUM(a, M1);
  return r;
}

/* Sign-extended: the high word is the sign, not zero. */
NI int64_t hi_of_signed(int32_t x) { return ((int64_t)x ^ 5) >> 32; }

/* A phi of a zero-high and a full value: nothing is known. */
NI uint64_t hi_of_phi(uint32_t x, uint64_t y, int c)
{
  uint64_t v = x;
  if (c)
    v = y;
  return (v ^ C1) >> 32;
}

/* The high word is the full 64-bit argument's: unknown. */
NI uint64_t mum_live_hi(uint64_t y)
{
  uint64_t a = y ^ C1, r;
  MUM(a, M1);
  return r;
}

#define P(e) printf(#e " %016llx\n", (unsigned long long)(e))

int main(void)
{
  static volatile uint32_t in[] = {0u, 1u, 0x80u, 0xffu, 0x1234u, 0x8001u, 0xffffu, 0x80000001u, 0xdeadbeefu,
                                   0xffffffffu};
  for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++)
  {
    uint32_t x = in[i];
    printf("-- %08x\n", (unsigned)x);
    P(hi_of_xor(x));
    P(hi32_of_or((uint16_t)x));
    P(hi_of_and(x));
    P(shr40_of_xor(x));
    P(sar40_of_xor(x));
    P(shl_keeps_low(x));
    P(expand((uint16_t)x));
    P(hi_of_expand((uint16_t)x));
    P(mum_const_hi(x));
    P(u16_key_hash((uint16_t)x));
    P(hi_of_signed((int32_t)x));
    P(hi_of_phi(x, ((uint64_t)x << 32) | 0x55u, (int)(i & 1)));
    P(mum_live_hi(((uint64_t)x << 32) | (x ^ 0x5a5a5a5au)));
  }
  return 0;
}
