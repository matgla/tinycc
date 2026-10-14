/*
 *  TCC - Tiny C Compiler
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

#ifndef TCC_OPT_H
#define TCC_OPT_H

#include "tccir.h"

/* FP offset cache: reuse a register already holding a computed frame-pointer offset. */

typedef struct TCCFPMatCacheEntry {
  int valid;
  int offset;           /* Frame offset */
  int phys_reg;         /* Physical register holding the address */
  uint32_t last_use;    /* LRU timestamp */
} TCCFPMatCacheEntry;

typedef struct TCCFPMatCache {
  TCCFPMatCacheEntry *entries;
  int count;
  int capacity;
  uint32_t access_count;
} TCCFPMatCache;

void tcc_opt_fp_mat_cache_init(TCCIRState *ir);
void tcc_opt_fp_mat_cache_clear(TCCIRState *ir);
void tcc_opt_fp_mat_cache_free(TCCIRState *ir);

int tcc_opt_fp_mat_cache_lookup(TCCIRState *ir, int offset, int *phys_reg);
void tcc_opt_fp_mat_cache_record(TCCIRState *ir, int offset, int phys_reg);
void tcc_opt_fp_mat_cache_invalidate_reg(TCCIRState *ir, int phys_reg);

typedef struct TCCOptStats {
  int dce_removed;         /* Instructions removed by DCE */
  int const_folded;        /* Constants folded */
  int cse_eliminated;      /* CSE eliminations */
  int copies_propagated;   /* Copy propagations */
  int fp_cache_hits;       /* FP offset cache hits */
} TCCOptStats;

void tcc_opt_get_stats(TCCOptStats *stats);
void tcc_opt_reset_stats(void);

#endif /* TCC_OPT_H */
