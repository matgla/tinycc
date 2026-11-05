/*
 *  TCC Memory Utilities - A fixed block of fast memory
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_MEMORY_HOT_ARENA_H
#define TCC_MEMORY_HOT_ARENA_H

#include <stddef.h>

/* Best-fit allocator over one caller-provided block: physical blocks with 8-byte
 * headers, coalesced on free.  A compile's few hot arrays need no more than that,
 * and going through it costs no syscall and no page clear.  Alignment is 8. */
typedef struct HotArena
{
  char *base, *end;
  size_t used, peak;
  unsigned misses; /* allocations that did not fit */
} HotArena;

void hot_arena_init(HotArena *a, void *memory, size_t bytes);
void *hot_arena_alloc(HotArena *a, size_t bytes);
void hot_arena_free(HotArena *a, void *p);
/* Grow or shrink in place; 0 when the block cannot grow where it is. */
int hot_arena_resize(HotArena *a, void *p, size_t bytes);
size_t hot_arena_block_size(const void *p);

static inline int hot_arena_contains(const HotArena *a, const void *p)
{
  return a->base && (const char *)p >= a->base && (const char *)p < a->end;
}

#endif
