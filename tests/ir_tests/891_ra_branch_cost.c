/*
 *  TCC Tests - Conditional loop spill costs
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include "asm/ra_branch_cost.c"

static volatile u32 sink;

__attribute__((noinline)) u32 cold(u32 x)
{
  sink += x;
  return (x * 13u + 7u) ^ 0x98765432u;
}

int main(void)
{
  Entry entries[64];
  u32 p[7], bases[8], tab[64];
  for (u32 i = 0; i < 7; i++)
    p[i] = 0x9e3779b9u * (i + 1u);
  for (u32 i = 0; i < 8; i++)
    bases[i] = i * 1234567u;
  for (u32 mode = 0; mode < 4; mode++) {
    for (u32 i = 0; i < 64; i++) {
      u32 flags = mode == 0 ? 0 : mode == 1 ? 56u : mode == 2 ? (i % 9u == 4u ? 8u : 0u) : (i * 13u & 56u);
      entries[i].header = (i * 17u << 8) | flags | (i & 7u);
      entries[i].value = i * 2654435761u;
      tab[i] = entries[i].value;
    }
    printf("%08x %08x %08x %08x\n", replay(entries, 0, p, bases), replay(entries, 1, p, bases),
           replay(entries, 31, p, bases), replay(entries, 64, p, bases));
  }
  printf("%08x %08x\n", hot_walk(tab, 0, 11), hot_walk(tab, 64, 11));
  return 0;
}
