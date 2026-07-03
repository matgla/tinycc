/*
 *  libtcc_api_stubs.c - link stubs for the libtcc-api/ binary
 *
 *  See libtcc_api_stubs.h. Two categories:
 *   - Frontend globals (tccpp.c, tccgen.c) read by libtcc.c's error1()
 *     and tcc_split_path() even on the A-bucket paths: plain zero-valued
 *     definitions.
 *   - Pipeline entry points (preprocess, tccgen, tccelf, tcc_load,
 *     tcc_assemble and friends): no-ops, never reached by an A-bucket test.
 *   - cstr helpers: REAL, verbatim-algorithm reimplementations of tccpp.c's
 *     CString helpers (not linked here) -- libtcc.c genuinely needs correct
 *     buffer growth for cmdline_defs, error messages, and -Wl suboption
 *     parsing; a no-op would silently corrupt tcc_define_symbol()'s output.
 */

#include "libtcc_api_stubs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- frontend globals ---- */

int tok_flags;
int pp_expr;
const int *macro_ptr;
struct BufferedFile *file;

/* ---- no-op pipeline stubs (never reached by an A-bucket test) ---- */

void preprocess_start(TCCState *s1, int filetype) { (void)s1; (void)filetype; }
void preprocess_end(TCCState *s1) { (void)s1; }
int tcc_preprocess(TCCState *s1) { (void)s1; return 0; }
void pp_error(CString *cs) { (void)cs; }

void tccgen_init(TCCState *s1) { (void)s1; }
int tccgen_compile(TCCState *s1) { (void)s1; return 0; }
void tccgen_finish(TCCState *s1) { (void)s1; }

void tccelf_new(TCCState *s) { (void)s; }
void tccelf_delete(TCCState *s) { (void)s; }
void tccelf_begin_file(TCCState *s1) { (void)s1; }
void tccelf_end_file(TCCState *s1) { (void)s1; }
void tccelf_add_crtbegin(TCCState *s1) { (void)s1; }

int tcc_object_type(int fd, ElfW(Ehdr) *h) { (void)fd; (void)h; return -1; }
int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset)
{
  (void)s1; (void)fd; (void)file_offset;
  return -1;
}
void tcc_free_lazy_objfiles(TCCState *s1) { (void)s1; }
int tcc_load_archive(TCCState *s1, int fd, int alacarte)
{
  (void)s1; (void)fd; (void)alacarte;
  return -1;
}
void tcc_archive_cache_free(TCCState *s1) { (void)s1; }
int tcc_load_dll(TCCState *s1, int fd, const char *filename, int level)
{
  (void)s1; (void)fd; (void)filename; (void)level;
  return -1;
}
int tcc_load_ldscript(TCCState *s1, int fd) { (void)s1; (void)fd; return -1; }
int tcc_load_yaff(TCCState *s1, int fd, const char *filename, int level)
{
  (void)s1; (void)fd; (void)filename; (void)level;
  return -1;
}
void tcc_yaff_libs_free(TCCState *s1) { (void)s1; }
int tcc_assemble(TCCState *s1, int do_preprocess) { (void)s1; (void)do_preprocess; return -1; }
void ld_script_cleanup(LDScript *ld) { (void)ld; }
void arm_deinit(struct TCCState *s) { (void)s; }
void tcc_ir_free_switch_func_cache(struct TCCState *s) { (void)s; }
void *load_data(int fd, unsigned long file_offset, unsigned long size)
{
  (void)fd; (void)file_offset; (void)size;
  return NULL;
}

/* ---- set_global_sym() call log ---- */

static int lapi_sgs_calls;
static char lapi_sgs_last_name[256];

int set_global_sym(TCCState *s1, const char *name, Section *sec, addr_t offs)
{
  (void)s1; (void)sec; (void)offs;
  lapi_sgs_calls++;
  if (name)
  {
    strncpy(lapi_sgs_last_name, name, sizeof(lapi_sgs_last_name) - 1);
    lapi_sgs_last_name[sizeof(lapi_sgs_last_name) - 1] = '\0';
  }
  else
  {
    lapi_sgs_last_name[0] = '\0';
  }
  return 0;
}

int lapi_set_global_sym_call_count(void) { return lapi_sgs_calls; }
const char *lapi_set_global_sym_last_name(void) { return lapi_sgs_last_name; }

void lapi_reset(void)
{
  lapi_sgs_calls = 0;
  lapi_sgs_last_name[0] = '\0';
}

/* ---- real CString helpers (verbatim algorithm from tccpp.c, not linked) ---- */

static void lapi_cstr_realloc(CString *cstr, int new_size)
{
  int size = cstr->size_allocated;
  if (size < 8)
    size = 8;
  while (size < new_size)
    size *= 2;
  cstr->data = tcc_realloc(cstr->data, size);
  cstr->size_allocated = size;
}

void cstr_ccat(CString *cstr, int ch)
{
  int size = cstr->size + 1;
  if (size > cstr->size_allocated)
    lapi_cstr_realloc(cstr, size);
  ((char *)cstr->data)[size - 1] = ch;
  cstr->size = size;
}

void cstr_cat(CString *cstr, const char *str, int len)
{
  int size;
  if (len <= 0)
    len = (int)strlen(str) + 1 + len;
  size = cstr->size + len;
  if (size > cstr->size_allocated)
    lapi_cstr_realloc(cstr, size);
  memmove((char *)cstr->data + cstr->size, str, len);
  cstr->size = size;
}

void cstr_new(CString *cstr)
{
  memset(cstr, 0, sizeof(CString));
}

void cstr_free(CString *cstr)
{
  tcc_free(cstr->data);
}

void cstr_reset(CString *cstr)
{
  cstr->size = 0;
}

int cstr_vprintf(CString *cstr, const char *fmt, va_list ap)
{
  va_list v;
  int len, size = 80;
  for (;;)
  {
    size += cstr->size;
    if (size > cstr->size_allocated)
      lapi_cstr_realloc(cstr, size);
    size = cstr->size_allocated - cstr->size;
    va_copy(v, ap);
    len = vsnprintf((char *)cstr->data + cstr->size, size, fmt, v);
    va_end(v);
    if (len >= 0 && len < size)
      break;
    size *= 2;
  }
  cstr->size += len;
  return len;
}

int cstr_printf(CString *cstr, const char *fmt, ...)
{
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = cstr_vprintf(cstr, fmt, ap);
  va_end(ap);
  return len;
}
