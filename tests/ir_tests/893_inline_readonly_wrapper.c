/*
 *  TCC Tests - Read-only wrapper expansion and side-effect preservation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include "asm/inline_readonly_wrapper.c"

__attribute__((noinline)) static u32 reference(const u8 *p, u8 alignment)
{
  u32 n = 0;
  for (const u8 *s = p + 4; *s; s++)
    n++;
  u32 value = n + 5u;
  value += (n % 4u) * 2u;
  value ^= n / 8u;
  value += (n * 7u) % 16u;
  return ((value + alignment - 1u) / alignment) * alignment;
}

int main(void)
{
  if (forced_name((const u8 *)"____sample") != 6)
    return 3;
  u8 p[192], q[192], r[192];
  for (u32 n = 0; n < 130; n++) {
    u32 lengths[3] = {n, (n * 5u + 7u) % 130u, (n * 11u + 3u) % 130u};
    u8 *records[3] = {p, q, r};
    for (u32 j = 0; j < 3; j++) {
      for (u32 i = 0; i < 4u + lengths[j]; i++)
        records[j][i] = (u8)(1u + i % 254u);
      records[j][4u + lengths[j]] = 0;
    }
    if (sum_name(n) != n * (n + 1u) / 2u)
      return 3;
    for (u32 bit = 0; bit < 8; bit++) {
      u8 a = (u8)(1u << bit);
      u32 x = reference(p, a), y = reference(q, a), z = reference(r, a);
      if (one_size(p, a) != x || two_sizes(p, q, a) != x + y ||
          three_sizes(p, q, r, a) != x + y + z)
        return 1;
      observations = 0;
      if (observed_one(p, a) != x || observed_two(p, a) != x + reference(p + 1, a) ||
          observations != 3)
        return 2;
    }
  }
  puts("readonly wrapper ok");
  return 0;
}
