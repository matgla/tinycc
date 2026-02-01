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

#define USING_GLOBALS
#include "tcc.h"
#include "tccopt.h"

#include <string.h>

/* ============================================================================
 * Global Statistics
 * ============================================================================ */

static TCCOptStats opt_stats = {0};

void tcc_opt_get_stats(TCCOptStats *stats)
{
  if (stats)
    memcpy(stats, &opt_stats, sizeof(*stats));
}

void tcc_opt_reset_stats(void)
{
  memset(&opt_stats, 0, sizeof(opt_stats));
}

/* ============================================================================
 * FP Offset Materialization Cache
 * ============================================================================
 * 
 * This cache tracks frame pointer offsets that have been computed into
 * physical registers. When the same offset is needed again, we can reuse
 * the register instead of recomputing the address.
 */

#define FP_MAT_CACHE_SIZE 8

void tcc_opt_fp_mat_cache_init(TCCIRState *ir)
{
  if (!ir)
    return;
    
  /* Allocate cache structure if needed */
  if (!ir->opt_fp_mat_cache) {
    ir->opt_fp_mat_cache = tcc_malloc(sizeof(TCCFPMatCache));
    memset(ir->opt_fp_mat_cache, 0, sizeof(TCCFPMatCache));
  }
  
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  
  /* Allocate initial entries */
  if (!cache->entries) {
    cache->capacity = FP_MAT_CACHE_SIZE;
    cache->entries = tcc_malloc(sizeof(TCCFPMatCacheEntry) * cache->capacity);
  }
  
  /* Clear all entries */
  for (int i = 0; i < cache->capacity; i++) {
    cache->entries[i].valid = 0;
  }
  cache->count = 0;
  cache->access_count = 0;
}

void tcc_opt_fp_mat_cache_clear(TCCIRState *ir)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;
    
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  
  for (int i = 0; i < cache->capacity; i++) {
    cache->entries[i].valid = 0;
  }
  cache->count = 0;
}

void tcc_opt_fp_mat_cache_free(TCCIRState *ir)
{
  if (!ir || !ir->opt_fp_mat_cache)
    return;
    
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  
  if (cache->entries) {
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
    
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  cache->access_count++;
  
  for (int i = 0; i < cache->capacity; i++) {
    if (cache->entries[i].valid && cache->entries[i].offset == offset) {
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
    
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  cache->access_count++;
  
  /* Check if already exists - update it */
  for (int i = 0; i < cache->capacity; i++) {
    if (cache->entries[i].valid && cache->entries[i].offset == offset) {
      cache->entries[i].phys_reg = phys_reg;
      cache->entries[i].last_use = cache->access_count;
      return;
    }
  }
  
  /* Find empty slot */
  int slot = -1;
  uint32_t oldest = cache->access_count;
  
  for (int i = 0; i < cache->capacity; i++) {
    if (!cache->entries[i].valid) {
      slot = i;
      break;
    }
    if (cache->entries[i].last_use < oldest) {
      oldest = cache->entries[i].last_use;
      slot = i;
    }
  }
  
  if (slot >= 0) {
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
    
  TCCFPMatCache *cache = (TCCFPMatCache*)ir->opt_fp_mat_cache;
  
  for (int i = 0; i < cache->capacity; i++) {
    if (cache->entries[i].valid && cache->entries[i].phys_reg == phys_reg) {
      cache->entries[i].valid = 0;
    }
  }
}

/* ============================================================================
 * Dead Code Elimination
 * ============================================================================ */

int tcc_opt_dead_code_elimination(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  int removed = 0;
  
  /* Simple DCE: remove instructions with no side effects whose
   * results are not used. This is a placeholder - full implementation
   * would require proper use-def analysis. */
  
  /* TODO: Implement full DCE using liveness information */
  
  opt_stats.dce_removed += removed;
  return removed;
}

/* ============================================================================
 * Constant Folding
 * ============================================================================ */

int tcc_opt_constant_folding(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  int folded = 0;
  
  /* TODO: Walk IR and fold constant operations
   * - Replace ADD(const, const) with single const
   * - Replace MUL(const, const) with single const
   * - etc.
   */
  
  opt_stats.const_folded += folded;
  return folded;
}

/* ============================================================================
 * Common Subexpression Elimination
 * ============================================================================ */

int tcc_opt_cse(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  int eliminated = 0;
  
  /* TODO: Implement CSE using value numbering or hashing */
  
  opt_stats.cse_eliminated += eliminated;
  return eliminated;
}

/* ============================================================================
 * Copy Propagation
 * ============================================================================ */

int tcc_opt_copy_propagation(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  int propagated = 0;
  
  /* TODO: Replace uses of copied variables with the source */
  
  opt_stats.copies_propagated += propagated;
  return propagated;
}

/* ============================================================================
 * Strength Reduction
 * ============================================================================ */

int tcc_opt_strength_reduction(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  int reduced = 0;
  
  /* TODO: Replace expensive operations with cheaper ones
   * - MUL by power of 2 -> SHL
   * - DIV by power of 2 -> SAR
   * - etc.
   */
  
  return reduced;
}

/* ============================================================================
 * FP Offset Caching Optimization Pass
 * ============================================================================ */

int tcc_opt_fp_offset_caching(TCCIRState *ir)
{
  if (!ir)
    return 0;
    
  /* Initialize cache if needed */
  tcc_opt_fp_mat_cache_init(ir);
  
  /* This pass doesn't transform the IR directly.
   * Instead, it sets up the cache that will be used
   * during code generation.
   */
  
  return 0;
}

/* ============================================================================
 * Optimization Pass Registry
 * ============================================================================ */

static TCCOptRegistry opt_registry = {0};

/* Built-in passes */
static TCCOptPass builtin_passes[] = {
  {
    .name = "fp-offset-cache",
    .description = "Frame pointer offset caching",
    .run = tcc_opt_fp_offset_caching,
    .flags = TCC_OPT_ENABLED_O1 | TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
  {
    .name = "dce",
    .description = "Dead code elimination",
    .run = tcc_opt_dead_code_elimination,
    .flags = TCC_OPT_ENABLED_O1 | TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
  {
    .name = "const-fold",
    .description = "Constant folding",
    .run = tcc_opt_constant_folding,
    .flags = TCC_OPT_ENABLED_O1 | TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
  {
    .name = "cse",
    .description = "Common subexpression elimination",
    .run = tcc_opt_cse,
    .flags = TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
  {
    .name = "copy-prop",
    .description = "Copy propagation",
    .run = tcc_opt_copy_propagation,
    .flags = TCC_OPT_ENABLED_O1 | TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
  {
    .name = "strength-reduce",
    .description = "Strength reduction",
    .run = tcc_opt_strength_reduction,
    .flags = TCC_OPT_ENABLED_O2 | TCC_OPT_ENABLED_OS,
    .should_run = NULL,
  },
};

void tcc_opt_register_pass(TCCOptPass *pass)
{
  if (!pass)
    return;
    
  if (!opt_registry.passes) {
    opt_registry.capacity = 16;
    opt_registry.passes = tcc_malloc(sizeof(TCCOptPass) * opt_registry.capacity);
  }
  
  if (opt_registry.count >= opt_registry.capacity) {
    opt_registry.capacity *= 2;
    opt_registry.passes = tcc_realloc(opt_registry.passes, 
                                       sizeof(TCCOptPass) * opt_registry.capacity);
  }
  
  opt_registry.passes[opt_registry.count++] = *pass;
}

const TCCOptPass* tcc_opt_get_passes(int *count)
{
  /* Initialize with built-in passes on first call */
  static int initialized = 0;
  if (!initialized) {
    int n = sizeof(builtin_passes) / sizeof(builtin_passes[0]);
    for (int i = 0; i < n; i++) {
      tcc_opt_register_pass(&builtin_passes[i]);
    }
    initialized = 1;
  }
  
  if (count)
    *count = opt_registry.count;
  return opt_registry.passes;
}

/* ============================================================================
 * Optimization Driver
 * ============================================================================ */

int tcc_opt_get_level(void)
{
  /* Get optimization level from TCCState */
  if (tcc_state) {
    /* Map TCC's optimization settings to our levels */
    if (tcc_state->opt_fp_offset_cache)
      return 1;
  }
  return 0;
}

void tcc_optimize_ir(TCCIRState *ir, int level)
{
  if (!ir || level <= 0)
    return;
    
  int pass_count;
  const TCCOptPass *passes = tcc_opt_get_passes(&pass_count);
  
  /* Determine which level flags apply */
  unsigned level_flags = 0;
  switch (level) {
    case 0: level_flags = TCC_OPT_ENABLED_O0; break;
    case 1: level_flags = TCC_OPT_ENABLED_O1; break;
    case 2: 
    case 3: level_flags = TCC_OPT_ENABLED_O2; break;
    default: level_flags = TCC_OPT_ENABLED_O1; break;
  }
  
  /* Run enabled passes */
  for (int i = 0; i < pass_count; i++) {
    if (passes[i].flags & level_flags) {
      if (!passes[i].should_run || passes[i].should_run(ir)) {
        passes[i].run(ir);
      }
    }
  }
}

int tcc_opt_run_pass(TCCIRState *ir, const char *name)
{
  if (!ir || !name)
    return 0;
    
  int pass_count;
  const TCCOptPass *passes = tcc_opt_get_passes(&pass_count);
  
  for (int i = 0; i < pass_count; i++) {
    if (strcmp(passes[i].name, name) == 0) {
      return passes[i].run(ir);
    }
  }
  
  return 0;
}
