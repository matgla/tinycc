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

/* tccpp.c's preprocess_start() references the target machine predefs blob that
 * normally lives in the backend (arm-thumb-gen.c).  Supply a minimal ARMv8-M
 * string so the lifecycle tests can call preprocess_start() without pulling in
 * the code generator. */
const char *const target_machine_defs =
    "__arm__\0"
    "__arm\0"
    "__ARM_ARCH_8M__\0";

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

/* -------------------------------------------------------------------------
 * Additional stubs needed once tests exercise next()/skip()/preprocess().
 * These are normally provided by libtcc.c/tccgen.c/tccdebug.c, which the
 * isolated tccpp binary intentionally does not link.
 * ------------------------------------------------------------------------- */

char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  char *q, *end;

  if (buf_size > 0)
  {
    q = buf;
    end = buf + buf_size - 1;
    while (*s != '\0' && q < end)
      *q++ = *s++;
    *q = '\0';
  }
  return buf;
}

char *pstrcat(char *buf, size_t buf_size, const char *s)
{
  size_t len;
  len = strlen(buf);
  if (len < buf_size)
    pstrcpy(buf + len, buf_size - len, s);
  return buf;
}

char *pstrncpy(char *out, const char *in, size_t num)
{
  memcpy(out, in, num);
  out[num] = '\0';
  return out;
}

char *tcc_strdup(const char *str)
{
  size_t n = strlen(str) + 1;
  char *p = tcc_malloc(n);
  memcpy(p, str, n);
  return p;
}

char *tcc_basename(const char *name)
{
  char *p = (char *)name + strlen(name);
  while (p > name && p[-1] != '/' && p[-1] != '\\')
    --p;
  return p;
}

void dynarray_add(void *ptab, int *nb_ptr, void *elem)
{
  void ***ptab_p = (void ***)ptab;
  int nb = *nb_ptr + 1;
  void **tab = tcc_realloc(*ptab_p, nb * sizeof(void *));
  tab[nb - 1] = elem;
  *ptab_p = tab;
  *nb_ptr = nb;
}

void tcc_debug_bincl(TCCState *s1) { (void)s1; }
void tcc_debug_eincl(TCCState *s1) { (void)s1; }
void tcc_debug_newfile(TCCState *s1) { (void)s1; }
int tcc_set_options(TCCState *s, const char *r) { (void)s; (void)r; return -1; }

int tcc_open(TCCState *s1, const char *filename)
{
  (void)s1;
  (void)filename;
  return -1;
}

/* Faithful push/pop of the BufferedFile stack, mirroring the real libtcc.c
   (which is not linked here).  Real semantics matter: helpers such as the
   _Pragma operator push a synthetic ":pragma:" buffer, parse it, then pop
   back to the original input, so tcc_open_bf must chain `prev' and tcc_close
   must restore it (fd is always -1 for these in-memory buffers). */
void tcc_open_bf(TCCState *s1, const char *filename, int initlen)
{
  BufferedFile *bf = tcc_mallocz(sizeof(BufferedFile) + initlen);
  pstrcpy(bf->filename, sizeof(bf->filename), filename);
  bf->true_filename = bf->filename;
  bf->buf_ptr = bf->buffer;
  bf->buf_end = bf->buffer + initlen;
  *bf->buf_end = CH_EOB;
  bf->fd = -1;
  bf->line_num = 1;
  bf->line_ref = 1;
  bf->ifdef_stack_ptr = s1->ifdef_stack_ptr;
  bf->prev = file;
  bf->prev_tok_flags = tok_flags;
  file = bf;
  tok_flags = TOK_FLAG_BOL | TOK_FLAG_BOF;
}

void tcc_close(void)
{
  BufferedFile *bf = file;
  if (bf == NULL)
    return;
  if (bf->true_filename != bf->filename)
    tcc_free(bf->true_filename);
  file = bf->prev;
  tok_flags = bf->prev_tok_flags;
  tcc_free(bf);
}

/* Expression evaluation is only reached by #if/#elif.  The real evaluator
   lives in tccgen.c and is not linked here.  Consume the preprocessed
   expression tokens so that expr_preprocess() sees TOK_EOF and returns a
   non-zero value, allowing the conditional-directive control flow to be
   exercised without crashing on the "..." error path. */
int64_t expr_const64(void)
{
  while (tok != TOK_EOF)
    next();
  return 1;
}

/* Symbol lookup used during macro expansion; not exercised by the tests. */
Sym *sym_find2(Sym *sym, int v)
{
  (void)sym;
  (void)v;
  return NULL;
}

int normalized_PATHCMP(const char *a, const char *b)
{
  return strcmp(a, b);
}
