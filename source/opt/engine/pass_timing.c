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

#define TCC_PASS_TIMING_MAX 256
static struct {
  const char *name;
  unsigned long self_us;      /* exclusive: minus time attributed to nested timed passes */
  unsigned long incl_us;      /* inclusive */
  unsigned long calls;
  unsigned long productive;   /* calls that reported changes > 0 */
  unsigned long unknown;      /* calls whose change count was not reported */
} tcc_pt_tab[TCC_PASS_TIMING_MAX];
static int tcc_pt_count;
static int tcc_pt_overflow;

/* Frames of currently-running timed passes, innermost last.  A pass name already
 * on this stack is a re-entry of the same pass one layer down (the pipeline
 * driver times the table entry, the pass body times itself): the inner frame is
 * suppressed so µs and call counts are not double-counted. */
#define TCC_PT_STACK_MAX 32
static const char *tcc_pt_stack[TCC_PT_STACK_MAX];
static int tcc_pt_depth;
/* Inclusive time consumed by children of the innermost active frame. */
static unsigned long tcc_pt_child_us;

static void tcc_pass_timing_record(const char *name, unsigned long self_us,
                                   unsigned long incl_us, int changes)
{
  int i;
  for (i = 0; i < tcc_pt_count; i++)
    if (tcc_pt_tab[i].name == name || strcmp(tcc_pt_tab[i].name, name) == 0)
      break;
  if (i == tcc_pt_count) {
    if (tcc_pt_count >= TCC_PASS_TIMING_MAX) {
      tcc_pt_overflow++;
      return;
    }
    tcc_pt_count++;
    tcc_pt_tab[i].name = name;
  }
  tcc_pt_tab[i].self_us += self_us;
  tcc_pt_tab[i].incl_us += incl_us;
  tcc_pt_tab[i].calls++;
  if (changes < 0)
    tcc_pt_tab[i].unknown++;
  else if (changes > 0)
    tcc_pt_tab[i].productive++;
}

void tcc_pass_timing_begin(TCCPassTimer *t, const char *name)
{
  t->name = name;
  t->active = 0;
  t->start_us = 0;
  t->saved_child_us = 0;
  tcc_pass_timing_init();
  if (tcc_pass_timing_on <= 0 || !name)
    return;
  for (int i = 0; i < tcc_pt_depth && i < TCC_PT_STACK_MAX; i++)
    if (tcc_pt_stack[i] == name || strcmp(tcc_pt_stack[i], name) == 0)
      return; /* outer frame of the same pass already owns this time */
  if (tcc_pt_depth < TCC_PT_STACK_MAX)
    tcc_pt_stack[tcc_pt_depth] = name;
  tcc_pt_depth++;
  t->active = 1;
  t->saved_child_us = tcc_pt_child_us;
  tcc_pt_child_us = 0;
  t->start_us = tcc_pass_clk_us();
}

/* changes < 0 means "this pass does not report a change count". */
void tcc_pass_timing_end(TCCPassTimer *t, int changes)
{
  if (!t->active)
    return;
  unsigned long incl = tcc_pass_clk_us() - t->start_us;
  unsigned long self = incl > tcc_pt_child_us ? incl - tcc_pt_child_us : 0;
  tcc_pt_child_us = t->saved_child_us + incl;
  if (tcc_pt_depth > 0)
    tcc_pt_depth--;
  t->active = 0;
  tcc_pass_timing_record(t->name, self, incl, changes);
}

void tcc_pass_timing_dump(void)
{
  /* tcc_pt_count is non-zero only when timing was enabled: no separate gate needed. */
  if (tcc_pt_count == 0)
    return;
  unsigned long total = 0, total_calls = 0, total_prod = 0;
  for (int i = 0; i < tcc_pt_count; i++) {
    total += tcc_pt_tab[i].self_us;
    total_calls += tcc_pt_tab[i].calls;
    total_prod += tcc_pt_tab[i].productive;
  }
  if (total == 0)
    return;
  char used[TCC_PASS_TIMING_MAX] = {0};
  printf("=== TCC per-pass timing  total=%lu us  calls=%lu  productive=%lu (%lu%%) ===\n",
         total, total_calls, total_prod,
         total_calls ? total_prod * 100UL / total_calls : 0UL);
  printf("PASS_TIME %-26s %9s %9s %7s %7s\n", "name", "self_us", "incl_us", "calls", "prod");
  for (int s = 0; s < tcc_pt_count; s++)
  {
    int best = -1;
    for (int i = 0; i < tcc_pt_count; i++)
    {
      if (used[i])
        continue;
      if (best < 0 || tcc_pt_tab[i].self_us > tcc_pt_tab[best].self_us)
        best = i;
    }
    if (best < 0)
      break;
    used[best] = 1;
    /* A pass whose every call went unreported cannot be judged productive. */
    char prod[16];
    if (tcc_pt_tab[best].unknown == tcc_pt_tab[best].calls)
      snprintf(prod, sizeof(prod), "%7s", "?");
    else
      snprintf(prod, sizeof(prod), "%7lu", tcc_pt_tab[best].productive);
    printf("PASS_TIME %-26s %9lu %9lu %7lu %s  %3lu%%\n", tcc_pt_tab[best].name,
           tcc_pt_tab[best].self_us, tcc_pt_tab[best].incl_us,
           tcc_pt_tab[best].calls, prod, tcc_pt_tab[best].self_us * 100UL / total);
  }
  if (tcc_pt_overflow)
    printf("PASS_TIME <%d records dropped: TCC_PASS_TIMING_MAX exceeded>\n", tcc_pt_overflow);
  fflush(stdout);
}
