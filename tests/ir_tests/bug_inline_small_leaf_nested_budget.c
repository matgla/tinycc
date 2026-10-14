/*
 *  TCC IR - small leaves under the nested-inline budget
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The nested-inline budget counted a small leaf by its tokens, so the 147-token
 * is_set stayed a call in try_extend once scan's other nested expansions had
 * spent the budget (the kernel's mremap page scan).  A small leaf's optimized
 * body is what is duplicated: it no longer counts (call.c). */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

struct bitset { uintptr_t bit_length; uintptr_t *masks; };
struct managed { void *alloc; const void *vtable; struct bitset bits; };

/* Inlinable by its token length, too many ops for a small leaf: every
 * nested expansion of it is charged to the caller's budget. */
static int work(int a, int b)
{
  int t = a * 3 + b, u = a ^ b, v = a - b;
  t = (t << 3) ^ (u >> 2);
  u = (u * 5) + (v >> 1);
  v = (v ^ t) + (u << 4);
  t = (t + u) * (v | 3);
  u = (u - t) ^ (v * 7);
  return t + u + v + (a >> 3);
}

/* bit_set.DynamicManaged.isSet as the Zig C backend writes it. */
static bool is_set(struct managed const a0, uintptr_t const a1)
{
  struct managed t0 = a0;
  struct managed const *t1 = &t0;
  struct bitset const *t2 = &t1->bits;
  struct bitset t3 = *t2;
  uintptr_t const *t4 = &t3.bit_length;
  if (!(a1 < *t4))
    __builtin_trap();
  uintptr_t *const *t5 = (uintptr_t *const *)&t3.masks;
  uintptr_t t6 = (*t5)[a1 >> 5];
  uintptr_t t7 = (uintptr_t)1u << (a1 & 31u);
  t6 = t6 & t7;
  return t6 != 0u;
}

#define W4(x) work(x, 1) + work(x, 2) + work(x, 3) + work(x, 4)
#define W16(x) W4(x) + W4(x + 1) + W4(x + 2) + W4(x + 3)

/* Called once: expanded into try_extend, every expansion inside it nested. */
static int scan(struct managed *m, uintptr_t start, uintptr_t end, int x)
{
  int r = W16(x) + W16(x + 4) + W16(x + 8);
  for (uintptr_t i = start; i < end; i++)
    if (is_set(*m, i))
      return -r;
  return r;
}

__attribute__((noinline)) int try_extend(struct managed *m, uintptr_t start, uintptr_t end, int x)
{
  return scan(m, start, end, x);
}

int main(void)
{
  uintptr_t masks[2] = {0x00000f00u, 0x1u};
  struct managed m = {0, 0, {64, masks}};
  printf("%d %d %d\n", try_extend(&m, 0, 8, 1), try_extend(&m, 12, 32, 2), try_extend(&m, 30, 33, 3));
  printf("%d\n", is_set(m, 32) + is_set(m, 9));
  return 0;
}
