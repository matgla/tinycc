/*
 *  TCC IR - Byte-assembled word compares round-trip through the stack
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A by-value align(1) 4-byte struct copy into a local read back byte-wise
 * leaves each group as four byte STOREs into stack slots plus four reload
 * LOADs: sl_forward runs before the constant-trip countdown is unrolled in
 * the RA pipeline, so the post-unroll fixed-offset slot loads are never
 * store-forwarded.  The fix forwards the stored byte values into their
 * readbacks.  This test pins the compare results and the assembled
 * big-endian words; test_codegen_asm::test_eqlbytes_local_byte_roundtrip
 * pins that the [sp] traffic goes. */

#include <stdio.h>
#include <stdint.h>

struct align1_u32x4 { uint8_t b[4]; } __attribute__((aligned(1)));

/* The defect site: both groups round-trip through stack slots. */
int eql4(const uint8_t *a, const uint8_t *b, uintptr_t n)
{
  for (uintptr_t off = 0; off + 4 <= n; off += 4) {
    struct align1_u32x4 va = *(const struct align1_u32x4 *)(a + off);
    struct align1_u32x4 vb = *(const struct align1_u32x4 *)(b + off);
    uint32_t x = 0, y = 0;
    for (uintptr_t k = 4; k-- > 0;) {
      x = (x << 8) | va.b[k];
      y = (y << 8) | vb.b[k];
    }
    if (x != y)
      return 0;
  }
  return 1;
}

/* Pins the assembled big-endian values, not just the comparison. */
uint32_t be_sum(const uint8_t *a, uintptr_t n)
{
  uint32_t s = 0;
  for (uintptr_t off = 0; off + 4 <= n; off += 4) {
    struct align1_u32x4 va = *(const struct align1_u32x4 *)(a + off);
    uint32_t x = 0;
    for (uintptr_t k = 4; k-- > 0;)
      x = (x << 8) | va.b[k];
    s += x;
  }
  return s;
}

int main(void)
{
  static const uint8_t p[12] = { 0xde, 0xad, 0xbe, 0xef,
                                 0x11, 0x22, 0x33, 0x44,
                                 0x55, 0x66, 0x77, 0x88 };
  static const uint8_t q[12] = { 0xde, 0xad, 0xbe, 0xef,
                                 0x11, 0x22, 0x33, 0x45,
                                 0x55, 0x66, 0x77, 0x88 };

  printf("%d %d %u %d\n", eql4(p, p, 12), eql4(p, q, 12),
         be_sum(p, 12), eql4(p, q, 4));
  return 0;
}
