/*
 *  TCC Tests - dense constant switch with a memory-resolved merge slot
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/* The switch_to_data pass also rewrites the memory-resolved phi form, where
 * every arm stores its constant into the merge temp's home slot and the join
 * reads the slot once: the dispatch becomes a table load.  These functions
 * return constants through a switch so the phi resolves through memory at
 * -O1/-O2 (values below 256 exercise the byte-enum shapes; larger ones the
 * full-word table). */

#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) uint8_t map_small(uint8_t t)
{
  switch (t) {
  case 0: return 8;
  case 1: return 4;
  case 2: return 1;
  case 3: return 2;
  case 4: return 3;
  case 5: return 9;
  case 6: return 10;
  case 7: return 11;
  }
  return 0;
}

__attribute__((noinline)) int32_t map_wide(uint32_t t)
{
  switch (t & 7u) {
  case 0: return 1000000;
  case 1: return -2;
  case 2: return 3;
  case 3: return 77;
  case 4: return 1 << 20;
  case 5: return 42;
  case 6: return -123456;
  case 7: return 5;
  }
  return -1;
}

/* The result feeds straight into addressing: a wrong table entry reads out
 * of line and the checksum differs. */
static const uint32_t table[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

__attribute__((noinline)) uint32_t through_addressing(uint8_t t)
{
  uint8_t m = map_small(t);
  return table[m & 15u] + (uint32_t)map_wide(m);
}

int main(void)
{
  const uint8_t small[] = {8, 4, 1, 2, 3, 9, 10, 11};
  for (uint8_t i = 0; i < 8; i++)
    if (map_small(i) != small[i])
      return 1;
  const int32_t wide[] = {1000000, -2, 3, 77, 1 << 20, 42, -123456, 5};
  for (uint32_t i = 0; i < 8; i++)
    if (map_wide(i) != wide[i])
      return 2;
  /* Out-of-range index: masked to 0..7 before the table, so the masked
   * mapping still has to hold. */
  for (uint32_t i = 8; i < 16; i++)
    if (map_wide(i) != wide[i & 7u])
      return 3;
  for (uint8_t i = 0; i < 8; i++)
    if (through_addressing(i) != table[small[i] & 15u] + (uint32_t)wide[small[i] & 7u])
      return 4;
  if (map_small(200) != 0)
    return 5;
  puts("ok");
  return 0;
}
