/*
 *  TCC IR - field-pointer LICM must not load above a zero-trip guard
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ssa:licm_field_ptr hoisted `r->bm.masks` above the loop's zero-trip test,
 * so probe(bad, x, 0) read memory the source never touches.  The hoist now
 * needs the load to run on every trip, and a top-tested loop gets the load
 * behind a copy of its guard.  BAD is a bus-faulting address on mps2-an505. */
#include <stdint.h>
#include <stdio.h>

struct BitSet { uint32_t len; uint32_t *masks; };
struct Region { uint32_t a, b, c, d, e; struct BitSet bm; };

struct Dyn { uint32_t bit_length; uint32_t *masks; };       /* bit_set.Dynamic */
struct BM { void *ptr; const void *vtable; struct Dyn u; }; /* DynamicManaged */
struct ZRegion { uint32_t start, page_count, page_size; struct BM bm; };

static inline int is_set(const struct BitSet *b, uint32_t i)
{
  return (b->masks[i >> 5] & (1u << (i & 31))) != 0;
}

static int is_set_val(struct Dyn d, uint32_t i)
{
  return (d.masks[i >> 5] & (1u << (i & 31))) != 0;
}

/* Top-tested, early return in the body. */
__attribute__((noinline)) uint32_t probe(const struct Region *r, uint32_t start, uint32_t off)
{
  while (off > 0) {
    off--;
    if (is_set(&r->bm, start + off))
      return start + off;
  }
  return (uint32_t)-1;
}

/* The same probe reading the masks out of a by-value copy. */
__attribute__((noinline)) uint32_t probe_byval(const struct ZRegion *r, uint32_t start, uint32_t off)
{
  while (off > 0) {
    off--;
    if (is_set_val(r->bm.u, start + off))
      return start + off;
  }
  return (uint32_t)-1;
}

/* Counted loop: the guard is the only way round the load. */
__attribute__((noinline)) uint32_t count_set(const struct Region *r, uint32_t start, uint32_t n)
{
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++)
    c += is_set(&r->bm, start + i);
  return c;
}

/* A break ahead of the load: the first trip may leave without reading. */
__attribute__((noinline)) uint32_t count_until(const struct Region *r, uint32_t n, uint32_t stop)
{
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (i == stop)
      break;
    c += is_set(&r->bm, i);
  }
  return c;
}

/* ProcessMemoryPool.find_free_run: the probe nested in the window loop. */
__attribute__((noinline)) uint32_t find_free_run(const struct ZRegion *r, uint32_t pages, uint32_t from,
                                                 uint32_t limit)
{
  if (pages == 0)
    return (uint32_t)-1;
  uint32_t start = from;
  while (start + pages <= limit) {
    uint32_t off = pages;
    int blocked = 0;
    while (off > 0) {
      off--;
      if (is_set_val(r->bm.u, start + off)) {
        blocked = 1;
        break;
      }
    }
    if (!blocked)
      return start;
    start = start + off + 1;
  }
  return (uint32_t)-1;
}

static volatile uintptr_t bad_addr = 0x60000000u;
static volatile uint32_t zero = 0;

static uint32_t words[2] = {0x00000101u, 0x80000000u}; /* bits 0, 8, 63 */

int main(void)
{
  struct Region reg = {1, 2, 3, 4, 5, {64, words}};
  struct ZRegion zr = {0, 64, 4096, {0, 0, {64, words}}};
  const struct Region *bad = (const struct Region *)bad_addr;
  const struct ZRegion *zbad = (const struct ZRegion *)bad_addr;

  printf("probe %d %d %d\n", (int)probe(&reg, 0, 64), (int)probe(&reg, 1, 7), (int)probe(&reg, 0, zero));
  printf("probe_byval %d %d %d\n", (int)probe_byval(&zr, 0, 64), (int)probe_byval(&zr, 1, 7),
         (int)probe_byval(&zr, 0, zero));
  printf("count_set %u %u %u\n", count_set(&reg, 0, 64), count_set(&reg, 1, 7), count_set(&reg, 0, zero));
  printf("count_until %u %u %u\n", count_until(&reg, 64, 9), count_until(&reg, 64, 1), count_until(&reg, 64, 0));
  printf("find_free_run %d %d %d %d\n", (int)find_free_run(&zr, 3, 0, 64), (int)find_free_run(&zr, 8, 0, 64),
         (int)find_free_run(&zr, 60, 0, 64), (int)find_free_run(&zr, zero, 0, 64));

  /* Zero trips through a pointer that faults when read. */
  printf("bad probe %d\n", (int)probe(bad, 5, zero));
  printf("bad probe_byval %d\n", (int)probe_byval(zbad, 5, zero));
  printf("bad count_set %u\n", count_set(bad, 5, zero));
  printf("bad count_until %u\n", count_until(bad, 64, zero));
  printf("bad find_free_run %d %d\n", (int)find_free_run(zbad, 4, 10, 8 + zero),
         (int)find_free_run(zbad, zero, 0, 64));
  return 0;
}
