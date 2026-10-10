/*
 *  TCC Tests - 64-bit byte-load combining
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdint.h>

#define LE64(p) ((uint64_t)(p)[0] | (uint64_t)(p)[1] << 8 | \
                 (uint64_t)(p)[2] << 16 | (uint64_t)(p)[3] << 24 | \
                 (uint64_t)(p)[4] << 32 | (uint64_t)(p)[5] << 40 | \
                 (uint64_t)(p)[6] << 48 | (uint64_t)(p)[7] << 56)

__attribute__((noinline)) uint64_t le64(const uint8_t *p) { return LE64(p); }
__attribute__((noinline)) uint64_t le64_off(const uint8_t *p) { return LE64(p + 3); }
__attribute__((noinline)) uint64_t vol_le64(const volatile uint8_t *p) { return LE64(p); }

__attribute__((noinline)) uint64_t zig_rd8(const uint8_t *p)
{
  struct bytes8 { uint8_t array[8]; } bytes = *(const struct bytes8 *)p;
  uintptr_t i = 7;
  uint64_t acc = 0;
  for (;;) {
    acc = (acc << 8) | (uint64_t)bytes.array[i];
    if (i == 0)
      goto done;
    --i;
  }
done:
  return acc;
}

__attribute__((noinline)) uint64_t be64(const uint8_t *p)
{
  uint64_t acc = 0;
  for (unsigned i = 0; i < 8; ++i)
    acc = (acc << 8) | p[i];
  return acc;
}

__attribute__((noinline)) uint64_t signed_low32(const uint8_t *p)
{
  int32_t lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
               (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  return (uint64_t)(int64_t)lo | (uint64_t)p[4] << 32 |
         (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

__attribute__((noinline)) uint64_t trunc_low32(const uint8_t *p)
{
  uint32_t lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  return (uint64_t)(lo << 8) | (uint64_t)p[4] << 32 |
         (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

__attribute__((noinline)) uint64_t store_between64(uint8_t *p)
{
  uint64_t lo = (uint64_t)p[0] | (uint64_t)p[1] << 8 |
                (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24;
  p[0] ^= 0xff;
  return lo | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
         (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}
