/*
 *  TCC IR - loop-invariant carriers the first loop split could not take
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ra:loop_split skipped graph-coalesced values (replay: `bases` merged from two
 * defs) and ran only after the first scan (apply3: carriers the rescan evicts
 * after other splits), so those carriers reloaded every iteration.  The yasld
 * template replay loops have both shapes.  Fix: members of a class and values
 * with several defs outside the loop take entry copies, and the split runs again
 * after the rescan. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Entry { uint32_t at; uint32_t value; };

__attribute__((noinline)) uint32_t replay(const struct Entry *list, uint32_t len, const uint32_t *b0,
                                          const uint32_t *b1, int which, uint8_t *data, uint32_t data_words,
                                          uint32_t start, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
                                          uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8,
                                          uint32_t p9, uint32_t p10, uint32_t p11)
{
  const uint32_t *bases;
  if (which)
    bases = b0;
  else
    bases = b1 + 1;
  uint32_t q0 = p0 * 3u ^ p1, q1 = p1 * 5u ^ p2, q2 = p2 * 7u ^ p3, q3 = p3 * 9u ^ p4;
  uint32_t q4 = p4 * 11u ^ p5, q5 = p5 * 13u ^ p6, q6 = p6 * 15u ^ p7, q7 = p7 * 17u ^ p8;
  uint32_t q8 = p8 * 19u ^ p9, q9 = p9 * 21u ^ p10, q10 = p10 * 23u ^ p11, q11 = p11 * 25u ^ p0;
  for (uint32_t i = start; i < len; i++) {
    struct Entry en = list[i];
    uint32_t index = en.at >> 8;
    if (index >= data_words)
      return i;
    uint32_t v = en.value + bases[en.at & 255];
    memcpy(data + index * 4, &v, sizeof v);
  }
  uint32_t r = len;
  r = r * 31u + q0; r = r * 31u + q1; r = r * 31u + q2; r = r * 31u + q3;
  r = r * 31u + q4; r = r * 31u + q5; r = r * 31u + q6; r = r * 31u + q7;
  r = r * 31u + q8; r = r * 31u + q9; r = r * 31u + q10; r = r * 31u + q11;
  return r + bases[0];
}

__attribute__((noinline)) void fill(uint32_t *b)
{
  for (int i = 0; i < 8; i++)
    b[i] = (uint32_t)i << 8;
}

__attribute__((noinline)) uint32_t apply3(const struct Entry *list, uint32_t len, uint32_t *r0, uint32_t *r1,
                                          uint32_t *r2, uint32_t w0, uint32_t w1, uint32_t p0, uint32_t p1,
                                          uint32_t p2, uint32_t p3, uint32_t p4, uint32_t p5, uint32_t p6,
                                          uint32_t p7, uint32_t p8, uint32_t p9)
{
  uint32_t q0 = p0 * 3u ^ p1, q1 = p1 * 5u ^ p2, q2 = p2 * 7u ^ p3, q3 = p3 * 9u ^ p4;
  uint32_t q4 = p4 * 11u ^ p5, q5 = p5 * 13u ^ p6, q6 = p6 * 15u ^ p7, q7 = p7 * 17u ^ p8;
  uint32_t q8 = p8 * 19u ^ p9, q9 = p9 * 21u ^ p0;
  uint32_t bases[8];
  fill(bases);
  uint32_t e0 = w0, e1 = w0 + w1;
  uint32_t next = len;
  for (uint32_t i = 0; i < len; i++) {
    struct Entry e = list[i];
    uint32_t ix = e.at >> 8;
    if (ix >= e0) {
      next = i;
      break;
    }
    r0[ix] = e.value + bases[e.at & 7];
  }
  uint32_t s = next;
  next = len;
  for (uint32_t i = s; i < len; i++) {
    struct Entry e = list[i];
    uint32_t ix = e.at >> 8;
    if (ix >= e1) {
      next = i;
      break;
    }
    r1[ix - e0] = e.value + bases[e.at & 7];
  }
  for (uint32_t k = next; k + 1 < len; k += 2) {
    struct Entry a = list[k], b = list[k + 1];
    uint32_t *slot = r2 + ((a.at >> 8) - e1) * 2;
    slot[0] = a.value + bases[a.at & 7];
    slot[1] = b.value + bases[b.at & 7];
  }
  uint32_t r = next + len;
  r = r * 31u + q0; r = r * 31u + q1; r = r * 31u + q2; r = r * 31u + q3; r = r * 31u + q4;
  r = r * 31u + q5; r = r * 31u + q6; r = r * 31u + q7; r = r * 31u + q8; r = r * 31u + q9;
  return r + list[0].value + (uint32_t)(r1 - r0) + e0 + e1;
}

static struct Entry table[40];
static uint32_t area[96];

static uint32_t sum(const uint32_t *p, int n)
{
  uint32_t h = 0;
  for (int i = 0; i < n; i++)
    h = h * 31u + p[i];
  return h;
}

int main(void)
{
  int n = 0;
  for (uint32_t w = 0; w < 24; w += 2, n++)
    table[n].at = (w << 8) | (w & 7), table[n].value = w * 7u;
  for (uint32_t w = 24; w < 32; w++, n++)
    table[n].at = (w << 8) | (w & 3), table[n].value = w + 100u;
  for (uint32_t w = 32; w < 48; w += 2, n++)
    table[n].at = (w << 8) | 5, table[n].value = w * 3u;
  uint32_t b0[8], b1[9];
  for (int i = 0; i < 8; i++)
    b0[i] = 1000u + i;
  for (int i = 0; i < 9; i++)
    b1[i] = 2000u + 3u * i;

  /* every entry in range, both bases arms */
  for (int which = 0; which < 2; which++) {
    memset(area, 0, sizeof area);
    uint32_t r = replay(table, (uint32_t)n, b0, b1, which, (uint8_t *)area, 96, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                        10, 11, 12);
    printf("replay which=%d r=%08x data=%08x\n", which, (unsigned)r, (unsigned)sum(area, 96));
  }
  /* early exit at the first entry past data_words */
  memset(area, 0, sizeof area);
  uint32_t r = replay(table, (uint32_t)n, b0, b1, 1, (uint8_t *)area, 20, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                      12);
  printf("replay early r=%u data=%08x\n", (unsigned)r, (unsigned)sum(area, 96));

  /* data words [0,24), GOT words [24,32), thunk pairs from 32 */
  memset(area, 0, sizeof area);
  r = apply3(table, (uint32_t)n, area, area + 24, area + 32, 24, 8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  printf("apply3 r=%08x data=%08x\n", (unsigned)r, (unsigned)sum(area, 96));
  /* no GOT range: the second loop exits at once */
  memset(area, 0, sizeof area);
  r = apply3(table, (uint32_t)n, area, area + 24, area + 32, 24, 0, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
  printf("apply3 nogot r=%08x data=%08x\n", (unsigned)r, (unsigned)sum(area, 96));
  return 0;
}
