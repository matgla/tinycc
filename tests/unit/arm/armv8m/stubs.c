/*
 *  stubs.c - libtcc memory stubs for unit tests (no tcc.h)
 *
 *  Unit tests link only the modules under test, not the full libtcc.
 *  This TU must NOT include tcc.h because tcc.h redefines malloc/realloc/free
 *  to guard helpers. We define the real tcc_malloc/realloc/free here using
 *  the raw libc symbols.
 *
 *  The tcc_state global lives in tcc_state_stub.c which does include tcc.h.
 */

#include <stdarg.h>
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

void tcc_free(void *ptr)
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

/* ───── Minimal stubs for thumb code paths ───── */

#include <stdio.h>
#include <stdlib.h>

void _tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[test stub] _tcc_error: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  abort();
}

void _tcc_warning(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[test stub] _tcc_warning: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/* `ind` is declared ST_DATA int rsym, anon_sym, ind, loc; in tcc.h.
 * In unit-test builds ST_DATA=extern, so we provide the definition. */
int ind;

/* set_elf_sym is declared in tcc.h; thumb.c uses it for symbol table entries.
 * Unit tests don't emit ELF, so return 0 (always succeeds). */
typedef unsigned long addr_t;
struct Section;

int set_elf_sym(struct Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  (void)s;
  (void)value;
  (void)size;
  (void)info;
  (void)other;
  (void)shndx;
  (void)name;
  return 0;
}

/* get_tok_str is declared `const char *get_tok_str(int, CValue*)` in tcc.h and
 * used by some opt passes only for diagnostic/symbol naming. Unit tests never
 * inspect the result, so return a constant. CValue is opaque here (no tcc.h),
 * hence the void* parameter — the linker resolves by name regardless. */
const char *get_tok_str(int v, void *cv)
{
  (void)v;
  (void)cv;
  return "?";
}

/* ───── Frontend link stubs pulled in by optimizer passes ─────
 *
 * opt_constfold.c/opt_utils.c reference the symbol-table helpers below.
 * They are unreachable at runtime for hand-built IR tests, but --gc-sections
 * keeps them reachable from pass entry points, so the linker needs a
 * definition.  Keep them opaque (no tcc.h) — pointer args/returns are enough.
 */
struct Sym;
struct CType;

struct Sym *global_stack = NULL;

struct Sym *sym_push2(struct Sym **ps, int v, int t, int c)
{
  (void)ps; (void)v; (void)t; (void)c;
  return NULL;
}

struct Sym *external_global_sym(int v, struct CType *type)
{
  (void)v; (void)type;
  return NULL;
}

int tok_alloc_const(const char *str)
{
  (void)str;
  return 0;
}

/* opt_dce.c (pulled in by the cmpfold suite) calls elfsym() on callee symbols.
 * Hand-built IR has no real ELF symbols, so return NULL. */
void *elfsym(void *s)
{
  (void)s;
  return 0;
}
