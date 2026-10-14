/*
 *  TCC Tests - ssa:dead_loop must not kill a rotated loop with a side exit
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* A dedup search `for (i < s->count) if (hit) return;` is pure, but its early
 * return leaves the loop by an edge other than the latch's fall-through.  Once
 * licm_field_ptr hoisted the s->count bound, try_kill_rotated_loop deleted the
 * back-edge and only element 0 was ever compared (self-hosted tcc's
 * sccp_add_mem_dep appended duplicates until "memory full").  The rotated arm
 * now refuses any exit edge but the latch's. */
#include <stdio.h>
#include <stdlib.h>

typedef struct { int src_pos, load_idx; } Dep;
typedef struct { char pad[48]; Dep *deps; int count; int cap; } S;

__attribute__((noinline)) static void add_dep(S *s, int src_pos, int load_idx)
{
  for (int i = 0; i < s->count; i++) {
    if (s->deps[i].src_pos == src_pos && s->deps[i].load_idx == load_idx)
      return;
  }
  if (s->count >= s->cap) {
    int nc = s->cap ? s->cap * 2 : 16;
    s->deps = realloc(s->deps, nc * sizeof(Dep));
    s->cap = nc;
  }
  s->deps[s->count++] = (Dep){ src_pos, load_idx };
}

typedef struct { int *items; int n; } Set;

/* Same shape with a `break` to a block other than the fall-through. */
__attribute__((noinline)) static int contains(Set *e, int v)
{
  int found = 0;
  for (int i = 0; i < e->n; i++) {
    if (e->items[i] == v) {
      found = 1;
      goto out;
    }
  }
  return 0;
out:
  return found + 10;
}

/* Pinned: a pure counting loop with no side exit still computes the same. */
__attribute__((noinline)) static int pure_count(Set *e)
{
  int r = 3;
  for (int i = 0; i < e->n; i++)
    r = 42;
  return r;
}

int main(void)
{
  S s = {{0}, 0, 0, 0};
  for (int k = 0; k < 3; k++) {
    add_dep(&s, 1, 2);
    add_dep(&s, 3, 4);
    add_dep(&s, 1, 2);
    add_dep(&s, 5, 6);
  }
  printf("count %d\n", s.count);
  for (int i = 0; i < s.count; i++)
    printf("dep %d %d\n", s.deps[i].src_pos, s.deps[i].load_idx);

  int items[5] = {7, 8, 9, 10, 11};
  Set e = {items, 5};
  printf("contains 7 %d\n", contains(&e, 7));
  printf("contains 11 %d\n", contains(&e, 11));
  printf("contains 9 %d\n", contains(&e, 9));
  printf("contains 12 %d\n", contains(&e, 12));
  e.n = 0;
  printf("contains empty %d\n", contains(&e, 7));
  printf("pure empty %d\n", pure_count(&e));
  e.n = 5;
  printf("pure full %d\n", pure_count(&e));
  free(s.deps);
  return 0;
}
