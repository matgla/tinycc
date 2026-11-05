/*
 *  TCC Memory Utilities - A fixed block of fast memory
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/hot_arena.h"

typedef struct HotBlock
{
  unsigned size; /* payload bytes, a multiple of 8; bit 0 set while in use */
  unsigned prev; /* payload bytes of the block physically before; 0 for the first */
} HotBlock;

#define HB_SIZE(b) ((size_t)((b)->size & ~1u))
#define HB_USED(b) ((b)->size & 1u)
#define HB_NEXT(b) ((HotBlock *)((char *)((b) + 1) + HB_SIZE(b)))
#define HB_PREV(b) ((HotBlock *)((char *)(b) - (b)->prev - sizeof(HotBlock)))

static size_t hb_round(size_t bytes)
{
  return (bytes + 7) & ~(size_t)7;
}

static void hb_link_next(HotArena *a, HotBlock *b)
{
  HotBlock *n = HB_NEXT(b);
  if ((char *)n < a->end)
    n->prev = (unsigned)HB_SIZE(b);
}

/* Leave `size` bytes in b and make the rest a free block of its own, when there is
 * room for one. */
static void hb_split(HotArena *a, HotBlock *b, size_t size)
{
  const size_t total = HB_SIZE(b);
  if (total < size + sizeof(HotBlock) + 8)
    return;
  HotBlock *t = (HotBlock *)((char *)(b + 1) + size);
  t->size = (unsigned)(total - size - sizeof(HotBlock));
  t->prev = (unsigned)size;
  b->size = (unsigned)size | HB_USED(b);
  hb_link_next(a, t);
}

static void hb_merge_next(HotArena *a, HotBlock *b)
{
  HotBlock *n = HB_NEXT(b);
  if ((char *)n >= a->end || HB_USED(n))
    return;
  b->size = (unsigned)(HB_SIZE(b) + HB_SIZE(n) + sizeof(HotBlock)) | HB_USED(b);
  hb_link_next(a, b);
}

void hot_arena_init(HotArena *a, void *memory, size_t bytes)
{
  bytes &= ~(size_t)7;
  a->base = memory;
  a->end = a->base + bytes;
  a->used = a->peak = 0;
  a->misses = 0;
  HotBlock *b = memory;
  b->size = (unsigned)(bytes - sizeof(HotBlock));
  b->prev = 0;
}

void *hot_arena_alloc(HotArena *a, size_t bytes)
{
  if (!a->base || bytes > (size_t)(a->end - a->base))
    return NULL;
  const size_t n = hb_round(bytes ? bytes : 1);
  HotBlock *best = NULL;
  for (HotBlock *b = (HotBlock *)a->base; (char *)b < a->end; b = HB_NEXT(b))
    if (!HB_USED(b) && HB_SIZE(b) >= n && (!best || HB_SIZE(b) < HB_SIZE(best)))
      best = b;
  if (!best)
  {
    a->misses++;
    return NULL;
  }
  hb_split(a, best, n);
  best->size |= 1;
  a->used += HB_SIZE(best) + sizeof(HotBlock);
  if (a->used > a->peak)
    a->peak = a->used;
  return best + 1;
}

void hot_arena_free(HotArena *a, void *p)
{
  HotBlock *b = (HotBlock *)p - 1;
  a->used -= HB_SIZE(b) + sizeof(HotBlock);
  b->size &= ~1u;
  hb_merge_next(a, b);
  if (b->prev)
  {
    HotBlock *q = HB_PREV(b);
    if (!HB_USED(q))
      hb_merge_next(a, q);
  }
}

int hot_arena_resize(HotArena *a, void *p, size_t bytes)
{
  HotBlock *b = (HotBlock *)p - 1;
  const size_t n = hb_round(bytes ? bytes : 1), old = HB_SIZE(b);
  if (n > old)
  {
    HotBlock *next = HB_NEXT(b);
    if ((char *)next >= a->end || HB_USED(next) || old + HB_SIZE(next) + sizeof(HotBlock) < n)
      return 0;
    hb_merge_next(a, b);
  }
  hb_split(a, b, n);
  HotBlock *t = HB_NEXT(b);
  if ((char *)t < a->end && !HB_USED(t))
    hb_merge_next(a, t);
  a->used = a->used - old + HB_SIZE(b);
  if (a->used > a->peak)
    a->peak = a->used;
  return 1;
}

size_t hot_arena_block_size(const void *p)
{
  return HB_SIZE((const HotBlock *)p - 1);
}
