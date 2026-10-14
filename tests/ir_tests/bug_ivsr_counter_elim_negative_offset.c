/*
 * ssa:iv_strength_reduction: counter elimination compared a derived integer offset that
 * starts negative with an unsigned condition, so the loop was skipped.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) int64_t f(const uint8_t *p, int i)
{
  uint32_t x = (uint32_t)i * 0x01010101u, r = 0;
  for (int k = 0; k < 3; k++)
  {
    uint32_t v = ((uint32_t)p[k * 4 + -5 + 0] << 24) | ((uint32_t)p[k * 4 + -5 + 1] << 16) |
                 ((uint32_t)p[k * 4 + -5 + 2] << 8) | (uint32_t)(p[k * 4 + -5 + 3] & 0xff);
    r = r * 33u + (v > 0x7fffu ? 3u : 5u);
  }
  return (int64_t)(r + x);
}

uint8_t big[64];

int main(void)
{
  for (int k = 0; k < 64; k++)
    big[k] = (uint8_t)(k * 131 + 0x5b);
  printf("%08x\n", (unsigned)f(big + 20, 0));
  printf("%08x\n", (unsigned)f(big + 20, 1));
  return 0;
}
