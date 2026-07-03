/*
 *  tccelf_stubs.c - minimal libtcc stubs for the tccelf/ unit-test binary
 *  (build_tccelf/run_unit_tests_tccelf)
 *
 *  The binary links the REAL tccelf.c, so this file must NOT define any
 *  symbol that tccelf.c provides itself.  It supplies only the small set
 *  of external helpers tccelf.c calls (memory, dynarray, error path,
 *  endian helpers, and a handful of pipeline entry points that are
 *  unreachable from the isolated helper tests).
 *
 *  This TU deliberately does NOT include tcc.h: tcc.h redefines malloc/
 *  realloc/free/strdup to guard helpers, and the unit-test stub layer
 *  needs the raw libc allocator symbols.
 */

#include <stdarg.h>
#include <stdint.h>
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

/* libc_free() is the escape hatch for buffers that came from libc. */
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

/* -------------------------------------------------------------------------- */
/* Error/warning stubs.  In the real compiler these enter/exit per-state
 * serialization; for unit tests the global tcc_state pointer is enough. */

void tcc_enter_state(void *s1)
{
  (void)s1;
}

void tcc_exit_state(void *s1)
{
  (void)s1;
}

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

/* -------------------------------------------------------------------------- */
/* dynarray helpers (real algorithm from libtcc.c). */

void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
  int nb, nb_alloc;
  void **pp;

  nb = *nb_ptr;
  pp = *(void ***)ptab;
  /* every power of two */
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
/* Small utility helpers (real algorithms). */

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

/* -------------------------------------------------------------------------- */
/* Pipeline entry points referenced by tccelf.c but unreachable from the
 * isolated helper tests below.  No-op or trivial-failure implementations
 * are sufficient. */

struct TCCState;
struct Section;
struct DLLReference;
struct Sym;
struct CString;

void tcc_debug_new(struct TCCState *s1)
{
  (void)s1;
}

void tcc_eh_frame_start(struct TCCState *s1)
{
  (void)s1;
}

int tcc_yaff_resolve(struct TCCState *s1, const char *name)
{
  (void)s1;
  (void)name;
  return 0;
}

int tcc_load_yaff(struct TCCState *s1, int fd, const char *filename, int level)
{
  (void)s1;
  (void)fd;
  (void)filename;
  (void)level;
  return -1;
}

void tcc_yaff_libs_free(struct TCCState *s1)
{
  (void)s1;
}

struct DLLReference *tcc_add_dllref(struct TCCState *s1, const char *dllname, int level)
{
  (void)s1;
  (void)dllname;
  (void)level;
  return NULL;
}

int tcc_assemble(struct TCCState *s1, int do_preprocess)
{
  (void)s1;
  (void)do_preprocess;
  return -1;
}

void arm_deinit(struct TCCState *s)
{
  (void)s;
}

void tcc_ir_free_switch_func_cache(struct TCCState *s)
{
  (void)s;
}

void ld_script_cleanup(void *ld)
{
  (void)ld;
}
