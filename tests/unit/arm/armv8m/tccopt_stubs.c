/*
 *  tccopt_stubs.c - minimal libtcc memory stubs for the tccopt/ unit-test
 *  binary (build_tccopt/run_unit_tests_tccopt)
 *
 *  tccopt.c's only external dependencies (besides libc memcpy/memset/strcmp)
 *  are tcc_malloc/tcc_realloc/tcc_free and the global `tcc_state` (supplied
 *  separately by tcc_state_stub.c, reused verbatim from the main binary).
 *  This file does NOT define the tcc_opt_fp_mat_cache_... functions or
 *  tcc_opt_get_stats/etc -- those come from the REAL tccopt.c linked into
 *  this binary; redefining any of them here would be a multiple-definition
 *  link error.
 *
 *  Implementations copied verbatim from stubs.c (the main binary's stub
 *  layer) -- see that file for the "no tcc.h" rationale (tcc.h redefines
 *  malloc/realloc/free to guard helpers, so this TU uses the raw libc
 *  symbols directly).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *tcc_malloc(unsigned long size)
{
  void *p = malloc(size);
  if (!p && size)
  {
    fprintf(stderr, "tcc_malloc: out of memory\n");
    exit(1);
  }
  return p;
}

void *tcc_mallocz(unsigned long size)
{
  void *p = tcc_malloc(size);
  if (p)
    memset(p, 0, size);
  return p;
}

void *tcc_realloc(void *ptr, unsigned long size)
{
  void *p = realloc(ptr, size);
  if (!p && size)
  {
    fprintf(stderr, "tcc_realloc: out of memory\n");
    exit(1);
  }
  return p;
}

/* tcc.h rewrites every tcc_malloc/tcc_mallocz/tcc_realloc call site into these
 * allocation-attribution wrappers (they record the source line of mmap-class
 * allocations for -bench), so the stub layer has to answer to the wrapper
 * names too.  The accounting is of no interest here: just forward. */
void *tcc_malloc_at(unsigned long size, const char *file, int line)
{
  (void)file;
  (void)line;
  return tcc_malloc(size);
}

void *tcc_mallocz_at(unsigned long size, const char *file, int line)
{
  (void)file;
  (void)line;
  return tcc_mallocz(size);
}

void *tcc_realloc_at(void *ptr, unsigned long size, const char *file, int line)
{
  (void)file;
  (void)line;
  return tcc_realloc(ptr, size);
}

void tcc_free(void *ptr)
{
  free(ptr);
}
