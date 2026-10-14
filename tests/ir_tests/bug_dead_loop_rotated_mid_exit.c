/*
 *  TCC IR - dead_loop rotated arm vs a search loop's early exit
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:dead_loop's rotated arm killed the back-edge of a pure search loop
 * whose only escaping value is a constant per exit edge (`found = 1; break`,
 * or an early return), so the loop ran once and looked at items[0] only.
 * The arm now requires the latch's fall-through to be the loop's only exit. */
#include <stdio.h>
#include <stdlib.h>

typedef struct { void **items; int count; } Set;
typedef struct Summary { struct Summary *next; int live; Set reads; } Summary;

__attribute__((noinline)) static int read_anywhere(Summary *head, void *g)
{
  int found = 0;
  for (Summary *e = head; e && !found; e = e->next) {
    if (!e->live)
      continue;
    for (int i = 0; i < e->reads.count; i++) {
      if (e->reads.items[i] == g) {
        found = 1;
        break;
      }
    }
  }
  return found;
}

typedef struct { int src, idx; } Dep;
typedef struct { char pad[48]; Dep *deps; int count; int cap; } Deps;

__attribute__((noinline)) static void add_dep(Deps *s, int src, int idx)
{
  for (int i = 0; i < s->count; i++)
    if (s->deps[i].src == src && s->deps[i].idx == idx)
      return;
  if (s->count >= s->cap) {
    int nc = s->cap ? s->cap * 2 : 16;
    s->deps = realloc(s->deps, nc * sizeof(Dep));
    s->cap = nc;
  }
  s->deps[s->count++] = (Dep){src, idx};
}

__attribute__((noinline)) static int contains(Set *s, void *v)
{
  int found = 0;
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == v) {
      found = 1;
      break;
    }
  return found;
}

__attribute__((noinline)) static int index_of(Set *s, void *v)
{
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == v)
      return i;
  return -1;
}

__attribute__((noinline)) static int classify(Set *s, void *v)
{
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == v)
      goto hit;
  return -5;
hit:
  return 9;
}

/* A pure counting loop with nothing escaping: still fine to drop. */
__attribute__((noinline)) static int empty_count(int n)
{
  int k = 7;
  for (int i = 0; i < n; i++)
    k = 7;
  return k;
}

int main(void)
{
  int a, b, c, d;
  void *it0[3] = {&a, &b, &c};
  void *it1[2] = {&d, &a};
  Summary s1 = {0, 1, {it1, 2}};
  Summary s0 = {&s1, 1, {it0, 3}};
  printf("%d %d %d %d\n", read_anywhere(&s0, &a), read_anywhere(&s0, &c),
         read_anywhere(&s0, &d), read_anywhere(&s0, &s0));
  s1.live = 0;
  printf("%d %d\n", read_anywhere(&s0, &d), read_anywhere(&s0, &b));

  Deps dp = {{0}, 0, 0, 0};
  for (int k = 0; k < 3; k++) {
    add_dep(&dp, 1, 2);
    add_dep(&dp, 3, 4);
    add_dep(&dp, 1, 2);
    add_dep(&dp, 5, k);
  }
  printf("%d\n", dp.count);
  free(dp.deps);

  Set s = {it0, 3};
  printf("%d %d %d %d\n", contains(&s, &a), contains(&s, &b), contains(&s, &c), contains(&s, &d));
  printf("%d %d %d %d\n", index_of(&s, &a), index_of(&s, &b), index_of(&s, &c), index_of(&s, &d));
  printf("%d %d %d\n", classify(&s, &a), classify(&s, &c), classify(&s, &d));
  printf("%d %d\n", empty_count(10), empty_count(0));
  return 0;
}
