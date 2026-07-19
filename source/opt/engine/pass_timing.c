/*
 *  TCC IR - Per-pass timing instrumentation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"

signed char tcc_pass_timing_on = -1;

void tcc_pass_timing_init(void)
{
  if (tcc_pass_timing_on < 0)
    tcc_pass_timing_on =
        (getenv("TCC_PASS_TIMING") || (tcc_state && tcc_state->do_bench)) ? 1 : 0;
}

unsigned long tcc_pass_clk_us(void)
{
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0)
    return (unsigned long)tv.tv_sec * 1000000UL + (unsigned long)tv.tv_usec;
  return 0;
}

#define TCC_PASS_TIMING_MAX 96
static struct { const char *name; unsigned long us; unsigned long calls; } tcc_pt_tab[TCC_PASS_TIMING_MAX];
static int tcc_pt_count;

void tcc_pass_timing_add(const char *name, unsigned long us)
{
  for (int i = 0; i < tcc_pt_count; i++)
  {
    if (tcc_pt_tab[i].name == name || strcmp(tcc_pt_tab[i].name, name) == 0)
    {
      tcc_pt_tab[i].us += us;
      tcc_pt_tab[i].calls++;
      return;
    }
  }
  if (tcc_pt_count < TCC_PASS_TIMING_MAX)
  {
    int i = tcc_pt_count++;
    tcc_pt_tab[i].name = name;
    tcc_pt_tab[i].us = us;
    tcc_pt_tab[i].calls = 1;
  }
}

void tcc_pass_timing_dump(void)
{
  /* tcc_pt_count is non-zero only when timing was enabled: no separate gate needed. */
  if (tcc_pt_count == 0)
    return;
  unsigned long total = 0;
  for (int i = 0; i < tcc_pt_count; i++)
    total += tcc_pt_tab[i].us;
  if (total == 0)
    return;
  char used[TCC_PASS_TIMING_MAX] = {0};
  printf("=== TCC per-pass timing  total=%lu us ===\n", total);
  for (int s = 0; s < tcc_pt_count; s++)
  {
    int best = -1;
    for (int i = 0; i < tcc_pt_count; i++)
    {
      if (used[i])
        continue;
      if (best < 0 || tcc_pt_tab[i].us > tcc_pt_tab[best].us)
        best = i;
    }
    if (best < 0)
      break;
    used[best] = 1;
    printf("PASS_TIME %-26s %9lu us  %6lu calls  %3lu%%\n", tcc_pt_tab[best].name, tcc_pt_tab[best].us,
           tcc_pt_tab[best].calls, tcc_pt_tab[best].us * 100UL / total);
  }
  fflush(stdout);
}
