/*
 *  TCC IR - Eight-byte copies through a local's address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* mem_inline expands a small constant-size memcpy only for one piece
 * (MI_MEMCPY_MAX 4), so the Zig C backend's spelling of an align(1) 8-byte
 * load - `memcpy(&local, p, 8)` into a temporary that is only read back -
 * stays a real call plus a stack slot.  When either end resolves through a
 * single-def `T <- &slot` temp the two-piece expansion wins: store-load
 * forwarding and dead-slot elimination delete the slot.  This test pins the
 * copied bytes in both directions and that a plain pointer-to-pointer copy
 * keeps the call. */

#include <stdio.h>
#include <string.h>

typedef unsigned u32;

struct E {
  u32 at, value;
};

/* The defect site: copy into a local whose only purpose is to be read. */
u32 sum_at(const unsigned char *list, u32 n)
{
  u32 t = 0;
  for (u32 i = 0; i < n; i++) {
    struct E e;
    memcpy(&e, list + i * 8, 8);
    t += e.at + e.value;
  }
  return t;
}

/* The store-side twin: an align(1) 8-byte store out of a local. */
void put_at(unsigned char *dst, u32 at, u32 value)
{
  struct E e;
  e.at = at;
  e.value = value;
  memcpy(dst, &e, 8);
}

/* Control: both ends are plain pointers - the strict policy keeps the call. */
void cp8_plain(char *d, const char *s) { memcpy(d, s, 8); }

int main(void)
{
  struct E entries[3];
  char ctl[8];
  u32 want = 0, got;
  int i;

  entries[0].at = 0x11223344u;
  entries[0].value = 0x55666667u;
  entries[1].at = 0x89abcdefu;
  entries[1].value = 0xfedcba98u;
  entries[2].at = 1u;
  entries[2].value = 0xfffffffeu;

  for (i = 0; i < 3; i++)
    want += entries[i].at + entries[i].value;
  got = sum_at((const unsigned char *)entries, 3);
  printf("sum want=%u got=%u\n", want, got);

  put_at(ctl, 0x11223333u, 0x44556666u);
  for (i = 0; i < 8; i++)
    printf("%02x", ctl[i]);
  puts("");

  cp8_plain(ctl, "abcdefgh");
  for (i = 0; i < 8; i++)
    printf("%c", ctl[i]);
  puts("");
  return 0;
}
