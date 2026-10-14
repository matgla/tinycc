/*
 *  TCC Tests - Conditional loop spill costs
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

typedef unsigned u32;
typedef struct Entry { u32 header, value; } Entry;
u32 cold(u32 x);

__attribute__((noinline)) u32 replay(const Entry *entries, u32 count, const u32 *p, const u32 *bases)
{
  u32 v0 = p[0] * 3u, v1 = p[1] ^ 0x1234u, v2 = p[2] + 77u, v3 = p[3] * 5u;
  u32 v4 = p[4] - 9u, v5 = p[5] * 11u, v6 = p[6] ^ 0xbeefu;
  u32 acc = 0;
  for (u32 i = 0; i < count; i++) {
    u32 header = entries[i].header, value = entries[i].value;
    u32 offset = header >> 8;
    if ((header & 8u) != 0)
      acc += cold(value + offset);
    if ((header & 16u) != 0)
      acc += v4 * offset + v5;
    if ((header & 32u) != 0)
      acc ^= v6 + offset;
    acc += bases[header & 7u] + value + ((v0 ^ i) + (v1 & offset) + (v2 | value) + (v3 - i));
  }
  return acc;
}

__attribute__((noinline)) u32 hot_walk(const u32 *tab, u32 n, u32 seed)
{
  u32 acc = seed;
  for (u32 i = 0; i < n; i++)
    acc = cold(acc + tab[i]) ^ i;
  return acc;
}
