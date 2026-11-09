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
#include <fcntl.h>
#include <unistd.h>

signed char tcc_pass_timing_on = -1;

/* TCC_PASS_TIMING=1 is the env form of -bench for the per-pass table
 * (scripts/opt_profile.py drives it).  Compiled out of a release build, where
 * -bench (tcc_state->do_bench) remains the way in — so an on-device PASS_TIME
 * dump needs the flag, not the variable. */
TCC_DBG_ENV_FLAG(pass_timing_env_on, "TCC_PASS_TIMING")

#ifdef TCC_ALLOC_BACKTRACE
/* The tracing cross marks every pass in the allocation trace
 * (scripts/alloc_trace: what each pass holds); the timing it rides on stays
 * unprinted unless asked for. */
static int tcc_pt_trace_only;
#endif

/* TCC_MEMPEAK=1 on YasOS: which pass the process memory peaks in.  Reading
 * /proc/mempeak returns the pool's watermark since the last read and clears
 * it, so a read at every timed pass's start and end gives the peak of the
 * stretch since the previous one, charged to the innermost pass running over
 * it ("-" outside them).  Not a debug-build flag: the device compiler is a
 * release build.  Off where the file does not exist. */
static int tcc_mp_on;
static int tcc_mp_only; /* timing rides along for the hooks, unprinted */
#define TCC_MP_MAX 256
static struct {
  const char *name;
  unsigned long peak;
} *tcc_mp_tab;
static int tcc_mp_count;
static unsigned long tcc_mp_max;
static char tcc_mp_max_pass[48], tcc_mp_max_func[64];

static void tcc_mp_sample(const char *owner)
{
  char buf[160];
  int fd = open("/proc/mempeak", O_RDONLY), n;
  if (fd < 0)
  {
    tcc_mp_on = 0;
    return;
  }
  n = (int)read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  const char *p = strstr(buf, "process_peak_bytes ");
  if (!p)
    return;
  unsigned long v = strtoul(p + 19, NULL, 10);
  if (!owner)
    owner = "-";
  int i;
  for (i = 0; i < tcc_mp_count; i++)
    if (tcc_mp_tab[i].name == owner || !strcmp(tcc_mp_tab[i].name, owner))
      break;
  if (i == tcc_mp_count && tcc_mp_count < TCC_MP_MAX)
  {
    if (!tcc_mp_tab)
      tcc_mp_tab = tcc_mallocz(sizeof(*tcc_mp_tab) * TCC_MP_MAX);
    tcc_mp_tab[tcc_mp_count++].name = owner;
  }
  if (i < tcc_mp_count && v > tcc_mp_tab[i].peak)
    tcc_mp_tab[i].peak = v;
  if (v > tcc_mp_max)
  {
    tcc_mp_max = v;
    snprintf(tcc_mp_max_pass, sizeof tcc_mp_max_pass, "%s", owner);
    snprintf(tcc_mp_max_func, sizeof tcc_mp_max_func, "%s",
             tcc_state && tcc_state->cur_func_sym ? get_tok_str(tcc_state->cur_func_sym->v, NULL) : "-");
  }
}

void tcc_pass_timing_init(void)
{
  if (tcc_pass_timing_on < 0)
  {
    tcc_pass_timing_on =
        (pass_timing_env_on() || (tcc_state && tcc_state->do_bench)) ? 1 : 0;
    if (getenv("TCC_MEMPEAK"))
    {
      tcc_mp_on = 1;
      if (!tcc_pass_timing_on)
        tcc_pass_timing_on = 1, tcc_mp_only = 1;
    }
#ifdef TCC_ALLOC_BACKTRACE
    if (!tcc_pass_timing_on && getenv("TCC_ALLOCTRACE"))
      tcc_pass_timing_on = 1, tcc_pt_trace_only = 1;
#endif
  }
}

unsigned long tcc_pass_clk_us(void)
{
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0)
    return (unsigned long)tv.tv_sec * 1000000UL + (unsigned long)tv.tv_usec;
  return 0;
}

#define TCC_PASS_TIMING_MAX 256
/* Allocated on the first record, i.e. only when timing is on: as a static
 * table it was 6 KiB of .bss (12 KiB on a 64-bit host) that every compile
 * paid for, timed or not.  Kept for the life of the process, as the static
 * table was: with several files on one -c command line each dump reports the
 * running totals of every file so far. */
static struct TCCPassTime {
  const char *name;
  unsigned long self_us;      /* exclusive: minus time attributed to nested timed passes */
  unsigned long incl_us;      /* inclusive */
  unsigned long calls;
  unsigned long productive;   /* calls that reported changes > 0 */
  unsigned long unknown;      /* calls whose change count was not reported */
} *tcc_pt_tab;
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
    if (!tcc_pt_tab)
      tcc_pt_tab = tcc_mallocz(sizeof(*tcc_pt_tab) * TCC_PASS_TIMING_MAX);
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
  if (tcc_mp_on)
    tcc_mp_sample(tcc_pt_depth > 0 && tcc_pt_depth <= TCC_PT_STACK_MAX ? tcc_pt_stack[tcc_pt_depth - 1] : NULL);
  if (tcc_pt_depth < TCC_PT_STACK_MAX)
    tcc_pt_stack[tcc_pt_depth] = name;
  tcc_pt_depth++;
  t->active = 1;
  tcc_alloc_trace_pass(name, 1);
  t->saved_child_us = tcc_pt_child_us;
  tcc_pt_child_us = 0;
  t->start_us = tcc_pass_clk_us();
}

/* changes < 0 means "this pass does not report a change count". */
void tcc_pass_timing_end(TCCPassTimer *t, int changes)
{
  if (!t->active)
    return;
  if (tcc_mp_on)
    tcc_mp_sample(t->name);
  unsigned long incl = tcc_pass_clk_us() - t->start_us;
  unsigned long self = incl > tcc_pt_child_us ? incl - tcc_pt_child_us : 0;
  tcc_pt_child_us = t->saved_child_us + incl;
  if (tcc_pt_depth > 0)
    tcc_pt_depth--;
  t->active = 0;
  tcc_alloc_trace_pass(t->name, 0);
  tcc_pass_timing_record(t->name, self, incl, changes);
}

static void tcc_mp_dump(void)
{
  if (!tcc_mp_count)
    return;
  tcc_mp_sample(NULL); /* the stretch after the last pass */
  fprintf(stderr, "MEMPEAK max %lu in %s of %s\n", tcc_mp_max, tcc_mp_max_pass, tcc_mp_max_func);
  char used[TCC_MP_MAX] = {0};
  for (int k = 0; k < 15 && k < tcc_mp_count; k++)
  {
    int best = -1;
    for (int i = 0; i < tcc_mp_count; i++)
      if (!used[i] && (best < 0 || tcc_mp_tab[i].peak > tcc_mp_tab[best].peak))
        best = i;
    used[best] = 1;
    fprintf(stderr, "MEMPEAK %9lu  %s\n", tcc_mp_tab[best].peak, tcc_mp_tab[best].name);
  }
}

void tcc_pass_timing_dump(void)
{
  tcc_mp_dump();
  if (tcc_mp_only)
    return;
  /* tcc_pt_count is non-zero only when timing was enabled: no separate gate needed. */
  if (tcc_pt_count == 0)
    return;
#ifdef TCC_ALLOC_BACKTRACE
  if (tcc_pt_trace_only)
    return;
#endif
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
