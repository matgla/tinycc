/*
 *  TCC IR - Two-piece mem* copies through a local's address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* mem_inline expands a small constant-size memcpy only for one piece
 * (MI_MEMCPY_MAX 4), so `memcpy(&local, p, 8)` - the Zig C backend's
 * spelling of an align(1) 8-byte load - stays a real call plus a stack
 * temporary.  When either end resolves through a single-def `T <- &slot`
 * temp, the two-piece expansion is a win: store-load forwarding and dead-slot
 * elimination delete the slot, leaving the two words in registers.  The
 * controls (both ends plain pointers) must keep the call. */

#include <stdio.h>
#include <string.h>

typedef unsigned u32;

struct E {
  u32 at, value;
};

/* The defect site: an 8-byte copy into a local whose only purpose is to be
 * read back field-wise. */
__attribute__((noinline)) static u32 sum_at(const unsigned char *list, u32 n)
{
  u32 t = 0;
  for (u32 i = 0; i < n; i++) {
    struct E e;
    memcpy(&e, list + i * 8, 8);
    t += e.at + e.value;
  }
  return t;
}

/* The store-side twin: an align(1) 8-byte write out of a local. */
static void put_at(unsigned char *dst, u32 at, u32 value)
{
  struct E e;
  e.at = at;
  e.value = value;
  memcpy(dst, &e, 8);
}

/* Control: neither end resolves through a slot temp - the strict policy keeps
 * the runtime call. */
void cp8_plain(char *d, const char *s) { memcpy(d, s, 8); }

int main(void)
{
  struct E entries[3];
  unsigned char put_buf[8];
  char out[8];
  const char src[8] = { 0xde, 0xad, 0xbe, 0xef, 0x11, 0x22, 0x33, 0x44 };
  u32 want = 0;
  int i;

  entries[0].at = 0x11u; entries[0].value = 0x111u;
  entries[1].at = 0x22u; entries[1].value = 0x222u;
  entries[2].at = 0x33u; entries[2].value = 0x333u;

  for (i = 0; i < 3; i++)
    want += entries[i].at + entries[i].value;

  /* The expanded load pair must reproduce the copy's bytes. */
  printf("sum %u %u\n", want,
         sum_at((const unsigned char *)entries, 3));

  /* The expanded store pair must land in the destination's byte order. */
  put_at(out, 0xdeadbeefu, 0xcafef00du);
  for (i = 0; i < 8; i++)
    printf("%02x", out[i]);
  puts("");

  /* Control: the call path still copies. */
  cp8_plain(out, "abcdefgh");
  for (i = 0; i < 8; i++)
    printf("%02x", (unsigned char)out[i]);
  puts("");
  return 0;
}
