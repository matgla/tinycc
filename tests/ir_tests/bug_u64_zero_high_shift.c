/*
 *  TCC IR - 64-bit shifts and literal ops on a value whose high word is zero
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* zero_half64 + thumb_emit_shift64_mop: a shift by k < 32, or an OR/XOR/AND
 * with a literal, of a value whose high word is provably zero now builds the
 * result from the low word alone, and the widening's `mov hi, #0` dies.  The
 * per-word may-be-set masks prove `(n >> 8) << 8` and `(n & 0xff) << 16` keep
 * a zero high word.  Refusals pinned: multi-def values, a high word read
 * elsewhere, SAR of a maybe-negative value, a source with a live high word. */

#include <stdint.h>

extern int printf(const char *, ...);

#define NI __attribute__((noinline))

#define SHIFTS(T, tag, k)                                                                \
  NI uint64_t shl_##tag##_##k(T x) { uint64_t n = x; return n << k; }                  \
  NI uint64_t shr_##tag##_##k(T x) { uint64_t n = x; return n >> k; }                  \
  NI int64_t sar_##tag##_##k(T x) { int64_t n = (int64_t)(uint64_t)x; return n >> k; }

#define ALLK(T, tag) SHIFTS(T, tag, 1) SHIFTS(T, tag, 8) SHIFTS(T, tag, 16) SHIFTS(T, tag, 31) \
  SHIFTS(T, tag, 32) SHIFTS(T, tag, 40)

ALLK(uint8_t, u8)
ALLK(uint16_t, u16)
ALLK(uint32_t, u32)

NI uint64_t xor_lit(uint16_t k) { uint64_t n = k; return n ^ 0xe7037ed1a0b428dbull; }
NI uint64_t or_lit(uint16_t k) { uint64_t n = k; return n | 0x1234567800000001ull; }
NI uint64_t and_lit(uint16_t k) { uint64_t n = k; return n & 0xffffffff0000ff0full; }

/* IntegerContext(u16).hash's key expansion. */
NI uint64_t expand(uint16_t k)
{
  uint64_t n = k;
  return ((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8);
}

NI uint64_t mul_hi64(uint64_t a, uint64_t b)
{
  uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
  uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
  uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
  return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

NI uint64_t key_hash(uint16_t k)
{
  uint64_t a = expand(k);
  uint64_t n = k;
  uint64_t b = ((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8);
  a = b ^ 0xe7037ed1a0b428dbull;
  uint64_t lo = a * 0x1ff5c2923a788d2cull;
  uint64_t hi = mul_hi64(a, 0x1ff5c2923a788d2cull);
  return (lo ^ 0xa0761d6478bd642full) ^ hi;
}

/* Masks through a chain: the high word is zero after every step. */
NI uint64_t chain(uint32_t x)
{
  uint64_t n = x;
  uint64_t a = (n & 0xffff) << 12;   /* lo <= 0x0ffff000 */
  uint64_t b = (a >> 4) << 3;        /* still below 2^32 */
  return (b << 3) ^ 0x80000000ull;   /* hi = b >> 29, maybe nonzero */
}

/* The zero-high value is also stored whole: its high word must be written. */
uint64_t g_sink;
NI uint64_t also_stored(uint16_t k)
{
  uint64_t n = k;
  g_sink = n;
  return n << 20;
}

/* Two definitions: nothing is known about `v`. */
NI uint64_t two_defs(uint16_t k, uint64_t y, int c)
{
  uint64_t v = k;
  if (c)
    v = y;
  return (v << 8) | (v >> 4);
}

/* A full 64-bit source: the high word is live and must be shifted in. */
NI uint64_t live_hi(uint64_t y) { return (y << 9) ^ (y >> 9); }

/* SAR of a value whose high word may carry the sign. */
NI int64_t sar_maybe_neg(int64_t y, int c) { int64_t v = c ? y : (int64_t)(uint16_t)y; return v >> 12; }

/* Loop-carried: the def follows its use. */
NI uint64_t loop_carried(uint16_t k, int n)
{
  uint64_t v = k;
  for (int i = 0; i < n; i++)
    v = (v << 13) ^ (v >> 7) ^ 0x9e3779b97f4a7c15ull;
  return v;
}

/* A volatile local lives in its stack slot and every use reads it from memory,
 * so its zero high word is never "known" at the use -- and its def must still
 * store both words.  The fix once deleted the high-word store while the use
 * still loaded it (garbage in the high word). */
NI uint64_t vol_shl(uint16_t k) { volatile uint64_t n = k; return n << 9; }
NI uint64_t vol_shr(uint16_t k) { volatile uint64_t n = k; return n >> 40; }
NI uint64_t vol_xor_lit(uint16_t k) { volatile uint64_t n = k; return n ^ 0x1234567800000001ull; }
NI uint64_t vol_or_reg(uint16_t k, uint64_t y) { volatile uint64_t n = k; return n | y; }
NI uint64_t vol_and_lit(uint16_t k) { volatile uint64_t n = k; return n & 0xffffffff0000ffffull; }
NI void vol_write_only(uint16_t k) { volatile uint64_t n = k; (void)0; }

/* Fill the stack below the caller with non-zero junk, so a volatile slot
 * whose high word is never written reads back garbage. */
NI void dirty_stack(void)
{
  volatile uint32_t junk[32];
  for (int i = 0; i < 32; i++)
    junk[i] = 0xdeadbeefu ^ (uint32_t)(i * 0x01010101u);
}
#define PV(e) do { dirty_stack(); P(e); } while (0)

#define P(e) printf(#e " %016llx\n", (unsigned long long)(e))
#define PK(tag, T, x)                                                                     \
  do {                                                                                    \
    P(shl_##tag##_1((T)(x))); P(shl_##tag##_8((T)(x))); P(shl_##tag##_16((T)(x)));       \
    P(shl_##tag##_31((T)(x))); P(shl_##tag##_32((T)(x))); P(shl_##tag##_40((T)(x)));     \
    P(shr_##tag##_1((T)(x))); P(shr_##tag##_8((T)(x))); P(shr_##tag##_16((T)(x)));       \
    P(shr_##tag##_31((T)(x))); P(shr_##tag##_32((T)(x))); P(shr_##tag##_40((T)(x)));     \
    P(sar_##tag##_1((T)(x))); P(sar_##tag##_8((T)(x))); P(sar_##tag##_16((T)(x)));       \
    P(sar_##tag##_31((T)(x))); P(sar_##tag##_32((T)(x))); P(sar_##tag##_40((T)(x)));     \
  } while (0)

int main(void)
{
  static volatile uint32_t in[] = {0u, 1u, 0x80u, 0xffu, 0x1234u, 0x8001u, 0xffffu, 0x80000001u, 0xdeadbeefu,
                                   0xffffffffu};
  for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++)
  {
    uint32_t x = in[i];
    printf("-- %08x\n", (unsigned)x);
    PK(u8, uint8_t, x);
    PK(u16, uint16_t, x);
    PK(u32, uint32_t, x);
    P(xor_lit((uint16_t)x));
    P(or_lit((uint16_t)x));
    P(and_lit((uint16_t)x));
    P(expand((uint16_t)x));
    P(key_hash((uint16_t)x));
    P(chain(x));
    P(also_stored((uint16_t)x));
    P(g_sink);
    P(two_defs((uint16_t)x, ((uint64_t)x << 32) | 0x55u, (int)(i & 1)));
    P(live_hi(((uint64_t)x << 32) | (x ^ 0x5a5a5a5au)));
    P(sar_maybe_neg(-(int64_t)x - 7, (int)(i & 1)));
    P(loop_carried((uint16_t)x, (int)(i & 3)));
    PV(vol_shl((uint16_t)x));
    PV(vol_shr((uint16_t)x));
    PV(vol_xor_lit((uint16_t)x));
    PV(vol_or_reg((uint16_t)x, 0x10u));
    PV(vol_and_lit((uint16_t)x));
    vol_write_only((uint16_t)x);
  }
  return 0;
}
