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

/* Target-independent IR passes: no architecture-specific assumptions (that is the backend's job). */

#include "tccir.h"

typedef enum TCCOptFlags {
  TCC_OPT_NONE = 0,
  TCC_OPT_ENABLED_O0 = (1u << 0),  /* Enabled at -O0 */
  TCC_OPT_ENABLED_O1 = (1u << 1),  /* Enabled at -O1 */
  TCC_OPT_ENABLED_O2 = (1u << 2),  /* Enabled at -O2 */
  TCC_OPT_ENABLED_OS = (1u << 3),  /* Enabled at -Os */
} TCCOptFlags;

typedef struct TCCOptPass {
  const char *name;            /* Pass name for debugging */
  const char *description;     /* Human-readable description */
  int (*run)(TCCIRState *ir);  /* Run function - returns number of changes */
  unsigned flags;              /* TCCOptFlags */
  int (*should_run)(TCCIRState *ir); /* Optional: check if pass should run */
} TCCOptPass;

typedef struct TCCOptRegistry {
  TCCOptPass *passes;
  int count;
  int capacity;
} TCCOptRegistry;

int tcc_opt_dead_code_elimination(TCCIRState *ir);

int tcc_opt_constant_folding(TCCIRState *ir);

int tcc_opt_cse(TCCIRState *ir);

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

int tcc_opt_fp_offset_caching(TCCIRState *ir);

/* Runs all passes enabled at the given level. */
void tcc_optimize_ir(TCCIRState *ir, int level);

int tcc_opt_run_pass(TCCIRState *ir, const char *name);

typedef struct TCCOptStats {
  int dce_removed;         /* Instructions removed by DCE */
  int const_folded;        /* Constants folded */
  int cse_eliminated;      /* CSE eliminations */
  int copies_propagated;   /* Copy propagations */
  int fp_cache_hits;       /* FP offset cache hits */
} TCCOptStats;

void tcc_opt_get_stats(TCCOptStats *stats);
void tcc_opt_reset_stats(void);

void tcc_opt_register_pass(TCCOptPass *pass);

const TCCOptPass* tcc_opt_get_passes(int *count);

static inline int tcc_opt_is_enabled(int level)
{
  return level > 0;
}

/* Optimization level of the current TCCState. */
int tcc_opt_get_level(void);

#endif /* TCC_OPT_H */
