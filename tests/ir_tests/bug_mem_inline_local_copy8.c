/*
 *  TCC IR - Two-piece mem* copies through a local's address
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* mem_inline expands memcpy only for one piece (n in {1,2,4}), so the Zig C
 * backend's spelling of an align(1) 8-byte read - `memcpy(&local, p, 8)` into
 * a temporary that is only read back - stays a `bl memcpy` plus a stack slot.
 * When either end resolves through a single-def `T <- &slot` temp the two-word
 * expansion is a win: store-load forwarding and dead-slot elimination delete the
 * slot, leaving plain loads.  The controls below pin that a copy whose ends are
 * both plain pointers keeps the runtime call. */

#include <stdio.h>
#include <string.h>

typedef unsigned u32;

struct E {
  u32 at, value;
};

/* The defect site: an 8-byte copy into a local whose only purpose is to be
 * read back. */
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

/* The store-side twin: an align(1) 8-byte write out of a local. */
void put_at(unsigned char *dst, u32 at, u32 value)
{
  struct E e;
  e.at = at;
  e.value = value;
  memcpy(dst, &e, 8);
}

/* Control: both ends are plain pointers, so the strict one-piece policy keeps
 * the runtime call. */
void cp8_plain(char *d, const char *s) { memcpy(d, s, 8); }

int main(void)
{
  struct E entries[3];
  unsigned char put[8];
  char ctl[8];
  u32 want = 0;
  int i;

  entries_init:
  entries[0].at = 0x11u;   entries[0].value = 0x111u;
  ...
  return 0;
}
