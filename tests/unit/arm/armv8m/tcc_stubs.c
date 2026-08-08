/*
 *  tcc_stubs.c - minimal stub layer for the tcc/ unit-test binary
 *  (build_tcc/run_unit_tests_tcc)
 *
 *  The binary pulls in tcc.c (which itself #includes tcctools.c) directly.
 *  Only a handful of external symbols survive --gc-sections from the isolated
 *  helper tests; this file provides those.  It deliberately does NOT include
 *  tcc.h so the raw libc allocator symbols are available.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TCCState;

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

void libc_free(void *ptr)
{
  free(ptr);
}

char *tcc_strdup(const char *str)
{
  size_t n = strlen(str) + 1;
  char *p = (char *)tcc_malloc(n);
  memcpy(p, str, n);
  return p;
}

/* Minimal basename that leaves the final path component untouched. */
char *tcc_basename(const char *name)
{
  const char *p = name;
  if (!p)
    return (char *)"";
  const char *last = p;
  while (*p)
  {
    if (*p == '/' || *p == '\\')
      last = p + 1;
    p++;
  }
  return (char *)last;
}

/* Minimal extension splitter: returns pointer to last '.' in basename. */
char *tcc_fileextension(const char *name)
{
  const char *b = tcc_basename(name);
  const char *e = strrchr(b, '.');
  return (char *)(e ? e : b + strlen(b));
}

int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[tcc stub] _tcc_error_noabort: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return -1;
}

void _tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[tcc stub] _tcc_error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  abort();
}

void _tcc_warning(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[tcc stub] _tcc_warning: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

void tcc_enter_state(struct TCCState *s1)
{
  (void)s1;
}

/* Timing stub used by the -bench path; unreachable from the helper tests. */
unsigned int tcc_getclock_ms(void)
{
  return 0;
}

void tcc_print_stats(struct TCCState *s1, unsigned int dt)
{
  (void)s1;
  (void)dt;
}

void tcc_pass_timing_dump(void)
{
}

/* Startup-phase stamps, likewise part of the -bench reporting surface: tcc.c
 * records them unconditionally on the way through main(). */
void tcc_init_stamp(const char *label)
{
  (void)label;
}

void tcc_init_stamps_dump(void)
{
}
