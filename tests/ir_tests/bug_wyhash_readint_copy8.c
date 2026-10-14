/*
 *  TCC IR - Streaming Wyhash eight-byte copy readback regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Zig's mem.readInt(u64, .little) spells an unaligned eight-byte load as a
 * by-value align(1) u8[8] struct copy into a local (`t29 = (*t27)`), read
 * back through a constant-trip countdown assembling the little-endian word.
 * vstore's small-aggregate path tiled that copy at byte width, hit the
 * four-chunk ceiling, and emitted an opaque __aeabi_memmove call instead:
 * inside Wyhash.update's block loop every input word then paid the call
 * plus a stack round-trip (docs/bugs/kernel-streaming-wyhash-byte-assembly.md).
 * The fix lets byte tiles run to eight chunks so the pieces store-forward
 * and load_combine fuses the readback into a wide load of the source.
 * test_codegen_asm::test_wyhash_readint_copy8 pins that no copy call and no
 * byte-store round-trip remain. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define zig_noinline __attribute__((noinline))

struct arr_8_u8 { uint8_t array[8]; } __attribute__((aligned(1)));

/* The defect site: countdown assembly over a by-value align(1) 8-byte copy. */
static zig_noinline uint64_t read_u64_le(const uint8_t *p)
{
  struct arr_8_u8 t = *(const struct arr_8_u8 *)p;
  uint64_t acc = 0;
  uintptr_t i = 7;
  for (;;) {
    uint64_t v = (uint64_t)t.array[i];
    acc = (acc << 8) | v;
    if (i == 0)
      break;
    i -= 1;
  }
  return acc;
}

/* The loop context it must also survive: Wyhash.update's 48-byte block loop
 * reads six words per iteration through the same copy idiom. */
static zig_noinline uint64_t mix3(uint64_t a, uint64_t b, uint64_t seed)
{
  return (a ^ seed) * (b ^ seed) * UINT64_C(0x9E3779B97F4A7C15);
}

zig_noinline uint64_t round48(const uint8_t *p, uintptr_t len, uint64_t seed)
{
  uint64_t h = seed;
  while (len >= 48) {
    h = mix3(h ^ read_u64_le(p), read_u64_le(p + 8), h);
    h = mix3(h ^ read_u64_le(p + 16), read_u64_le(p + 24), h);
    h = mix3(h ^ read_u64_le(p + 32), read_u64_le(p + 40), h);
    p += 48;
    len -= 48;
  }
  return h;
}

/* Unaligned inputs must read the same bytes (word pieces of a possibly
 * unaligned source stay UNDERALIGN-marked, never LDRD). */
zig_noinline uint64_t odd_sum(const uint8_t *p, uintptr_t len)
{
  uint64_t s = 0;
  for (uintptr_t k = 0; k + 8 <= len; k += 3)
    s += read_u64_le(p + k);
  return s;
}

int main(void)
{
  static const uint8_t buf[64] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
  };
  printf("%llu %llu %llu\n",
         (unsigned long long)read_u64_le(buf),
         (unsigned long long)round48(buf, 48, 42),
         (unsigned long long)odd_sum(buf, 60));
  return 0;
}
