/*
 *  TCC Tests - 64-bit assembly preserves alignment, extension and memory order
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include "asm/readint64_combine.c"
#include <stdio.h>

static uint64_t reference(const volatile uint8_t *p, int big)
{
  uint64_t acc = 0;
  for (unsigned i = 0; i < 8; ++i)
    acc |= (uint64_t)p[i] << (8 * (big ? 7 - i : i));
  return acc;
}

int main(void)
{
  uint8_t data[20];
  for (unsigned seed = 0; seed < 256; ++seed) {
    for (unsigned i = 0; i < sizeof data; ++i)
      data[i] = seed + i * 37;
    for (unsigned off = 0; off < 4; ++off) {
      uint8_t *p = data + off;
      uint64_t ref = reference(p, 0);
      if (le64(p) != ref || le64_off(p) != reference(p + 3, 0) ||
          zig_rd8(p) != ref || vol_le64(p) != ref || be64(p) != reference(p, 1))
        return 1;
      uint64_t signed_ref = (uint64_t)(int64_t)(int32_t)(uint32_t)ref | (ref & UINT64_C(0xffffffff00000000));
      uint64_t trunc_ref = (uint64_t)(uint32_t)((uint32_t)ref << 8) | (ref & UINT64_C(0xffffffff00000000));
      if (signed_low32(p) != signed_ref || trunc_low32(p) != trunc_ref)
        return 2;
      if (store_between64(p) != ref || p[0] != ((uint8_t)ref ^ 0xff))
        return 3;
      p[0] ^= 0xff;
    }
  }
  puts("readint64 ok");
  return 0;
}
