/*
 *  TCC - FP offset materialization cache and optimizer stats
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#define USING_GLOBALS
#include "tcc.h"
#include "tccopt.h"

TCCOptStats opt_stats = {0};

void tcc_opt_get_stats(TCCOptStats *stats)
{
  if (stats)
    memcpy(stats, &opt_stats, sizeof(*stats));
}

void tcc_opt_reset_stats(void)
{
  memset(&opt_stats, 0, sizeof(opt_stats));
}

/* caches frame-pointer offsets already materialized into a phys reg, so the address is not recomputed */

#define FP_MAT_CACHE_SIZE 8

void tcc_opt_fp_mat_cache_init(TCCIRState *ir)
{
  if (!ir)
    return;

  if (!ir->opt_fp_mat_cache)
  {
    ir->opt_fp_mat_cache = tcc_malloc(sizeof(TCCFPMatCache));
    memset(ir->opt_fp_mat_cache, 0, sizeof(TCCFPMatCache));
  }

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;

  if (!cache->entries)
  {
    cache->capacity = FP_MAT_CACHE_SIZE;
    cache->entries = tcc_malloc(sizeof(TCCFPMatCacheEntry) * cache->capacity);
  }

  for (int i = 0; i < cache->capacity; i++)
  {
    cache->entries[i].valid = 0;
  }
  cache->count = 0;
  cache->access_count = 0;
}

void tcc_opt_fp_mat_cache_clear(TCCIRState *ir)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;

  for (int i = 0; i < cache->capacity; i++)
  {
    cache->entries[i].valid = 0;
  }
  cache->count = 0;
}

void tcc_opt_fp_mat_cache_free(TCCIRState *ir)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;

  if (cache->entries)
  {
    tcc_free(cache->entries);
    cache->entries = NULL;
  }
  cache->capacity = 0;
  cache->count = 0;

  tcc_free(ir->opt_fp_mat_cache);
  ir->opt_fp_mat_cache = NULL;
}

int tcc_opt_fp_mat_cache_lookup(TCCIRState *ir, int offset, int *phys_reg)
{
  if (!ir || !ir->opt_fp_mat_cache || !phys_reg)
    return 0;

  if (!tcc_state->opt_fp_offset_cache)
    return 0;

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;
  cache->access_count++;

  for (int i = 0; i < cache->capacity; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      *phys_reg = cache->entries[i].phys_reg;
      cache->entries[i].last_use = cache->access_count;
      opt_stats.fp_cache_hits++;

      return 1;
    }
  }

  return 0;
}

void tcc_opt_fp_mat_cache_record(TCCIRState *ir, int offset, int phys_reg)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;

  if (!tcc_state->opt_fp_offset_cache)
    return;

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;

  cache->access_count++;

  for (int i = 0; i < cache->capacity; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      cache->entries[i].phys_reg = phys_reg;
      cache->entries[i].last_use = cache->access_count;
      return;
    }
  }

  /* first free slot, else LRU victim */
  int slot = -1;
  uint32_t oldest = cache->access_count;

  for (int i = 0; i < cache->capacity; i++)
  {
    if (!cache->entries[i].valid)
    {
      slot = i;
      break;
    }
    if (cache->entries[i].last_use < oldest)
    {
      oldest = cache->entries[i].last_use;
      slot = i;
    }
  }

  if (slot >= 0)
  {
    cache->entries[slot].valid = 1;
    cache->entries[slot].offset = offset;
    cache->entries[slot].phys_reg = phys_reg;
    cache->entries[slot].last_use = cache->access_count;
    if (slot >= cache->count)
      cache->count = slot + 1;
  }
}

void tcc_opt_fp_mat_cache_invalidate_reg(TCCIRState *ir, int phys_reg)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;

  TCCFPMatCache *cache = (TCCFPMatCache *)ir->opt_fp_mat_cache;

  for (int i = 0; i < cache->capacity; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].phys_reg == phys_reg)
    {
      cache->entries[i].valid = 0;
    }
  }
}
