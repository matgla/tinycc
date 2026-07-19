/*
 *  TCC - Optimization pass registry and driver
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

/* Defined in fp_mat_cache.c. */
extern TCCOptStats opt_stats;

int tcc_opt_dead_code_elimination(TCCIRState *ir)
{
  if (!ir)
    return 0;

  int removed = 0;

  /* TODO: Implement full DCE using liveness information */

  opt_stats.dce_removed += removed;
  return removed;
}

int tcc_opt_constant_folding(TCCIRState *ir)
{
  if (!ir)
    return 0;

  int folded = 0;

  /* TODO: Walk IR and fold constant operations */

  opt_stats.const_folded += folded;
  return folded;
}

int tcc_opt_cse(TCCIRState *ir)
{
  (void)ir;
  return 0;
}

int tcc_opt_fp_offset_caching(TCCIRState *ir)
{
  if (!ir)
    return 0;

  tcc_opt_fp_mat_cache_init(ir);

  /* Only primes the cache used during code generation; never edits the IR. */

  return 0;
}

static TCCOptRegistry opt_registry = {0};

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
    }
};

void tcc_opt_register_pass(TCCOptPass *pass)
{
  if (!pass)
    return;

  if (!opt_registry.passes)
  {
    opt_registry.capacity = 16;
    opt_registry.passes = tcc_malloc(sizeof(TCCOptPass) * opt_registry.capacity);
  }

  if (opt_registry.count >= opt_registry.capacity)
  {
    opt_registry.capacity *= 2;
    opt_registry.passes = tcc_realloc(opt_registry.passes, sizeof(TCCOptPass) * opt_registry.capacity);
  }

  opt_registry.passes[opt_registry.count++] = *pass;
}

const TCCOptPass *tcc_opt_get_passes(int *count)
{
  static int initialized = 0;
  if (!initialized)
  {
    int n = sizeof(builtin_passes) / sizeof(builtin_passes[0]);
    for (int i = 0; i < n; i++)
    {
      tcc_opt_register_pass(&builtin_passes[i]);
    }
    initialized = 1;
  }

  if (count)
    *count = opt_registry.count;
  return opt_registry.passes;
}

int tcc_opt_get_level(void)
{
  /* -O3 and above clamp to 2, matching tcc_optimize_ir()'s level->flag map. */
  if (tcc_state)
  {
    if (tcc_state->optimize >= 2)
      return 2;
    if (tcc_state->optimize >= 1)
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

  unsigned level_flags = 0;
  switch (level)
  {
  case 0:
    level_flags = TCC_OPT_ENABLED_O0;
    break;
  case 1:
    level_flags = TCC_OPT_ENABLED_O1;
    break;
  case 2:
  case 3:
    level_flags = TCC_OPT_ENABLED_O2;
    break;
  default:
    level_flags = TCC_OPT_ENABLED_O1;
    break;
  }

  for (int i = 0; i < pass_count; i++)
  {
    if (passes[i].flags & level_flags)
    {
      if (!passes[i].should_run || passes[i].should_run(ir))
      {
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

  for (int i = 0; i < pass_count; i++)
  {
    if (strcmp(passes[i].name, name) == 0)
    {
      return passes[i].run(ir);
    }
  }

  return 0;
}
