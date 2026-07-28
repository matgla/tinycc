/*
 *  tccyaff_stubs.c - minimal stub layer for the tccyaff/ unit-test binary
 *  (build_tccyaff/run_unit_tests_tccyaff)
 *
 *  Links the REAL tccyaff.c and tccelf.c from the tinycc source tree.
 *  Provides the memory allocator family, error/warning handlers, and any
 *  frontend/utility symbols the two modules need that are not available in
 *  libc or the test harness itself.
 *
 *  This TU deliberately does NOT include tcc.h: tcc.h redefines malloc/free/
 *  realloc to catch accidental raw frees, so the stub allocator uses the raw
 *  libc symbols directly.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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

unsigned tcc_test_free_count;

void tcc_free(void *ptr)
{
  if (ptr)
    tcc_test_free_count++;
  free(ptr);
}

char *tcc_strdup(const char *str)
{
  size_t n = strlen(str) + 1;
  char *p = (char *)tcc_malloc(n);
  memcpy(p, str, n);
  return p;
}

void libc_free(void *ptr)
{
  free(ptr);
}

/* Error/warning handling.  tcc.h maps tcc_error_noabort/tcc_error/tcc_warning
 * to the _tcc_* variants when USING_GLOBALS is active (the unit-test build
 * defines tcc_state as a global in tcc_state_stub.c). */
int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[test stub] _tcc_error_noabort: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return -1;
}

void _tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[test stub] _tcc_error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  abort();
}

void _tcc_warning(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[test stub] _tcc_warning: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

/* tcc_basename is referenced by tcc_load_yaff and tcc_output_yaff.
 * Minimal basename that leaves the input untouched (no allocation). */
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

/* tcc_enter_state is called by the tcc_error/tcc_warning macros in every
 * translation unit that does not #define USING_GLOBALS.  The unit-test build
 * provides a global tcc_state, so entering state is a no-op. */
struct TCCState;
void tcc_enter_state(struct TCCState *s1)
{
  (void)s1;
}

/* tcc_add_dllref is called by tcc_load_yaff after reading the library.
 * The YAFF loader does not need a real DLL registry; just record a dummy
 * reference so s1->nb_loaded_dlls grows if the caller inspects it. */
struct DLLReference;
struct TCCState;

struct DLLReference *tcc_add_dllref(struct TCCState *s1, const char *dllname, int level)
{
  (void)s1;
  (void)dllname;
  (void)level;
  return NULL;
}

/* -------------------------------------------------------------------------- */
/* dynarray helpers (real algorithm from libtcc.c) - needed by tccelf.c. */

void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
  int nb, nb_alloc;
  void **pp;

  nb = *nb_ptr;
  pp = *(void ***)ptab;
  if ((nb & (nb - 1)) == 0)
  {
    if (!nb)
      nb_alloc = 1;
    else
      nb_alloc = nb * 2;
    pp = tcc_realloc(pp, nb_alloc * sizeof(void *));
    *(void ***)ptab = pp;
  }
  pp[nb++] = data;
  *nb_ptr = nb;
}

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

/* -------------------------------------------------------------------------- */
/* Small utility helpers (real algorithms) - needed by tccelf.c / tccyaff.c. */

char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  char *q, ch;
  size_t len;

  q = buf;
  if (buf_size > 0)
  {
    for (len = buf_size - 1; len != 0; len--)
    {
      ch = *s++;
      if (ch == '\0')
        break;
      *q++ = ch;
    }
    *q = '\0';
  }
  return buf;
}

uint32_t read32le(unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void write32le(unsigned char *p, uint32_t x)
{
  p[0] = (unsigned char)x;
  p[1] = (unsigned char)(x >> 8);
  p[2] = (unsigned char)(x >> 16);
  p[3] = (unsigned char)(x >> 24);
}

void add32le(unsigned char *p, int32_t x)
{
  write32le(p, read32le(p) + x);
}
