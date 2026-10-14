/* Inlined Zig HashMap capacity(): the second `m ? hdr(m)->capacity : 0`
 * diamond on the same value reuses the first one's result (ssa:dup_diamond),
 * and `(hdr *)(m - 12)->capacity` loads `[m, #-4]` once SSA collapses the
 * pointer-cast local (arm_indexed_chain).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdint.h>
#include <stdio.h>

struct Map
{
  uint8_t *metadata;
  uint32_t size;
  uint32_t available;
};
struct Header
{
  void *values;
  uint16_t *keys;
  uint32_t capacity;
};

/* Spelled as Zig's C backend writes the accessors: by-value struct arguments
 * copied and their address taken. */
static struct Header *header(struct Map const a0)
{
  struct Map t0 = a0;
  return (struct Header *)((uintptr_t)t0.metadata - sizeof(struct Header));
}

static uint32_t capacity(struct Map const a0)
{
  struct Map t0 = a0;
  struct Map const *t1 = &t0;
  if (t1->metadata == NULL)
    return 0;
  struct Map t5 = *t1;
  struct Header *t6 = header(t5);
  return t6->capacity;
}

static uint16_t *keys(struct Map const a0)
{
  struct Map t0 = a0;
  struct Header *t6 = header(t0);
  return t6->keys;
}

/* getIndex's prefix: mask = capacity() - 1; limit = capacity(). */
__attribute__((noinline)) uint32_t two_caps(struct Map const a0, uint32_t hash)
{
  struct Map t0 = a0;
  struct Map const *t1 = &t0;
  struct Map t14 = *t1;
  uint32_t mask = capacity(t14) - 1;
  t14 = *t1;
  uint32_t limit = capacity(t14);
  return (hash & mask) + limit * 7;
}

/* The displacement half on its own. */
__attribute__((noinline)) uint32_t cap_of(uint8_t *m)
{
  struct Header *h = (struct Header *)((uintptr_t)m - sizeof(struct Header));
  return h->capacity;
}

/* Header fields at two negative displacements off one base. */
__attribute__((noinline)) uint32_t cap_plus_key(struct Map const a0, uint32_t i)
{
  if (a0.metadata == NULL)
    return 0;
  return capacity(a0) + keys(a0)[i];
}

/* Must stay two tests: the capacity changes between them. */
__attribute__((noinline)) uint32_t caps_around_store(struct Map const a0, uint32_t newcap)
{
  uint32_t before = capacity(a0);
  if (a0.metadata != NULL)
    header(a0)->capacity = newcap;
  uint32_t after = capacity(a0);
  return before * 1000 + after;
}

/* Must stay two tests: the second diamond reads another field. */
__attribute__((noinline)) uint32_t cap_then_key0(struct Map const a0)
{
  uint32_t c;
  if (a0.metadata == NULL)
    c = 0;
  else
    c = header(a0)->capacity;
  uint32_t mask = c - 1;
  uint32_t k;
  if (a0.metadata == NULL)
    k = 0;
  else
    k = *(uint32_t *)a0.metadata;
  return mask * 3 + k;
}

/* Must stay two tests: the second diamond tests another value. */
__attribute__((noinline)) uint32_t two_maps(struct Map const a, struct Map const b)
{
  uint32_t ca = capacity(a) - 1;
  uint32_t cb = capacity(b);
  return ca * 5 + cb;
}

/* The same diamond again, but the guard is inverted on the second test. */
__attribute__((noinline)) uint32_t two_caps_inverted(uint8_t *m, uint32_t hash)
{
  uint32_t c1, c2;
  if (m == NULL)
    c1 = 0;
  else
    c1 = ((uint32_t *)m)[-1];
  uint32_t mask = c1 - 1;
  if (m != NULL)
    c2 = ((uint32_t *)m)[-1];
  else
    c2 = 0;
  return (hash & mask) + c2 * 7;
}

static struct
{
  struct Header h;
  uint8_t meta[16];
} storage = {{0, 0, 16}, {1, 2, 3, 4}};

static uint16_t key_store[4] = {11, 22, 33, 44};

int main(void)
{
  struct Map full = {storage.meta, 3, 13};
  struct Map empty = {NULL, 0, 0};
  storage.h.keys = key_store;

  printf("two_caps full: %u\n", two_caps(full, 0x12345u));
  printf("two_caps empty: %u\n", two_caps(empty, 0x12345u));
  printf("cap_of: %u\n", cap_of(storage.meta));
  printf("cap_plus_key: %u %u\n", cap_plus_key(full, 2), cap_plus_key(empty, 2));
  printf("caps_around_store: %u\n", caps_around_store(full, 32));
  printf("cap now: %u\n", storage.h.capacity);
  printf("caps_around_store empty: %u\n", caps_around_store(empty, 64));
  printf("cap_then_key0: %u %u\n", cap_then_key0(full), cap_then_key0(empty));
  printf("two_maps: %u %u %u\n", two_maps(full, empty), two_maps(empty, full), two_maps(full, full));
  printf("two_caps_inverted: %u %u\n", two_caps_inverted(storage.meta, 0x777u),
         two_caps_inverted(NULL, 0x777u));
  return 0;
}
