/*
 *  tccpp_stubs.c - minimal stub layer for the tccpp/ unit-test binary
 *  (build_tccpp/run_unit_tests_tccpp)
 *
 *  tccpp.c needs tcc_malloc/tcc_realloc/tcc_free, sym_push2 (called from
 *  define_push during tccpp_new), dynarray_reset (called from tccpp_delete),
 *  and the error/warning reporters.  This file supplies those without dragging
 *  in the rest of the compiler.
 *
 *  Compiled with USING_GLOBALS so the tcc.h macros leave _tcc_error/_tcc_warning
 *  alone and the real TCCState layout is visible.
 */

#define USING_GLOBALS
#include "tcc.h"

/* tcc.h redirects malloc/realloc/free/strdup to use_tcc_*.  Undo that here
   so this TU can call the raw libc allocators for its own implementations. */
#undef malloc
#undef realloc
#undef free
#undef strdup

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* define_stack is defined in tccgen.c, which we do not link. */
Sym *define_stack;

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

void tcc_free(void *ptr)
{
  free(ptr);
}

/* Minimal sym_push2: just enough for the define_push calls in tccpp_new. */
Sym *sym_push2(Sym **ps, int v, int t, int c)
{
  Sym *s = tcc_mallocz(sizeof(Sym));
  s->v = v;
  s->type.t = t;
  s->c = c;
  s->prev = *ps;
  *ps = s;
  return s;
}

/* Minimal dynarray_reset: only needs to free a NULL-terminated/empty array. */
void dynarray_reset(void *pp, int *n)
{
  void **p = *(void ***)pp;
  int i;
  for (i = 0; i < *n; i++)
    tcc_free(p[i]);
  tcc_free(p);
  *(void ***)pp = NULL;
  *n = 0;
}

/* tccpp.c calls sym_free from free_defines()/macro_arg_find(); tccgen.c owns
   the real implementation, which we do not link. */
void sym_free(Sym *sym)
{
  tcc_free(sym);
}

int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  return -1;
}

void _tcc_error(const char *fmt, ...)
{
  if (tcc_state && tcc_state->error_set_jmp_enabled)
    longjmp(tcc_state->error_jmp_buf, 1);

  {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
  exit(1);
}

void _tcc_warning(const char *fmt, ...)
{
  (void)fmt;
}
