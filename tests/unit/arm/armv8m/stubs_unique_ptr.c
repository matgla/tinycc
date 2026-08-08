/*
 *  stubs_unique_ptr.c - minimal stubs for UT12 (unique_ptr + vector tests)
 *
 *  UT12 links only source/memory/{unique_ptr,vector}.c plus the test files.
 *  unique_ptr.c now calls tcc_free() (Phase 0) and vector.c calls tcc_mallocz/
 *  tcc_malloc via tcc.h. Provide the minimal allocator stubs so the linker
 *  resolves without dragging in the full stub layer.
 *
 *  Note: tcc_realloc and tcc_free are provided by test_vector.c (which uses
 *  custom allocators for testing), so we only define the remaining helpers.
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

/* tcc.h rewrites every tcc_malloc/tcc_mallocz call site into these
 * allocation-attribution wrappers, so those are the names vector.c and
 * unique_ptr.c actually reference.  (tcc_realloc_at goes with tcc_realloc in
 * test_vector.c, whose counting allocator the vector tests assert on.) */
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

char *tcc_strdup(const char *str)
{
  size_t n = strlen(str) + 1;
  char *p = (char *)tcc_malloc(n);
  memcpy(p, str, n);
  return p;
}

/* Intercept raw free() calls from tests that don't go through tcc_free.
 * The Makefile links with -Wl,--wrap=free, so any direct free() call in test
 * files becomes __wrap_free. Provide a passthrough so the linker resolves. */
extern void __real_free(void *ptr);

void __wrap_free(void *ptr)
{
  __real_free(ptr);
}
