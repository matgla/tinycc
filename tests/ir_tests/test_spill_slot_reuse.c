/* Spill slots are reused once their interval has expired (ra_spill_pool_*).
 * High register pressure in several phases, so many short-lived values spill
 * and hand their slots on; loop-carried values and an eviction victim must
 * keep their slot for their whole live range.  Every phase is checked. */
#include <stdio.h>

__attribute__((noinline)) static unsigned mix(unsigned a, unsigned b) { return a * 31u + b; }

__attribute__((noinline)) static unsigned phases(unsigned s)
{
  unsigned total = 0;
  /* Phase 1: 14 simultaneously live values force spills. */
  unsigned a0 = s + 1, a1 = s + 2, a2 = s + 3, a3 = s + 4, a4 = s + 5, a5 = s + 6, a6 = s + 7;
  unsigned a7 = s + 8, a8 = s + 9, a9 = s + 10, a10 = s + 11, a11 = s + 12, a12 = s + 13, a13 = s + 14;
  total += mix(a0, a13) + mix(a1, a12) + mix(a2, a11) + mix(a3, a10) + mix(a4, a9) + mix(a5, a8) + mix(a6, a7);
  /* Phase 2: a new set that may take phase 1's freed slots. */
  unsigned b0 = total ^ 1, b1 = total ^ 2, b2 = total ^ 3, b3 = total ^ 4, b4 = total ^ 5, b5 = total ^ 6;
  unsigned b6 = total ^ 7, b7 = total ^ 8, b8 = total ^ 9, b9 = total ^ 10, b10 = total ^ 11, b11 = total ^ 12;
  total += mix(b0, b11) + mix(b1, b10) + mix(b2, b9) + mix(b3, b8) + mix(b4, b7) + mix(b5, b6);
  /* Phase 3: loop-carried values live across calls while temporaries churn. */
  unsigned c0 = total, c1 = total >> 1, c2 = total >> 2, c3 = total >> 3;
  for (unsigned i = 0; i < 10; i++)
  {
    unsigned t0 = mix(c0, i), t1 = mix(c1, i), t2 = mix(c2, i), t3 = mix(c3, i);
    unsigned t4 = t0 ^ t1, t5 = t2 ^ t3, t6 = t4 + t5, t7 = t6 * 3u;
    c0 += t7 + t0;
    c1 ^= t6 + t1;
    c2 += t5 + t2;
    c3 ^= t4 + t3;
  }
  return total + c0 + c1 + c2 + c3;
}

__attribute__((noinline)) static unsigned long long wide(unsigned long long x)
{
  /* 64-bit values spill as 8-byte slots. */
  unsigned long long w0 = x + 1, w1 = x * 3, w2 = x ^ 0x55, w3 = x << 3, w4 = x >> 2, w5 = x + 77;
  unsigned long long r = mix((unsigned)w0, (unsigned)w5) + w1 * w4 + (w2 ^ w3);
  unsigned long long v0 = r + 5, v1 = r * 7, v2 = r ^ 0xaa, v3 = r << 1, v4 = r >> 3, v5 = r + 99;
  return r + mix((unsigned)v0, (unsigned)v5) + v1 * v4 + (v2 ^ v3);
}

int main(void)
{
  unsigned acc = 0;
  for (unsigned s = 0; s < 4; s++)
    acc = acc * 7u + phases(s);
  printf("%u\n", acc);
  printf("%llu\n", wide(12345));
  return 0;
}
