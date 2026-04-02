/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tcc.h"
#include "tccld.h"

/********************************************************/
/* global variables */

/* XXX: get rid of this ASAP (or maybe not) */
ST_DATA struct TCCState *tcc_state;
TCC_SEM(static tcc_compile_sem);
/* an array of pointers to memory to be free'd after errors */
ST_DATA void **stk_data;
ST_DATA int nb_stk_data;

/********************************************************/

PUB_FUNC void tcc_enter_state(TCCState *s1)
{
  if (s1->error_set_jmp_enabled)
    return;
  WAIT_SEM(&tcc_compile_sem);
  tcc_state = s1;
}

PUB_FUNC void tcc_exit_state(TCCState *s1)
{
  if (s1->error_set_jmp_enabled)
    return;
  tcc_state = NULL;
  POST_SEM(&tcc_compile_sem);
}

/********************************************************/
/* copy a string and truncate it. */
ST_FUNC char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  char *q, *q_end;
  int c;

  if (buf_size > 0)
  {
    q = buf;
    q_end = buf + buf_size - 1;
    while (q < q_end)
    {
      c = *s++;
      if (c == '\0')
        break;
      *q++ = c;
    }
    *q = '\0';
  }
  return buf;
}

/* strcat and truncate. */
ST_FUNC char *pstrcat(char *buf, size_t buf_size, const char *s)
{
  size_t len;
  len = strlen(buf);
  if (len < buf_size)
    pstrcpy(buf + len, buf_size - len, s);
  return buf;
}

ST_FUNC char *pstrncpy(char *out, const char *in, size_t num)
{
  memcpy(out, in, num);
  out[num] = '\0';
  return out;
}

/* extract the basename of a file */
PUB_FUNC char *tcc_basename(const char *name)
{
  char *p = (char *)strchr(name, 0);
  while (p > name && !IS_DIRSEP(p[-1]))
    --p;
  return p;
}

/* extract extension part of a file
 *
 * (if no extension, return pointer to end-of-string)
 */
PUB_FUNC char *tcc_fileextension(const char *name)
{
  const char *b = tcc_basename(name);
  const char *e = strrchr(b, '.');
  return (char *)(e ? e : strchr(b, 0));
}

ST_FUNC char *tcc_load_text(int fd)
{
  int len = lseek(fd, 0, SEEK_END);
  char *buf = load_data(fd, 0, len + 1);
  if (buf)
    buf[len] = 0;
  return buf;
}

/********************************************************/
/* memory management */

/* we'll need the actual versions for a minute */
#undef free
#undef realloc

static void *default_reallocator(void *ptr, unsigned long size)
{
  void *ptr1;
  if (size == 0)
  {
    free(ptr);
    ptr1 = NULL;
  }
  else
  {
    ptr1 = realloc(ptr, size);
    if (!ptr1)
    {
      fprintf(stderr, "memory full\n");
      exit(1);
    }
  }
  return ptr1;
}

ST_FUNC void libc_free(void *ptr)
{
  free(ptr);
}

#define free(p) use_tcc_free(p)
#define realloc(p, s) use_tcc_realloc(p, s)

/* global so that every tcc_alloc()/tcc_free() call doesn't need to be changed
 */
static void *(*reallocator)(void *, unsigned long) = default_reallocator;

LIBTCCAPI void tcc_set_realloc(TCCReallocFunc *realloc)
{
  reallocator = realloc ? realloc : default_reallocator;
}

/* in case MEM_DEBUG is #defined */
#undef tcc_free
#undef tcc_malloc
#undef tcc_realloc
#undef tcc_mallocz
#undef tcc_strdup

PUB_FUNC void tcc_free(void *ptr)
{
  reallocator(ptr, 0);
}

PUB_FUNC void *tcc_malloc(unsigned long size)
{
  return reallocator(0, size);
}

PUB_FUNC void *tcc_realloc(void *ptr, unsigned long size)
{
  return reallocator(ptr, size);
}

PUB_FUNC void *tcc_mallocz(unsigned long size)
{
  void *ptr;
  ptr = tcc_malloc(size);
  if (size)
    memset(ptr, 0, size);
  return ptr;
}

PUB_FUNC char *tcc_strdup(const char *str)
{
  char *ptr;
  ptr = tcc_malloc(strlen(str) + 1);
  strcpy(ptr, str);
  return ptr;
}

#ifdef MEM_DEBUG

#define MEM_DEBUG_MAGIC1 0xFEEDDEB1
#define MEM_DEBUG_MAGIC2 0xFEEDDEB2
#define MEM_DEBUG_MAGIC3 0xFEEDDEB3
#define MEM_DEBUG_FILE_LEN 40
#define MEM_DEBUG_CHECK3(header) ((mem_debug_header_t *)((char *)header + header->size))->magic3
#define MEM_USER_PTR(header) ((char *)header + offsetof(mem_debug_header_t, magic3))
#define MEM_HEADER_PTR(ptr) (mem_debug_header_t *)((char *)ptr - offsetof(mem_debug_header_t, magic3))

struct mem_debug_header
{
  unsigned magic1;
  unsigned size;
  struct mem_debug_header *prev;
  struct mem_debug_header *next;
  int line_num;
  char file_name[MEM_DEBUG_FILE_LEN + 1];
  unsigned magic2;
  ALIGNED(16) unsigned char magic3[4];
};

typedef struct mem_debug_header mem_debug_header_t;

TCC_SEM(static mem_sem);
static mem_debug_header_t *mem_debug_chain;
static unsigned mem_cur_size;
static unsigned mem_max_size;
static int nb_states;

static mem_debug_header_t *malloc_check(void *ptr, const char *msg)
{
  mem_debug_header_t *header = MEM_HEADER_PTR(ptr);
  if (header->magic1 != MEM_DEBUG_MAGIC1 || header->magic2 != MEM_DEBUG_MAGIC2 ||
      read32le(MEM_DEBUG_CHECK3(header)) != MEM_DEBUG_MAGIC3 || header->size == (unsigned)-1)
  {
    fprintf(stderr, "%s check failed\n", msg);
    if (header->magic1 == MEM_DEBUG_MAGIC1)
      fprintf(stderr, "%s:%u: block allocated here.\n", header->file_name, header->line_num);
    exit(1);
  }
  return header;
}

PUB_FUNC void *tcc_malloc_debug(unsigned long size, const char *file, int line)
{
  int ofs;
  mem_debug_header_t *header;
  if (!size)
    return NULL;
  header = tcc_malloc(sizeof(mem_debug_header_t) + size);
  header->magic1 = MEM_DEBUG_MAGIC1;
  header->magic2 = MEM_DEBUG_MAGIC2;
  header->size = size;
  write32le(MEM_DEBUG_CHECK3(header), MEM_DEBUG_MAGIC3);
  header->line_num = line;
  ofs = strlen(file) - MEM_DEBUG_FILE_LEN;
  strncpy(header->file_name, file + (ofs > 0 ? ofs : 0), MEM_DEBUG_FILE_LEN);
  header->file_name[MEM_DEBUG_FILE_LEN] = 0;
  WAIT_SEM(&mem_sem);
  header->next = mem_debug_chain;
  header->prev = NULL;
  if (header->next)
    header->next->prev = header;
  mem_debug_chain = header;
  mem_cur_size += size;
  if (mem_cur_size > mem_max_size)
    mem_max_size = mem_cur_size;
  POST_SEM(&mem_sem);
  return MEM_USER_PTR(header);
}

PUB_FUNC void tcc_free_debug(void *ptr)
{
  mem_debug_header_t *header;
  if (!ptr)
    return;
  header = malloc_check(ptr, "tcc_free");
  WAIT_SEM(&mem_sem);
  mem_cur_size -= header->size;
  header->size = (unsigned)-1;
  if (header->next)
    header->next->prev = header->prev;
  if (header->prev)
    header->prev->next = header->next;
  if (header == mem_debug_chain)
    mem_debug_chain = header->next;
  POST_SEM(&mem_sem);
  tcc_free(header);
}

PUB_FUNC void *tcc_mallocz_debug(unsigned long size, const char *file, int line)
{
  void *ptr;
  ptr = tcc_malloc_debug(size, file, line);
  if (size)
    memset(ptr, 0, size);
  return ptr;
}

PUB_FUNC void *tcc_realloc_debug(void *ptr, unsigned long size, const char *file, int line)
{
  mem_debug_header_t *header;
  int mem_debug_chain_update = 0;

  if (!ptr)
    return tcc_malloc_debug(size, file, line);
  if (!size)
  {
    tcc_free_debug(ptr);
    return NULL;
  }
  header = malloc_check(ptr, "tcc_realloc");
  WAIT_SEM(&mem_sem);
  mem_cur_size -= header->size;
  mem_debug_chain_update = (header == mem_debug_chain);
  header = tcc_realloc(header, sizeof(mem_debug_header_t) + size);
  header->size = size;
  write32le(MEM_DEBUG_CHECK3(header), MEM_DEBUG_MAGIC3);
  if (header->next)
    header->next->prev = header;
  if (header->prev)
    header->prev->next = header;
  if (mem_debug_chain_update)
    mem_debug_chain = header;
  mem_cur_size += size;
  if (mem_cur_size > mem_max_size)
    mem_max_size = mem_cur_size;
  POST_SEM(&mem_sem);
  return MEM_USER_PTR(header);
}

PUB_FUNC char *tcc_strdup_debug(const char *str, const char *file, int line)
{
  char *ptr;
  ptr = tcc_malloc_debug(strlen(str) + 1, file, line);
  strcpy(ptr, str);
  return ptr;
}

PUB_FUNC void tcc_memcheck(int d)
{
  WAIT_SEM(&mem_sem);
  nb_states += d;
  if (0 == nb_states && mem_cur_size)
  {
    mem_debug_header_t *header = mem_debug_chain;
    fflush(stdout);
    fprintf(stderr, "MEM_DEBUG: mem_leak= %d bytes, mem_max_size= %d bytes\n", mem_cur_size, mem_max_size);
    while (header)
    {
      fprintf(stderr, "%s:%u: error: %u bytes leaked\n", header->file_name, header->line_num, header->size);
      header = header->next;
    }
    fflush(stderr);
    mem_cur_size = 0;
    mem_max_size = 0;
    mem_debug_chain = NULL;
#if MEM_DEBUG - 0 == 2
    exit(2);
#endif
  }
  POST_SEM(&mem_sem);
}

/* restore the debug versions */
#define tcc_free(ptr) tcc_free_debug(ptr)
#define tcc_malloc(size) tcc_malloc_debug(size, __FILE__, __LINE__)
#define tcc_mallocz(size) tcc_mallocz_debug(size, __FILE__, __LINE__)
#define tcc_realloc(ptr, size) tcc_realloc_debug(ptr, size, __FILE__, __LINE__)
#define tcc_strdup(str) tcc_strdup_debug(str, __FILE__, __LINE__)

#endif /* MEM_DEBUG */

/* for #pragma once */
ST_FUNC int normalized_PATHCMP(const char *f1, const char *f2)
{
  char *p1, *p2;
  int ret = 1;
  if (!!(p1 = realpath(f1, NULL)))
  {
    if (!!(p2 = realpath(f2, NULL)))
    {
      ret = PATHCMP(p1, p2);
      libc_free(p2); /* realpath() requirement */
    }
    libc_free(p1);
  }
  return ret;
}

/********************************************************/
/* dynarrays */

ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
  int nb, nb_alloc;
  void **pp;

  nb = *nb_ptr;
  pp = *(void ***)ptab;
  /* every power of two we double array size */
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

ST_FUNC void dynarray_reset(void *pp, int *n)
{
  void **p;
  for (p = *(void ***)pp; *n; ++p, --*n)
    if (*p)
      tcc_free(*p);
  tcc_free(*(void **)pp);
  *(void **)pp = NULL;
}

static void tcc_split_path(TCCState *s, void *p_ary, int *p_nb_ary, const char *in)
{
  const char *p;
  do
  {
    int c;
    CString str;

    cstr_new(&str);
    for (p = in; c = *p, c != '\0' && c != PATHSEP[0]; ++p)
    {
      if (c == '{' && p[1] && p[2] == '}')
      {
        c = p[1], p += 2;
        if (c == 'B')
          cstr_cat(&str, s->tcc_lib_path, -1);
        if (c == 'R')
          cstr_cat(&str, CONFIG_SYSROOT, -1);
        if (c == 'f' && file)
        {
          /* substitute current file's dir */
          const char *f = file->true_filename;
          const char *b = tcc_basename(f);
          if (b > f)
            cstr_cat(&str, f, b - f - 1);
          else
            cstr_cat(&str, ".", 1);
        }
      }
      else
      {
        cstr_ccat(&str, c);
      }
    }
    if (str.size)
    {
      cstr_ccat(&str, '\0');
      dynarray_add(p_ary, p_nb_ary, tcc_strdup(str.data));
    }
    cstr_free(&str);
    in = p + 1;
  } while (*p);
}

/********************************************************/
/* warning / error */

/* warn_... option bits */
#define WARN_ON 1  /* warning is on (-Woption) */
#define WARN_ERR 2 /* warning is an error (-Werror=option) */
#define WARN_NOE 4 /* warning is not an error (-Wno-error=option) */

/* error1() modes */
enum
{
  ERROR_WARN,
  ERROR_NOABORT,
  ERROR_ERROR
};

static void error1(int mode, const char *fmt, va_list ap)
{
  BufferedFile **pf, *f;
  TCCState *s1 = tcc_state;
  CString cs;
  int line = 0;

  tcc_exit_state(s1);

  if (mode == ERROR_WARN)
  {
    if (s1->warn_error)
      mode = ERROR_ERROR;
    if (s1->warn_num)
    {
      /* handle tcc_warning_c(warn_option)(fmt, ...) */
      int wopt = *(&s1->warn_none + s1->warn_num);
      s1->warn_num = 0;
      if (0 == (wopt & WARN_ON))
        return;
      if (wopt & WARN_ERR)
        mode = ERROR_ERROR;
      if (wopt & WARN_NOE)
        mode = ERROR_WARN;
    }
    if (s1->warn_none)
      return;
  }

  cstr_new(&cs);
  if (fmt[0] == '%' && fmt[1] == 'i' && fmt[2] == ':')
    line = va_arg(ap, int), fmt += 3;
  f = NULL;
  if (s1->error_set_jmp_enabled)
  { /* we're called while parsing a file */
    /* use upper file if inline ":asm:" or token ":paste:" */
    for (f = file; f && f->filename[0] == ':'; f = f->prev)
      ;
  }
  if (f)
  {
    for (pf = s1->include_stack; pf < s1->include_stack_ptr; pf++)
      cstr_printf(&cs, "In file included from %s:%d:\n", (*pf)->filename, (*pf)->line_num - 1);
    if (0 == line)
      line = f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
    cstr_printf(&cs, "%s:%d: ", f->filename, line);
  }
  else if (s1->current_filename)
  {
    cstr_printf(&cs, "%s: ", s1->current_filename);
  }
  else
  {
    cstr_printf(&cs, "tcc: ");
  }
  cstr_printf(&cs, mode == ERROR_WARN ? "warning: " : "error: ");
  if (pp_expr > 1)
    pp_error(&cs); /* special handler for preprocessor expression errors */
  else
    cstr_vprintf(&cs, fmt, ap);
  if (!s1->error_func)
  {
    /* default case: stderr */
    if (s1 && s1->output_type == TCC_OUTPUT_PREPROCESS && s1->ppfp == stdout)
      printf("\n"); /* print a newline during tcc -E */
    fflush(stdout); /* flush -v output */
    fprintf(stderr, "%s\n", (char *)cs.data);
    fflush(stderr); /* print error/warning now (win32) */
  }
  else
  {
    s1->error_func(s1->error_opaque, (char *)cs.data);
  }
  cstr_free(&cs);
  if (mode != ERROR_WARN)
    s1->nb_errors++;
  if (mode == ERROR_ERROR && s1->error_set_jmp_enabled)
  {
    while (nb_stk_data)
      tcc_free(*(void **)stk_data[--nb_stk_data]);
    longjmp(s1->error_jmp_buf, 1);
  }
}

LIBTCCAPI void tcc_set_error_func(TCCState *s, void *error_opaque, TCCErrorFunc *error_func)
{
  s->error_opaque = error_opaque;
  s->error_func = error_func;
}

/* error without aborting current compilation */
PUB_FUNC int _tcc_error_noabort(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_NOABORT, fmt, ap);
  va_end(ap);
  return -1;
}

#undef _tcc_error
PUB_FUNC void _tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_ERROR, fmt, ap);
  exit(1);
}
#define _tcc_error use_tcc_error_noabort

PUB_FUNC void _tcc_warning(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error1(ERROR_WARN, fmt, ap);
  va_end(ap);
}

/********************************************************/
/* I/O layer */

ST_FUNC void tcc_open_bf(TCCState *s1, const char *filename, int initlen)
{
  BufferedFile *bf;
  int buflen = initlen ? initlen : IO_BUF_SIZE;

  bf = tcc_mallocz(sizeof(BufferedFile) + buflen);
  bf->buf_ptr = bf->buffer;
  bf->buf_end = bf->buffer + initlen;
  bf->buf_end[0] = CH_EOB; /* put eob symbol */
  pstrcpy(bf->filename, sizeof(bf->filename), filename);
  bf->true_filename = bf->filename;
  bf->line_num = 1;
  bf->ifdef_stack_ptr = s1->ifdef_stack_ptr;
  bf->fd = -1;
  bf->prev = file;
  bf->prev_tok_flags = tok_flags;
  file = bf;
  tok_flags = TOK_FLAG_BOL | TOK_FLAG_BOF;
}

ST_FUNC void tcc_close(void)
{
  TCCState *s1 = tcc_state;
  BufferedFile *bf = file;
  if (bf->fd > 0)
  {
    close(bf->fd);
    total_lines += bf->line_num - 1;
  }
  if (bf->true_filename != bf->filename)
    tcc_free(bf->true_filename);
  file = bf->prev;
  tok_flags = bf->prev_tok_flags;
  tcc_free(bf);
}

static int _tcc_open(TCCState *s1, const char *filename)
{
  int fd;
  if (strcmp(filename, "-") == 0)
    fd = 0, filename = "<stdin>";
  else
    fd = open(filename, O_RDONLY | O_BINARY);
  if ((s1->verbose == 2 && fd >= 0) || s1->verbose == 3)
    printf("%s %*s%s\n", fd < 0 ? "nf" : "->", (int)(s1->include_stack_ptr - s1->include_stack), "", filename);
  return fd;
}

ST_FUNC int tcc_open(TCCState *s1, const char *filename)
{
  int fd = _tcc_open(s1, filename);
  if (fd < 0)
    return -1;
  tcc_open_bf(s1, filename, 0);
  file->fd = fd;
  return 0;
}

/* compile the file opened in 'file'. Return non zero if errors. */
static int tcc_compile(TCCState *s1, int filetype, const char *str, int fd)
{
  unsigned compile_start = 0;
  unsigned phase_start = 0;

  /* Here we enter the code section where we use the global variables for
     parsing and code generation (tccpp.c, tccgen.c, <target>-gen.c).
     Other threads need to wait until we're done.

     Alternatively we could use thread local storage for those global
     variables, which may or may not have advantages */

  if (s1->do_bench)
  {
    compile_start = tcc_getclock_ms();
    phase_start = compile_start;
  }

  tcc_enter_state(s1);
  s1->error_set_jmp_enabled = 1;

  if (setjmp(s1->error_jmp_buf) == 0)
  {
    s1->nb_errors = 0;

    if (fd == -1)
    {
      int len = strlen(str);
      tcc_open_bf(s1, "<string>", len);
      memcpy(file->buffer, str, len);
    }
    else
    {
      tcc_open_bf(s1, str, 0);
      file->fd = fd;
    }

    preprocess_start(s1, filetype);
    tccgen_init(s1);

    if (s1->output_type != TCC_OUTPUT_PREPROCESS && s1->output_type != TCC_OUTPUT_PCH)
      tccelf_begin_file(s1);

    if (s1->do_bench)
    {
      unsigned elapsed = tcc_getclock_ms() - phase_start;
      s1->bench_compile_setup_time += elapsed;
      s1->bench_compile_setup_count++;
      tcc_bench_log(s1, "compile-setup", str, elapsed);
      phase_start = tcc_getclock_ms();
    }

    if (s1->output_type == TCC_OUTPUT_PREPROCESS)
    {
      tcc_preprocess(s1);
    }
    else if (s1->output_type == TCC_OUTPUT_PCH)
    {
      tcc_pch_generate(s1, str, filetype);
    }
    else
    {
      if (filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP))
      {
        tcc_assemble(s1, !!(filetype & AFF_TYPE_ASMPP));
      }
      else
      {
        tccgen_compile(s1);
      }
    }

    if (s1->do_bench)
    {
      unsigned elapsed = tcc_getclock_ms() - phase_start;
      s1->bench_compile_exec_time += elapsed;
      s1->bench_compile_exec_count++;
      tcc_bench_log(s1, "compile-exec", str, elapsed);
      phase_start = tcc_getclock_ms();
    }

    if (s1->output_type != TCC_OUTPUT_PREPROCESS && s1->output_type != TCC_OUTPUT_PCH)
      tccelf_end_file(s1);
  }
  tccgen_finish(s1);
  preprocess_end(s1);
  s1->error_set_jmp_enabled = 0;
  tcc_exit_state(s1);
  if (s1->do_bench)
  {
    unsigned now = tcc_getclock_ms();
    unsigned finalize_elapsed = now - phase_start;
    unsigned elapsed = now - compile_start;
    s1->bench_compile_finalize_time += finalize_elapsed;
    s1->bench_compile_finalize_count++;
    tcc_bench_log(s1, "compile-finalize", str, finalize_elapsed);
    s1->bench_compile_time += elapsed;
    s1->bench_compile_count++;
    tcc_bench_log(s1, filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP) ? "assemble" : "compile", str, elapsed);
  }
  return s1->nb_errors != 0 ? -1 : 0;
}

LIBTCCAPI int tcc_compile_string(TCCState *s, const char *str)
{
  return tcc_compile(s, s->filetype, str, -1);
}

/* define a preprocessor symbol. value can be NULL, sym can be "sym=val" */
LIBTCCAPI void tcc_define_symbol(TCCState *s1, const char *sym, const char *value)
{
  const char *eq;
  if (NULL == (eq = strchr(sym, '=')))
    eq = strchr(sym, 0);
  if (NULL == value)
    value = *eq ? eq + 1 : "1";
  cstr_printf(&s1->cmdline_defs, "#define %.*s %s\n", (int)(eq - sym), sym, value);
}

/* undefine a preprocessor symbol */
LIBTCCAPI void tcc_undefine_symbol(TCCState *s1, const char *sym)
{
  cstr_printf(&s1->cmdline_defs, "#undef %s\n", sym);
}

LIBTCCAPI TCCState *tcc_new(void)
{
  TCCState *s;

  s = tcc_mallocz(sizeof(TCCState));
#ifdef MEM_DEBUG
  tcc_memcheck(1);
#endif

#undef gnu_ext
  s->gnu_ext = 1;
  s->tcc_ext = 1;
  s->nocommon = 1;
  s->dollars_in_identifiers = 1; /*on by default like in gcc/clang*/
  s->cversion = 201112;          /* default to C11 */
  s->warn_implicit_function_declaration = 1;
  s->warn_discarded_qualifiers = 1;
  s->ms_extensions = 1;
  s->unwind_tables = 1;

#ifdef CHAR_IS_UNSIGNED
  s->char_is_unsigned = 1;
#endif
  s->pic = 0;
  s->no_pie = 0;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  s->float_abi = ARM_SOFTFP_FLOAT;
  s->fpu_type = ARM_FPU_AUTO; /* default to auto-detect */
#if defined(TCC_TARGET_YASOS)
  s->text_and_data_separation = 1;
  s->pic = 1;
  s->section_align = 4;
  s->text_addr = 0;
  s->has_text_addr = 1;
#else
  s->text_and_data_separation = 0;
#endif
#endif
#ifdef CONFIG_NEW_DTAGS
  s->enable_new_dtags = 1;
#endif
  s->ppfp = stdout;
  /* might be used in error() before preprocess_start() */
  s->include_stack_ptr = s->include_stack;
  s->pch_auto_enabled = 1;

  tcc_set_lib_path(s, CONFIG_TCCDIR);
  return s;
}

LIBTCCAPI void tcc_delete(TCCState *s1)
{
  /* free target-specific backend state */
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  arm_deinit(s1);
#endif

  /* free lazy object files (Phase 2 GC) */
  tcc_free_lazy_objfiles(s1);

  /* free cached archive symbol tables */
  tcc_archive_cache_free(s1);

  /* free sections */
  tccelf_delete(s1);

  /* free library paths */
  dynarray_reset(&s1->library_paths, &s1->nb_library_paths);
  dynarray_reset(&s1->crt_paths, &s1->nb_crt_paths);

  /* free include paths */
  dynarray_reset(&s1->include_paths, &s1->nb_include_paths);
  dynarray_reset(&s1->sysinclude_paths, &s1->nb_sysinclude_paths);
  tcc_pch_auto_reset(s1);

  tcc_free(s1->tcc_lib_path);
  tcc_free(s1->soname);
  tcc_free(s1->rpath);
  tcc_free(s1->elf_entryname);
  tcc_free(s1->init_symbol);
  tcc_free(s1->fini_symbol);
  tcc_free(s1->mapfile);
  tcc_free(s1->outfile);
  tcc_free(s1->deps_outfile);
  tcc_free(s1->pch_infile);
  tcc_free(s1->linker_script);
  tcc_pch_free(s1);
  if (s1->ld_script)
  {
    ld_script_cleanup(s1->ld_script);
    tcc_free(s1->ld_script);
  }
  dynarray_reset(&s1->files, &s1->nb_files);
  dynarray_reset(&s1->target_deps, &s1->nb_target_deps);
  dynarray_reset(&s1->pragma_libs, &s1->nb_pragma_libs);
  dynarray_reset(&s1->argv, &s1->argc);
  cstr_free(&s1->cmdline_defs);
  cstr_free(&s1->cmdline_incl);
  cstr_free(&s1->linker_arg);
  tcc_free(s1->dState);
  /* free loaded dlls array */
  dynarray_reset(&s1->loaded_dlls, &s1->nb_loaded_dlls);
  tcc_free(s1);
#ifdef MEM_DEBUG
  tcc_memcheck(-1);
#endif
}

LIBTCCAPI int tcc_set_output_type(TCCState *s, int output_type)
{
#if defined(CONFIG_TCC_PIE)
  if (output_type == TCC_OUTPUT_EXE)
  {
#if defined(TCC_TARGET_ARM_THUMB)
    /* Disable PIE for ARM Thumb targets - the YAFF format handles
       data relocations directly from R_ARM_ABS32 entries without
       needing the full DYN infrastructure (which breaks GOT filling
       for local symbols) */
#elif defined(s)
    if (s->no_pie)
    {
      /* Explicitly disabled via -no-pie */
    }
    else
    {
      output_type |= TCC_OUTPUT_DYN;
    }
#else
    output_type |= TCC_OUTPUT_DYN;
#endif
  }
#endif
  s->output_type = output_type;

  if (!s->nostdinc)
  {
    /* default include paths */
    /* -isystem paths have already been handled */
    tcc_add_sysinclude_path(s, CONFIG_TCC_SYSINCLUDEPATHS);
  }

  if (output_type == TCC_OUTPUT_PREPROCESS || output_type == TCC_OUTPUT_PCH)
  {
    s->do_debug = 0;
    return 0;
  }

  /* add sections */
  tccelf_new(s);

  if (output_type == TCC_OUTPUT_OBJ)
  {
    /* always elf for objects */
    s->output_format = TCC_OUTPUT_FORMAT_ELF;
    return 0;
  }

  tcc_add_library_path(s, CONFIG_TCC_LIBPATHS);

  /* paths for crt objects */
  tcc_split_path(s, &s->crt_paths, &s->nb_crt_paths, CONFIG_TCC_CRTPREFIX);
  if (output_type != TCC_OUTPUT_MEMORY && !s->nodefaultlibs && !s->nostdlib)
    tccelf_add_crtbegin(s);
  return 0;
}

LIBTCCAPI int tcc_add_include_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->include_paths, &s->nb_include_paths, pathname);
  return 0;
}

LIBTCCAPI int tcc_add_sysinclude_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->sysinclude_paths, &s->nb_sysinclude_paths, pathname);
  return 0;
}

/* add/update a 'DLLReference', Just find if level == -1  */
ST_FUNC DLLReference *tcc_add_dllref(TCCState *s1, const char *dllname, int level)
{
  DLLReference *ref = NULL;
  int i;
  for (i = 0; i < s1->nb_loaded_dlls; i++)
    if (0 == strcmp(s1->loaded_dlls[i]->name, dllname))
    {
      ref = s1->loaded_dlls[i];
      break;
    }
  if (level == -1)
    return ref;
  if (ref)
  {
    if (level < ref->level)
      ref->level = level;
    ref->found = 1;
    return ref;
  }
  ref = tcc_mallocz(sizeof(DLLReference) + strlen(dllname));
  strcpy(ref->name, dllname);
  dynarray_add(&s1->loaded_dlls, &s1->nb_loaded_dlls, ref);
  ref->level = level;
  ref->index = s1->nb_loaded_dlls;
  return ref;
}

static int guess_filetype(const char *filename);

ST_FUNC int tcc_add_file_internal(TCCState *s1, const char *filename, int flags)
{
  int fd, ret = -1;
  unsigned open_start = 0;
  unsigned elapsed = 0;

  if (0 == (flags & AFF_TYPE_MASK))
    flags |= guess_filetype(filename);

  /* ignore binary files with -E */
  if (s1->output_type == TCC_OUTPUT_PREPROCESS && (flags & AFF_TYPE_BIN))
    return 0;

  /* open the file */
  if (s1->do_bench)
    open_start = tcc_getclock_ms();
  fd = _tcc_open(s1, filename);
  if (s1->do_bench)
  {
    elapsed = tcc_getclock_ms() - open_start;
    s1->bench_file_open_time += elapsed;
    s1->bench_file_open_count++;
    tcc_bench_log(s1, "open", filename, elapsed);
  }
  if (fd < 0)
  {
    if (flags & AFF_PRINT_ERROR)
      tcc_error_noabort("file '%s' not found", filename);
    return FILE_NOT_FOUND;
  }

  s1->current_filename = filename;
  s1->current_archive_offset = 0;  /* Reset archive offset for regular files */
  s1->current_archive_path = NULL; /* Reset archive path for regular files */
  if (flags & AFF_TYPE_BIN)
  {
    ElfW(Ehdr) ehdr;
    int obj_type;

    obj_type = tcc_object_type(fd, &ehdr);
    lseek(fd, 0, SEEK_SET);

    switch (obj_type)
    {
    case AFF_BINTYPE_REL:
      ret = tcc_load_object_file(s1, fd, 0);
      break;

    case AFF_BINTYPE_AR:
      ret = tcc_load_archive(s1, fd, !(flags & AFF_WHOLE_ARCHIVE));
      break;
    case AFF_BINTYPE_YAFF:
      ret = tcc_load_yaff(s1, fd, filename, (flags & AFF_REFERENCED_DLL) != 0);
      break;

    case AFF_BINTYPE_DYN:
      if (s1->output_type == TCC_OUTPUT_MEMORY)
      {
      }
      else
      {
        ret = tcc_load_dll(s1, fd, filename, (flags & AFF_REFERENCED_DLL) != 0);
      }
      break;

    default:
      /* as GNU ld, consider it is an ld script if not recognized */
      ret = tcc_load_ldscript(s1, fd);
      goto check_success;

    check_success:
      if (ret < 0)
        tcc_error_noabort("%s: unrecognized file type", filename);
      break;
    }
    close(fd);
  }
  else
  {
    /* update target deps */
    if (s1->output_type != TCC_OUTPUT_PCH)
      dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(filename));
    ret = tcc_compile(s1, flags, filename, fd);
  }
  s1->current_filename = NULL;
  return ret;
}

static int guess_filetype(const char *filename)
{
  int filetype = 0;
  if (1)
  {
    /* use a file extension to detect a filetype */
    const char *ext = tcc_fileextension(filename);
    if (ext[0])
    {
      ext++;
      if (!strcmp(ext, "S"))
        filetype = AFF_TYPE_ASMPP;
      else if (!strcmp(ext, "s"))
        filetype = AFF_TYPE_ASM;
      else if (!PATHCMP(ext, "c") || !PATHCMP(ext, "h") || !PATHCMP(ext, "i"))
        filetype = AFF_TYPE_C;
      else
        filetype |= AFF_TYPE_BIN;
    }
    else
    {
      filetype = AFF_TYPE_C;
    }
  }
  return filetype;
}

LIBTCCAPI int tcc_add_file(TCCState *s, const char *filename)
{
  return tcc_add_file_internal(s, filename, s->filetype | AFF_PRINT_ERROR);
}

LIBTCCAPI int tcc_add_library_path(TCCState *s, const char *pathname)
{
  tcc_split_path(s, &s->library_paths, &s->nb_library_paths, pathname);
  return 0;
}

static int tcc_add_library_internal(TCCState *s1, const char *fmt, const char *filename, int flags, char **paths,
                                    int nb_paths)
{
  char buf[1024];
  int i, ret;
  unsigned resolve_start = 0;

  if (s1->do_bench)
    resolve_start = tcc_getclock_ms();

  for (i = 0; i < nb_paths; i++)
  {
    snprintf(buf, sizeof(buf), fmt, paths[i], filename);
    ret = tcc_add_file_internal(s1, buf, flags & ~AFF_PRINT_ERROR);
    if (ret != FILE_NOT_FOUND)
    {
      if (s1->do_bench)
      {
        unsigned elapsed = tcc_getclock_ms() - resolve_start;
        s1->bench_library_resolve_time += elapsed;
        s1->bench_library_resolve_count++;
        tcc_bench_log(s1, "resolve-lib", buf, elapsed);
      }
      return ret;
    }
  }
  if (s1->do_bench)
  {
    unsigned elapsed = tcc_getclock_ms() - resolve_start;
    s1->bench_library_resolve_time += elapsed;
    s1->bench_library_resolve_count++;
    tcc_bench_log(s1, "resolve-lib", filename, elapsed);
  }
  if (flags & AFF_PRINT_ERROR)
    tcc_error_noabort("library '%s' not found", filename);
  return FILE_NOT_FOUND;
}

/* find and load a dll. Return non zero if not found */
ST_FUNC int tcc_add_dll(TCCState *s, const char *filename, int flags)
{
  return tcc_add_library_internal(s, "%s/%s", filename, flags, s->library_paths, s->nb_library_paths);
}

/* find [cross-]libtcc1.a and tcc helper objects in library path */
ST_FUNC int tcc_add_support(TCCState *s1, const char *filename)
{
  char buf[100];
  if (CONFIG_TCC_CROSSPREFIX[0])
    filename = strcat(strcpy(buf, CONFIG_TCC_CROSSPREFIX), filename);
  return tcc_add_dll(s1, filename, AFF_PRINT_ERROR);
}

#if !defined TCC_TARGET_PE && !defined TCC_TARGET_MACHO
ST_FUNC int tcc_add_crt(TCCState *s1, const char *filename)
{
  return tcc_add_library_internal(s1, "%s/%s", filename, AFF_PRINT_ERROR, s1->crt_paths, s1->nb_crt_paths);
}
#endif

/* the library name is the same as the argument of the '-l' option */
LIBTCCAPI int tcc_add_library(TCCState *s, const char *libraryname)
{
  static const char *const libs[] = {"%s/lib%s.so", "%s/lib%s.a", NULL};
  const char *const *pp = s->static_link ? libs + 1 : libs;
  int flags = s->filetype & AFF_WHOLE_ARCHIVE;
  while (*pp)
  {
    int ret = tcc_add_library_internal(s, *pp, libraryname, flags, s->library_paths, s->nb_library_paths);
    if (ret != FILE_NOT_FOUND)
      return ret;
    ++pp;
  }
  return tcc_add_dll(s, libraryname, AFF_PRINT_ERROR);
}

/* handle #pragma comment(lib,) */
ST_FUNC void tcc_add_pragma_libs(TCCState *s1)
{
  int i;
  for (i = 0; i < s1->nb_pragma_libs; i++)
    tcc_add_library(s1, s1->pragma_libs[i]);
}

LIBTCCAPI int tcc_add_symbol(TCCState *s1, const char *name, const void *val)
{
  char buf[256];
  if (s1->leading_underscore)
  {
    buf[0] = '_';
    pstrcpy(buf + 1, sizeof(buf) - 1, name);
    name = buf;
  }
  set_global_sym(s1, name, NULL, (addr_t)(uintptr_t)val); /* NULL: SHN_ABS */
  return 0;
}

LIBTCCAPI void tcc_set_lib_path(TCCState *s, const char *path)
{
  tcc_pch_auto_reset(s);
  tcc_free(s->tcc_lib_path);
  s->tcc_lib_path = tcc_strdup(path);
}

/********************************************************/
/* options parser */

static int strstart(const char *val, const char **str)
{
  const char *p, *q;
  p = *str;
  q = val;
  while (*q)
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }
  *str = p;
  return 1;
}

/* Like strstart, but automatically takes into account that ld options can
 *
 * - start with double or single dash (e.g. '--soname' or '-soname')
 * - arguments can be given as separate or after '=' (e.g. '-Wl,-soname,x.so'
 *   or '-Wl,-soname=x.so')
 *
 * you provide `val` always in 'option[=]' form (no leading -)
 */
static int link_option(const char *str, const char *val, const char **ptr)
{
  const char *p, *q;
  int ret;

  /* there should be 1 or 2 dashes */
  if (*str++ != '-')
    return 0;
  if (*str == '-')
    str++;

  /* then str & val should match (potentially up to '=') */
  p = str;
  q = val;

  ret = 1;
  if (q[0] == '?')
  {
    ++q;
    if (strstart("no-", &p))
      ret = -1;
  }

  while (*q != '\0' && *q != '=')
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }

  /* '=' near eos means ',' or '=' is ok */
  if (*q == '=')
  {
    if (*p == 0)
      *ptr = p;
    if (*p != ',' && *p != '=')
      return 0;
    p++;
  }
  else if (*p)
  {
    return 0;
  }
  *ptr = p;
  return ret;
}

static int link_arg(const char *opt, const char *str)
{
  int l = strlen(opt);
  return 0 == strncmp(opt, str, l) && (str[l] == '\0' || str[l] == ',');
}

static const char *skip_linker_arg(const char **str)
{
  const char *s1 = *str;
  const char *s2 = strchr(s1, ',');
  *str = s2 ? s2++ : (s2 = s1 + strlen(s1));
  return s2;
}

static void copy_linker_arg(char **pp, const char *s, int sep)
{
  const char *q = s;
  char *p = *pp;
  int l = 0;
  if (p && sep)
    p[l = strlen(p)] = sep, ++l;
  skip_linker_arg(&q);
  pstrncpy(l + (*pp = tcc_realloc(p, q - s + l + 1)), s, q - s);
}

static void args_parser_add_file(TCCState *s, const char *filename, int filetype)
{
  struct filespec *f = tcc_malloc(sizeof *f + strlen(filename));
  f->type = filetype;
  strcpy(f->name, filename);
  dynarray_add(&s->files, &s->nb_files, f);
}

static void args_parser_add_group_marker(TCCState *s, int marker_type)
{
  struct filespec *f = tcc_malloc(sizeof *f + 1);
  f->type = marker_type;
  f->name[0] = '\0';
  dynarray_add(&s->files, &s->nb_files, f);
}

/* set linker options */
static int tcc_set_linker(TCCState *s, const char *option)
{
  TCCState *s1 = s;
  while (*option)
  {

    const char *p = NULL;
    char *end = NULL;
    int ignoring = 0;
    int ret;

    if (link_option(option, "Bsymbolic", &p))
    {
      s->symbolic = 1;
    }
    else if (link_option(option, "nostdlib", &p))
    {
      s->nostdlib = 1;
    }
    else if (link_option(option, "nodefaultlibs", &p))
    {
      s->nodefaultlibs = 1;
    }
    else if (link_option(option, "e=", &p) || link_option(option, "entry=", &p))
    {
      copy_linker_arg(&s->elf_entryname, p, 0);
    }
    else if (link_option(option, "fini=", &p))
    {
      copy_linker_arg(&s->fini_symbol, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "image-base=", &p) || link_option(option, "Ttext=", &p))
    {
      s->text_addr = strtoull(p, &end, 16);
      s->has_text_addr = 1;
    }
    else if (link_option(option, "init=", &p))
    {
      copy_linker_arg(&s->init_symbol, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "Map=", &p))
    {
      copy_linker_arg(&s->mapfile, p, 0);
      ignoring = 1;
    }
    else if (link_option(option, "oformat=", &p))
    {
#if PTR_SIZE == 8
      if (strstart("elf64-", &p))
      {
#else
      if (strstart("elf32-", &p))
      {
#endif
        s->output_format = TCC_OUTPUT_FORMAT_ELF;
      }
      else if (link_arg("binary", p))
      {
        s->output_format = TCC_OUTPUT_FORMAT_BINARY;
#ifdef TCC_TARGET_YAFF
      }
      else if (link_arg("yaff", p))
      {
        s->output_format = TCC_OUTPUT_FORMAT_YAFF;
#endif
      }
      else
        goto err;
    }
    else if (link_option(option, "as-needed", &p))
    {
      ignoring = 1;
    }
    else if (link_option(option, "O", &p))
    {
      ignoring = 1;
    }
    else if (link_option(option, "export-all-symbols", &p))
    {
      s->rdynamic = 1;
    }
    else if (link_option(option, "export-dynamic", &p))
    {
      s->rdynamic = 1;
    }
    else if (link_option(option, "rpath=", &p))
    {
      copy_linker_arg(&s->rpath, p, ':');
    }
    else if (link_option(option, "enable-new-dtags", &p))
    {
      s->enable_new_dtags = 1;
    }
    else if (link_option(option, "gc-sections", &p))
    {
      s->gc_sections = 1;
    }
    else if (link_option(option, "gc-sections-aggressive", &p))
    {
      s->gc_sections = 1;
      s->gc_sections_aggressive = 1;
    }
    else if (link_option(option, "no-gc-sections", &p))
    {
      s->gc_sections = 0;
    }
    else if (link_option(option, "section-alignment=", &p))
    {
      s->section_align = strtoul(p, &end, 16);
    }
    else if (link_option(option, "soname=", &p))
    {
      copy_linker_arg(&s->soname, p, 0);
    }
    else if (link_option(option, "install_name=", &p))
    {
      copy_linker_arg(&s->soname, p, 0);
    }
    else if (link_option(option, "start-group", &p))
    {
      args_parser_add_group_marker(s, AFF_GROUP_START);
    }
    else if (link_option(option, "end-group", &p))
    {
      args_parser_add_group_marker(s, AFF_GROUP_END);
    }
    else if (ret = link_option(option, "?whole-archive", &p), ret)
    {
      if (ret > 0)
        s->filetype |= AFF_WHOLE_ARCHIVE;
      else
        s->filetype &= ~AFF_WHOLE_ARCHIVE;
    }
    else if (link_option(option, "z=", &p))
    {
      ignoring = 1;
    }
    else if (p)
    {
      return 0;
    }
    else
    {
    err:
      return tcc_error_noabort("unsupported linker option '%s'", option);
    }
    if (ignoring)
      tcc_warning_c(warn_unsupported)("unsupported linker option '%s'", option);
    option = skip_linker_arg(&p);
  }
  return 1;
}

typedef struct TCCOption
{
  const char *name;
  uint16_t index;
  uint16_t flags;
} TCCOption;

enum
{
  TCC_OPTION_ignored = 0,
  TCC_OPTION_HELP,
  TCC_OPTION_HELP2,
  TCC_OPTION_v,
  TCC_OPTION_I,
  TCC_OPTION_D,
  TCC_OPTION_U,
  TCC_OPTION_P,
  TCC_OPTION_L,
  TCC_OPTION_B,
  TCC_OPTION_l,
  TCC_OPTION_bench,
  TCC_OPTION_bt,
  TCC_OPTION_b,
  TCC_OPTION_ba,
  TCC_OPTION_g,
  TCC_OPTION_c,
  TCC_OPTION_dumpmachine,
  TCC_OPTION_dumpversion,
  TCC_OPTION_d,
  TCC_OPTION_static,
  TCC_OPTION_std,
  TCC_OPTION_shared,
  TCC_OPTION_soname,
  TCC_OPTION_o,
  TCC_OPTION_r,
  TCC_OPTION_Wl,
  TCC_OPTION_Wp,
  TCC_OPTION_W,
  TCC_OPTION_O,
  TCC_OPTION_mfloat_abi,
  TCC_OPTION_mfpu,
  TCC_OPTION_march,
  TCC_OPTION_m,
  TCC_OPTION_f,
  TCC_OPTION_isystem,
  TCC_OPTION_iwithprefix,
  TCC_OPTION_include,
  TCC_OPTION_nostdinc,
  TCC_OPTION_nostdlib,
  TCC_OPTION_nodefaultlibs,
  TCC_OPTION_print_search_dirs,
  TCC_OPTION_rdynamic,
  TCC_OPTION_pthread,
  TCC_OPTION_run,
  TCC_OPTION_w,
  TCC_OPTION_E,
  TCC_OPTION_generate_pch,
  TCC_OPTION_use_pch,
  TCC_OPTION_verbose_pch,
  TCC_OPTION_fno_auto_pch,
  TCC_OPTION_M,
  TCC_OPTION_MD,
  TCC_OPTION_MF,
  TCC_OPTION_MM,
  TCC_OPTION_MMD,
  TCC_OPTION_MP,
  TCC_OPTION_x,
  TCC_OPTION_ar,
  TCC_OPTION_impdef,
  TCC_OPTION_dynamiclib,
  TCC_OPTION_flat_namespace,
  TCC_OPTION_two_levelnamespace,
  TCC_OPTION_undefined,
  TCC_OPTION_install_name,
  TCC_OPTION_compatibility_version,
  TCC_OPTION_current_version,
  TCC_OPTION_mpic_data_is_text_relative,
  TCC_OPTION_fpic,
  TCC_OPTION_fpie,
  TCC_OPTION_no_pie,
  TCC_OPTION_T,
#ifdef CONFIG_TCC_DEBUG
  TCC_OPTION_dump_ir,
#endif
};

#define TCC_OPTION_HAS_ARG 0x0001
#define TCC_OPTION_NOSEP 0x0002 /* cannot have space before option and arg */

static const TCCOption tcc_options[] = {
    {"h", TCC_OPTION_HELP, 0},
    {"-help", TCC_OPTION_HELP, 0},
    {"?", TCC_OPTION_HELP, 0},
    {"hh", TCC_OPTION_HELP2, 0},
    /* Must appear before the short "-v" option, otherwise "-verbose-pch" is parsed as "-v erbose-pch". */
    {"verbose-pch", TCC_OPTION_verbose_pch, 0},
    {"fno-auto-pch", TCC_OPTION_fno_auto_pch, 0},
    {"v", TCC_OPTION_v, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"-version", TCC_OPTION_v, 0}, /* handle as verbose, also prints version*/
    {"I", TCC_OPTION_I, TCC_OPTION_HAS_ARG},
    {"D", TCC_OPTION_D, TCC_OPTION_HAS_ARG},
    {"U", TCC_OPTION_U, TCC_OPTION_HAS_ARG},
    {"P", TCC_OPTION_P, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"L", TCC_OPTION_L, TCC_OPTION_HAS_ARG},
    {"B", TCC_OPTION_B, TCC_OPTION_HAS_ARG},
    {"l", TCC_OPTION_l, TCC_OPTION_HAS_ARG},
    {"bench", TCC_OPTION_bench, 0},
    {"generate-pch", TCC_OPTION_generate_pch, TCC_OPTION_HAS_ARG},
    {"use-pch", TCC_OPTION_use_pch, TCC_OPTION_HAS_ARG},
    {"g", TCC_OPTION_g, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"c", TCC_OPTION_c, 0},
    {"dumpmachine", TCC_OPTION_dumpmachine, 0},
    {"dumpversion", TCC_OPTION_dumpversion, 0},
#ifdef CONFIG_TCC_DEBUG
    /* Must appear before the short "-d" option, otherwise "-dump-ir" is parsed as "-d ump-ir". */
    {"dump-ir", TCC_OPTION_dump_ir, 0},
#endif
    {"d", TCC_OPTION_d, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"static", TCC_OPTION_static, 0},
    {"std", TCC_OPTION_std, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"shared", TCC_OPTION_shared, 0},
    {"soname", TCC_OPTION_soname, TCC_OPTION_HAS_ARG},
    {"o", TCC_OPTION_o, TCC_OPTION_HAS_ARG},
    {"pthread", TCC_OPTION_pthread, 0},
    {"run", TCC_OPTION_run, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"rdynamic", TCC_OPTION_rdynamic, 0},
    {"r", TCC_OPTION_r, 0},
    {"Wl,", TCC_OPTION_Wl, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"Wp,", TCC_OPTION_Wp, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"W", TCC_OPTION_W, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"O", TCC_OPTION_O, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"fpie", TCC_OPTION_fpie, 0},
    {"fpic", TCC_OPTION_fpic, 0},
    {"no-pie", TCC_OPTION_no_pie, 0},
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
    {"mfloat-abi=", TCC_OPTION_mfloat_abi, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfloat-abi", TCC_OPTION_mfloat_abi, TCC_OPTION_HAS_ARG},
    {"mfpu=", TCC_OPTION_mfpu, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"mfpu", TCC_OPTION_mfpu, TCC_OPTION_HAS_ARG},
    {"march=", TCC_OPTION_march, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"march", TCC_OPTION_march, TCC_OPTION_HAS_ARG},
    {"mpic-data-is-text-relative", TCC_OPTION_mpic_data_is_text_relative, 0},
#endif
    {"m", TCC_OPTION_m, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"f", TCC_OPTION_f, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP},
    {"isystem", TCC_OPTION_isystem, TCC_OPTION_HAS_ARG},
    {"include", TCC_OPTION_include, TCC_OPTION_HAS_ARG},
    {"nostdinc", TCC_OPTION_nostdinc, 0},
    {"nostdlib", TCC_OPTION_nostdlib, 0},
    {"nodefaultlibs", TCC_OPTION_nodefaultlibs, 0},
    {"print-search-dirs", TCC_OPTION_print_search_dirs, 0},
    {"w", TCC_OPTION_w, 0},
    {"E", TCC_OPTION_E, 0},
    {"M", TCC_OPTION_M, 0},
    {"MD", TCC_OPTION_MD, 0},
    {"MF", TCC_OPTION_MF, TCC_OPTION_HAS_ARG},
    {"MM", TCC_OPTION_MM, 0},
    {"MMD", TCC_OPTION_MMD, 0},
    {"MP", TCC_OPTION_MP, 0},
    {"x", TCC_OPTION_x, TCC_OPTION_HAS_ARG},
    {"T", TCC_OPTION_T, TCC_OPTION_HAS_ARG},
    {"ar", TCC_OPTION_ar, 0},
    /* ignored (silently, except after -Wunsupported) */
    {"arch", 0, TCC_OPTION_HAS_ARG},
    {"C", 0, 0},
    {"-param", 0, TCC_OPTION_HAS_ARG},
    {"pedantic", 0, 0},
    {"pipe", 0, 0},
    {"s", 0, 0},
    {"traditional", 0, 0},
    {NULL, 0, 0},
};

typedef struct FlagDef
{
  uint16_t offset;
  uint16_t flags;
  const char *name;
} FlagDef;

#define WD_ALL 0x0001    /* warning is activated when using -Wall */
#define FD_INVERT 0x0002 /* invert value before storing */

static const FlagDef options_W[] = {
    {offsetof(TCCState, warn_all), WD_ALL, "all"},
    {offsetof(TCCState, warn_error), 0, "error"},
    {offsetof(TCCState, warn_write_strings), 0, "write-strings"},
    {offsetof(TCCState, warn_unsupported), 0, "unsupported"},
    {offsetof(TCCState, warn_implicit_function_declaration), WD_ALL, "implicit-function-declaration"},
    {offsetof(TCCState, warn_discarded_qualifiers), WD_ALL, "discarded-qualifiers"},
    {0, 0, NULL}};

static const FlagDef options_f[] = {{offsetof(TCCState, char_is_unsigned), 0, "unsigned-char"},
                                    {offsetof(TCCState, char_is_unsigned), FD_INVERT, "signed-char"},
                                    {offsetof(TCCState, nocommon), FD_INVERT, "common"},
                                    {offsetof(TCCState, leading_underscore), 0, "leading-underscore"},
                                    {offsetof(TCCState, ms_extensions), 0, "ms-extensions"},
                                    {offsetof(TCCState, dollars_in_identifiers), 0, "dollars-in-identifiers"},
                                    {offsetof(TCCState, test_coverage), 0, "test-coverage"},
                                    {offsetof(TCCState, reverse_funcargs), 0, "reverse-funcargs"},
                                    {offsetof(TCCState, gnu89_inline), 0, "gnu89-inline"},
                                    {offsetof(TCCState, unwind_tables), 0, "asynchronous-unwind-tables"},
                                    {offsetof(TCCState, function_sections), 0, "function-sections"},
                                    {offsetof(TCCState, data_sections), 0, "data-sections"},
                                    /* IR optimization flags */
                                    {offsetof(TCCState, opt_dce), 0, "dce"},
                                    {offsetof(TCCState, opt_const_prop), 0, "const-prop"},
                                    {offsetof(TCCState, opt_copy_prop), 0, "copy-prop"},
                                    {offsetof(TCCState, opt_cse), 0, "cse"},
                                    {offsetof(TCCState, opt_bool_cse), 0, "bool-cse"},
                                    {offsetof(TCCState, opt_bool_idempotent), 0, "bool-idempotent"},
                                    {offsetof(TCCState, opt_bool_simplify), 0, "bool-simplify"},
                                    {offsetof(TCCState, opt_return_value), 0, "return-value-opt"},
                                    {offsetof(TCCState, opt_store_load_fwd), 0, "store-load-fwd"},
                                    {offsetof(TCCState, opt_redundant_store), 0, "redundant-store-elim"},
                                    {offsetof(TCCState, opt_dead_store), 0, "dead-store-elim"},
                                    {offsetof(TCCState, opt_fp_offset_cache), 0, "fp-offset-cache"},
                                    {offsetof(TCCState, opt_indexed_memory), 0, "indexed-memory"},
                                    {offsetof(TCCState, opt_disp_fusion), 0, "disp-fusion"},
                                    {offsetof(TCCState, opt_lea_fold), 0, "lea-fold"},
                                    {offsetof(TCCState, opt_postinc_fusion), 0, "postinc-fusion"},
                                    {offsetof(TCCState, opt_mla_fusion), 0, "mla-fusion"},
                                    {offsetof(TCCState, opt_stack_addr_cse), 0, "stack-addr-cse"},
                                    {offsetof(TCCState, opt_licm), 0, "licm"},
                                    {offsetof(TCCState, opt_strength_red), 0, "strength-red"},
                                    {offsetof(TCCState, opt_iv_strength_red), 0, "iv-strength-red"},
                                    {offsetof(TCCState, opt_loop_unroll), 0, "loop-unroll"},
                                    {offsetof(TCCState, opt_loop_rotation), 0, "loop-rotation"},
                                    {offsetof(TCCState, opt_jump_threading), 0, "jump-threading"},
                                    {offsetof(TCCState, opt_nonneg_fold), 0, "nonneg-fold"},
                                    {offsetof(TCCState, opt_vrp), 0, "vrp"},
                                    {offsetof(TCCState, opt_float_narrow), 0, "float-narrow"},
                                    {offsetof(TCCState, opt_inline_functions), 0, "inline-functions"},
                                    {offsetof(TCCState, opt_inline_small), 0, "inline-small-functions"},
                                    {offsetof(TCCState, instrument_functions), 0, "instrument-functions"},
                                    {0, 0, NULL}};

static const FlagDef options_m[] = {{offsetof(TCCState, ms_bitfields), 0, "ms-bitfields"}, {0, 0, NULL}};

static int set_flag(TCCState *s, const FlagDef *flags, const char *name)
{
  int value, mask, ret;
  const FlagDef *p;
  const char *r;
  unsigned char *f;

  r = name, value = !strstart("no-", &r), mask = 0;

  /* when called with options_W, look for -W[no-]error=<option> */
  if ((flags->flags & WD_ALL) && strstart("error=", &r))
    value = value ? WARN_ON | WARN_ERR : WARN_NOE, mask = WARN_ON;

  for (ret = -1, p = flags; p->name; ++p)
  {
    if (ret)
    {
      if (strcmp(r, p->name))
        continue;
    }
    else
    {
      if (0 == (p->flags & WD_ALL))
        continue;
    }

    f = (unsigned char *)s + p->offset;
    *f = (*f & mask) | (value ^ !!(p->flags & FD_INVERT));

    if (ret)
    {
      ret = 0;
      if (strcmp(r, "all"))
        break;
    }
  }
  return ret;
}

static const char dumpmachine_str[] =
/* this is a best guess, please refine as necessary */
#if defined TCC_TARGET_ARM_THUMB
    "armv8m"
#if defined TCC_TARGET_YasOS
    "-yasos"
#endif
#endif
    ;

static int args_parser_make_argv(const char *r, int *argc, char ***argv)
{
  int ret = 0, q, c;
  CString str;
  for (;;)
  {
    while (c = (unsigned char)*r, c && c <= ' ')
      ++r;
    if (c == 0)
      break;
    q = 0;
    cstr_new(&str);
    while (c = (unsigned char)*r, c)
    {
      ++r;
      if (c == '\\' && (*r == '"' || *r == '\\'))
      {
        c = *r++;
      }
      else if (c == '"')
      {
        q = !q;
        continue;
      }
      else if (q == 0 && c <= ' ')
      {
        break;
      }
      cstr_ccat(&str, c);
    }
    cstr_ccat(&str, 0);
    dynarray_add(argv, argc, tcc_strdup(str.data));
    cstr_free(&str);
    ++ret;
  }
  return ret;
}

/* read list file */
static int args_parser_listfile(TCCState *s, const char *filename, int optind, int *pargc, char ***pargv)
{
  TCCState *s1 = s;
  int fd, i;
  char *p;
  int argc = 0;
  char **argv = NULL;

  fd = open(filename, O_RDONLY | O_BINARY);
  if (fd < 0)
    return tcc_error_noabort("listfile '%s' not found", filename);

  p = tcc_load_text(fd);
  for (i = 0; i < *pargc; ++i)
    if (i == optind)
      args_parser_make_argv(p, &argc, &argv);
    else
      dynarray_add(&argv, &argc, tcc_strdup((*pargv)[i]));

  tcc_free(p);
  dynarray_reset(&s->argv, &s->argc);
  *pargc = s->argc = argc, *pargv = s->argv = argv;
  return 0;
}

PUB_FUNC int tcc_parse_args(TCCState *s, int *pargc, char ***pargv, int optind)
{
  TCCState *s1 = s;
  const TCCOption *popt;
  const char *optarg, *r;
  const char *run = NULL;
  int x;
  int tool = 0, arg_start = 0, noaction = optind;
  char **argv = *pargv;
  int argc = *pargc;

  cstr_reset(&s->linker_arg);

  while (optind < argc)
  {
    r = argv[optind];
    if (r[0] == '@' && r[1] != '\0')
    {
      if (args_parser_listfile(s, r + 1, optind, &argc, &argv))
        return -1;
      continue;
    }
    optind++;
    if (tool)
    {
      if (r[0] == '-' && r[1] == 'v' && r[2] == 0)
        ++s->verbose;
      continue;
    }
  reparse:
    if (r[0] != '-' || r[1] == '\0')
    {
      args_parser_add_file(s, r, s->filetype);
      if (run)
      {
      dorun:
        if (tcc_set_options(s, run))
          return -1;
        arg_start = optind - 1;
        break;
      }
      continue;
    }

    /* allow "tcc files... -run -- args ..." */
    if (r[1] == '-' && r[2] == '\0' && run)
      goto dorun;

    /* find option in table */
    for (popt = tcc_options;; ++popt)
    {
      const char *p1 = popt->name;
      const char *r1 = r + 1;
      if (p1 == NULL)
        return tcc_error_noabort("invalid option -- '%s'", r);
      if (!strstart(p1, &r1))
        continue;
      optarg = r1;
      if (popt->flags & TCC_OPTION_HAS_ARG)
      {
        if (*r1 == '\0' && !(popt->flags & TCC_OPTION_NOSEP))
        {
          if (optind >= argc)
          arg_err:
            return tcc_error_noabort("argument to '%s' is missing", r);
          optarg = argv[optind++];
        }
      }
      else if (*r1 != '\0')
        continue;
      break;
    }

    switch (popt->index)
    {
    case TCC_OPTION_HELP:
      x = OPT_HELP;
      goto extra_action;
    case TCC_OPTION_HELP2:
      x = OPT_HELP2;
      goto extra_action;
    case TCC_OPTION_I:
      tcc_add_include_path(s, optarg);
      break;
    case TCC_OPTION_D:
      tcc_define_symbol(s, optarg, NULL);
      break;
    case TCC_OPTION_U:
      tcc_undefine_symbol(s, optarg);
      break;
    case TCC_OPTION_L:
      tcc_add_library_path(s, optarg);
      break;
    case TCC_OPTION_B:
      /* set tcc utilities path (mainly for tcc development) */
      tcc_set_lib_path(s, optarg);
      ++noaction;
      break;
    case TCC_OPTION_l:
      args_parser_add_file(s, optarg, AFF_TYPE_LIB | (s->filetype & ~AFF_TYPE_MASK));
      s->nb_libraries++;
      break;
    case TCC_OPTION_pthread:
      s->option_pthread = 1;
      break;
    case TCC_OPTION_bench:
      s->do_bench = 1;
      break;
    case TCC_OPTION_g:
      s->do_debug = 2;
      s->dwarf = CONFIG_DWARF_VERSION;
      if (strstart("dwarf", &optarg))
      {
        s->dwarf = (*optarg) ? (0 - atoi(optarg)) : DEFAULT_DWARF_VERSION;
      }
      else if (isnum(*optarg))
      {
        x = *optarg - '0';
        /* -g0 = no info, -g1 = lines/functions only, -g2 = full info */
        s->do_debug = x > 2 ? 2 : x == 0 && s->do_backtrace ? 1 : x;
      }
      break;
    case TCC_OPTION_c:
      x = TCC_OUTPUT_OBJ;
    set_output_type:
      if (s->output_type)
        tcc_warning("-%s: overriding compiler action already specified", popt->name);
      s->output_type = x;
      break;
    set_output_type_add_file:
      if (s->output_type)
        tcc_warning("-%s: overriding compiler action already specified", popt->name);
      s->output_type = x;
      args_parser_add_file(s, optarg, AFF_TYPE_C | (s->filetype & ~AFF_TYPE_MASK));
      break;
    case TCC_OPTION_d:
      if (*optarg == 'D')
        s->dflag = 3;
      else if (*optarg == 'M')
        s->dflag = 7;
      else if (*optarg == 't')
        s->dflag = 16;
      else if (isnum(*optarg))
        s->g_debug |= atoi(optarg);
      else
        goto unsupported_option;
      break;
    case TCC_OPTION_static:
      s->static_link = 1;
      break;
    case TCC_OPTION_std:
      if (strcmp(optarg, "=c11") == 0 || strcmp(optarg, "=gnu11") == 0)
        s->cversion = 201112;
      else if (strcmp(optarg, "=c17") == 0 || strcmp(optarg, "=gnu17") == 0 || strcmp(optarg, "=c18") == 0 ||
               strcmp(optarg, "=gnu18") == 0)
        s->cversion = 201710;
      else if (strcmp(optarg, "=c23") == 0 || strcmp(optarg, "=gnu23") == 0 || strcmp(optarg, "=c2x") == 0 ||
               strcmp(optarg, "=gnu2x") == 0)
        s->cversion = 202311;
      break;
    case TCC_OPTION_shared:
      x = TCC_OUTPUT_DLL;
      goto set_output_type;
    case TCC_OPTION_soname:
      s->soname = tcc_strdup(optarg);
      break;
    case TCC_OPTION_o:
      if (s->outfile)
      {
        tcc_warning("multiple -o option");
        tcc_free(s->outfile);
      }
      s->outfile = tcc_strdup(optarg);
      break;
    case TCC_OPTION_r:
      /* generate a .o merging several output files */
      s->option_r = 1;
      x = TCC_OUTPUT_OBJ;
      goto set_output_type;
    case TCC_OPTION_isystem:
      tcc_add_sysinclude_path(s, optarg);
      break;
    case TCC_OPTION_include:
      cstr_printf(&s->cmdline_incl, "#include \"%s\"\n", optarg);
      break;
    case TCC_OPTION_nostdinc:
      s->nostdinc = 1;
      break;
    case TCC_OPTION_nostdlib:
      s->nostdlib = 1;
      break;
    case TCC_OPTION_nodefaultlibs:
      s->nodefaultlibs = 1;
      break;
    case TCC_OPTION_v:
      do
        ++s->verbose;
      while (*optarg++ == 'v');
      ++noaction;
      break;
    case TCC_OPTION_f:
      /* Handle -finline-limit=N */
      if (!strncmp(optarg, "inline-limit=", 13))
      {
        int n = atoi(optarg + 13);
        if (n > 0)
          s->opt_inline_limit = n;
        break;
      }
      /* Handle -fno-builtin-<name> flags */
      if (!strncmp(optarg, "no-builtin-", 11))
      {
        const char *bname = optarg + 11;
        if (!strcmp(bname, "abs"))
          s->no_builtin_funcs |= NO_BUILTIN_ABS;
        else if (!strcmp(bname, "labs"))
          s->no_builtin_funcs |= NO_BUILTIN_LABS;
        else if (!strcmp(bname, "llabs"))
          s->no_builtin_funcs |= NO_BUILTIN_LLABS;
        else if (!strcmp(bname, "uabs"))
          s->no_builtin_funcs |= NO_BUILTIN_UABS;
        else if (!strcmp(bname, "ulabs"))
          s->no_builtin_funcs |= NO_BUILTIN_ULABS;
        else if (!strcmp(bname, "ullabs"))
          s->no_builtin_funcs |= NO_BUILTIN_ULLABS;
        else if (!strcmp(bname, "umaxabs"))
          s->no_builtin_funcs |= NO_BUILTIN_UMAXABS;
        /* Silently accept other -fno-builtin-<name> flags */
        break;
      }
      if (set_flag(s, options_f, optarg) < 0)
        goto unsupported_option;
      break;
    case TCC_OPTION_fpic:
    case TCC_OPTION_fpie:
      s->pic = 1;
      break;
    case TCC_OPTION_no_pie:
      s->no_pie = 1;
      break;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
    case TCC_OPTION_mfloat_abi:
      if (!strcmp(optarg, "soft"))
      {
        s->float_abi = ARM_SOFT_FLOAT;
      }
      else if (!strcmp(optarg, "softfp"))
      {
        s->float_abi = ARM_SOFTFP_FLOAT;
      }
      else if (!strcmp(optarg, "hard"))
        s->float_abi = ARM_HARD_FLOAT;
      else
        return tcc_error_noabort("unsupported float abi '%s'", optarg);
      break;
    case TCC_OPTION_mfpu:
      if (!strcmp(optarg, "vfp") || !strcmp(optarg, "vfpv2"))
      {
        s->fpu_type = ARM_FPU_VFP;
      }
      else if (!strcmp(optarg, "vfpv3") || !strcmp(optarg, "vfpv3-d16"))
      {
        s->fpu_type = ARM_FPU_VFPV3;
      }
      else if (!strcmp(optarg, "vfpv4") || !strcmp(optarg, "vfpv4-d16"))
      {
        s->fpu_type = ARM_FPU_VFPV4;
      }
      else if (!strcmp(optarg, "fpv4-sp-d16"))
      {
        s->fpu_type = ARM_FPU_FPV4_SP_D16;
      }
      else if (!strcmp(optarg, "fpv5-sp-d16"))
      {
        s->fpu_type = ARM_FPU_FPV5_SP_D16;
      }
      else if (!strcmp(optarg, "fpv5-d16"))
      {
        s->fpu_type = ARM_FPU_FPV5_D16;
      }
      else if (!strcmp(optarg, "neon") || !strcmp(optarg, "neon-vfpv3"))
      {
        s->fpu_type = ARM_FPU_NEON;
      }
      else if (!strcmp(optarg, "neon-vfpv4"))
      {
        s->fpu_type = ARM_FPU_NEON_VFPV4;
      }
      else if (!strcmp(optarg, "neon-fp-armv8") || !strcmp(optarg, "crypto-neon-fp-armv8"))
      {
        s->fpu_type = ARM_FPU_NEON_FP_ARMV8;
      }
      else if (!strcmp(optarg, "auto"))
      {
        s->fpu_type = ARM_FPU_AUTO;
      }
      else if (!strcmp(optarg, "none"))
      {
        s->fpu_type = ARM_FPU_NONE;
      }
      else
      {
        return tcc_error_noabort("unsupported FPU type '%s'", optarg);
      }
      break;
    case TCC_OPTION_march:
      s->march_str = optarg;
      break;
    case TCC_OPTION_mpic_data_is_text_relative:
      printf("Setting text and data separation to: 1\n");
      s->text_and_data_separation = 1;
      break;
#endif
    case TCC_OPTION_m:
      if (set_flag(s, options_m, optarg) < 0)
      {
        if (x = atoi(optarg), x != 32 && x != 64)
          goto unsupported_option;
        if (PTR_SIZE != x / 8)
          return x;
        ++noaction;
      }
      break;
    case TCC_OPTION_W:
      s->warn_none = 0;
      if (optarg[0] && set_flag(s, options_W, optarg) < 0)
        goto unsupported_option;
      break;
    case TCC_OPTION_w:
      s->warn_none = 1;
      break;
    case TCC_OPTION_rdynamic:
      s->rdynamic = 1;
      break;
    case TCC_OPTION_Wl:
      if (s->linker_arg.size)
        ((char *)s->linker_arg.data)[s->linker_arg.size - 1] = ',';
      cstr_cat(&s->linker_arg, optarg, 0);
      x = tcc_set_linker(s, s->linker_arg.data);
      if (x)
        cstr_reset(&s->linker_arg);
      if (x < 0)
        return -1;
      break;
    case TCC_OPTION_Wp:
      r = optarg;
      goto reparse;
    case TCC_OPTION_E:
      x = TCC_OUTPUT_PREPROCESS;
      goto set_output_type;
    case TCC_OPTION_generate_pch:
      x = TCC_OUTPUT_PCH;
      goto set_output_type_add_file;
    case TCC_OPTION_use_pch:
      tcc_free(s->pch_infile);
      s->pch_infile = tcc_strdup(optarg);
      break;
    case TCC_OPTION_verbose_pch:
      s->pch_verbose = 1;
      break;
    case TCC_OPTION_fno_auto_pch:
      s->pch_auto_enabled = 0;
      break;
    case TCC_OPTION_P:
      s->Pflag = atoi(optarg) + 1;
      break;
    case TCC_OPTION_M:
      s->include_sys_deps = 1;
      // fall through
    case TCC_OPTION_MM:
      s->just_deps = 1;
      if (!s->deps_outfile)
        s->deps_outfile = tcc_strdup("-");
      // fall through
    case TCC_OPTION_MMD:
      s->gen_deps = 1;
      break;
    case TCC_OPTION_MD:
      s->gen_deps = 1;
      s->include_sys_deps = 1;
      break;
    case TCC_OPTION_MF:
      s->deps_outfile = tcc_strdup(optarg);
      break;
    case TCC_OPTION_MP:
      s->gen_phony_deps = 1;
      break;
    case TCC_OPTION_dumpmachine:
      printf("%s\n", dumpmachine_str);
      exit(0);
    case TCC_OPTION_dumpversion:
      printf("%s\n", TCC_VERSION);
      exit(0);
    case TCC_OPTION_x:
      x = 0;
      if (*optarg == 'c')
        x = AFF_TYPE_C;
      else if (*optarg == 'a')
        x = AFF_TYPE_ASMPP;
      else if (*optarg == 'b')
        x = AFF_TYPE_BIN;
      else if (*optarg == 'n')
        x = AFF_TYPE_NONE;
      else
        tcc_warning("unsupported language '%s'", optarg);
      s->filetype = x | (s->filetype & ~AFF_TYPE_MASK);
      break;
    case TCC_OPTION_O:
      s->optimize = atoi(optarg);
      /* Enable all IR optimizations when -O1 or higher */
      if (s->optimize >= 1)
      {
        s->opt_dce = 1;
        s->opt_const_prop = 1;
        s->opt_copy_prop = 1;
        s->opt_cse = 1;
        s->opt_bool_cse = 1;
        s->opt_bool_idempotent = 1;
        s->opt_bool_simplify = 1;
        s->opt_return_value = 1;
        s->opt_store_load_fwd = 1;
        s->opt_redundant_store = 1;
        s->opt_dead_store = 1;
        s->opt_indexed_memory = 1; /* Fuse SHL+ADD+LOAD/STORE into indexed ops */
        s->opt_disp_fusion = 1;    /* Fuse ADD+imm+LOAD/STORE into displacement-addressed ops */
        s->opt_lea_fold = 1;       /* Fold LEA Addr[StackLoc]+deref into direct stack slot access */
        s->opt_postinc_fusion = 1; /* Fuse LOAD/STORE + ADD into post-increment ops */
        s->opt_mla_fusion = 1;     /* Fuse MUL+ADD into MLA */
        /* fp-offset-cache disabled: miscompiles loops when combined with
           iv-strength-red (e.g. SHA-1 sha_transform).  Can still be
           enabled manually with -ffp-offset-cache for debugging. */
        s->opt_stack_addr_cse = 1;  /* Hoist repeated stack address computations */
        s->opt_licm = 1;            /* Loop-invariant code motion */
        s->opt_ipc = 1;             /* Interprocedural constant propagation */
        s->opt_strength_red = 1;    /* Strength reduction for multiply */
        s->opt_iv_strength_red = 1; /* IV strength reduction for array loops */
        s->opt_loop_unroll = 1;    /* Full-unroll small constant-trip-count loops */
        s->opt_loop_rotation = 1;  /* Rotate top-tested loops to bottom-tested */
        s->opt_nonneg_fold = 1;     /* Non-negative value branch folding */
        s->opt_vrp = 1;             /* Value range propagation branch folding */
        s->opt_float_narrow = 1;    /* Narrow double math to float when safe */
        s->opt_jump_threading = 1;  /* Jump threading optimization */
        s->opt_inline_small = 1;    /* Inline tiny static/inline functions (≤30 words) */
        if (!s->opt_inline_limit)
          s->opt_inline_limit = 30;
      }
      if (s->optimize >= 2)
      {
        s->opt_inline_functions = 1; /* Inline small static/inline functions (≤60 words) */
        if (s->opt_inline_limit < 60)
          s->opt_inline_limit = 60;
      }
      break;
    case TCC_OPTION_T:
      if (s->linker_script)
      {
        tcc_warning("multiple -T option");
        tcc_free(s->linker_script);
      }
      s->linker_script = tcc_strdup(optarg);
      break;
#ifdef CONFIG_TCC_DEBUG
    case TCC_OPTION_dump_ir:
      s->dump_ir = 1;
      break;
#endif
    case TCC_OPTION_print_search_dirs:
      x = OPT_PRINT_DIRS;
      goto extra_action;
    case TCC_OPTION_impdef:
      x = OPT_IMPDEF;
      goto extra_action;
    case TCC_OPTION_ar:
      x = OPT_AR;
    extra_action:
      arg_start = optind - 1;
      if (arg_start != noaction)
        return tcc_error_noabort("cannot parse %s here", r);
      tool = x;
      break;
    default:
    unsupported_option:
      tcc_warning_c(warn_unsupported)("unsupported option '%s'", r);
      break;
    }
  }
  if (s->linker_arg.size)
  {
    r = s->linker_arg.data;
    goto arg_err;
  }
  *pargc = argc - arg_start;
  *pargv = argv + arg_start;
  if (tool)
    return tool;
  if (optind != noaction)
    return 0;
  if (s->verbose == 2)
    return OPT_PRINT_DIRS;
  if (s->verbose)
    return OPT_V;
  return OPT_HELP;
}

LIBTCCAPI int tcc_set_options(TCCState *s, const char *r)
{
  char **argv = NULL;
  int argc = 0, ret;
  args_parser_make_argv(r, &argc, &argv);
  ret = tcc_parse_args(s, &argc, &argv, 0);
  dynarray_reset(&argv, &argc);
  return ret < 0 ? ret : 0;
}

PUB_FUNC void tcc_bench_log(TCCState *s1, const char *operation, const char *name, unsigned elapsed_ms)
{
  if (!s1 || !s1->do_bench)
    return;
  if (!name || !name[0])
    name = "<unknown>";
  fprintf(stderr, "# bench %-14s %6u ms  %s\n", operation, elapsed_ms, name);
}

static void tcc_print_bench_breakdown(const char *label, unsigned total_time, unsigned count)
{
  if (!count)
    return;
  fprintf(stderr, "# bench total %-16s %6u ms  %4u calls  %7.2f ms avg\n", label, total_time, count,
         (double)total_time / count);
}

PUB_FUNC void tcc_print_stats(TCCState *s1, unsigned total_time)
{
  if (!total_time)
    total_time = 1;
  fprintf(stderr,
          "# %d idents, %d lines, %u bytes\n"
          "# %0.3f s, %u lines/s, %0.1f MB/s\n",
          total_idents, total_lines, total_bytes, (double)total_time / 1000, (unsigned)total_lines * 1000 / total_time,
          (double)total_bytes / 1000 / total_time);
  fprintf(stderr, "# text %u, data.rw %u, data.ro %u, bss %u bytes\n", s1->total_output[0], s1->total_output[1],
          s1->total_output[2], s1->total_output[3]);
  tcc_print_bench_breakdown("open", s1->bench_file_open_time, s1->bench_file_open_count);
  tcc_print_bench_breakdown("resolve", s1->bench_library_resolve_time, s1->bench_library_resolve_count);
    tcc_print_bench_breakdown("compile-setup", s1->bench_compile_setup_time, s1->bench_compile_setup_count);
    tcc_print_bench_breakdown("compile-exec", s1->bench_compile_exec_time, s1->bench_compile_exec_count);
    tcc_print_bench_breakdown("compile-finalize", s1->bench_compile_finalize_time, s1->bench_compile_finalize_count);
  tcc_print_bench_breakdown("func-body", s1->bench_function_body_time, s1->bench_function_body_count);
  tcc_print_bench_breakdown("func-opt", s1->bench_function_opt_time, s1->bench_function_opt_count);
  tcc_print_bench_breakdown("func-alloc", s1->bench_function_alloc_time, s1->bench_function_alloc_count);
  tcc_print_bench_breakdown("func-codegen", s1->bench_function_codegen_time, s1->bench_function_codegen_count);
  tcc_print_bench_breakdown("compile", s1->bench_compile_time, s1->bench_compile_count);
  tcc_print_bench_breakdown("obj", s1->bench_object_load_time, s1->bench_object_load_count);
  tcc_print_bench_breakdown("archive", s1->bench_archive_load_time, s1->bench_archive_load_count);
  if (s1->bench_archive_member_count)
    fprintf(stderr, "# bench total archive-members %u\n", s1->bench_archive_member_count);
  tcc_print_bench_breakdown("dll", s1->bench_dll_load_time, s1->bench_dll_load_count);
  tcc_print_bench_breakdown("ldscript", s1->bench_ldscript_load_time, s1->bench_ldscript_load_count);
  tcc_print_bench_breakdown("output", s1->bench_output_time, s1->bench_output_count);
#ifdef MEM_DEBUG
  fprintf(stderr, "# memory usage");
#ifdef TCC_IS_NATIVE
  if (s1->run_size)
  {
    Section *s = s1->symtab;
    unsigned ms = s->data_offset + s->link->data_offset + s->hash->data_offset;
    unsigned rs = s1->run_size;
    fprintf(stderr, ": %d to run, %d symbols, %d other,", rs, ms, mem_cur_size - rs - ms);
  }
#endif
  fprintf(stderr, " %d max (bytes)\n", mem_max_size);
#endif
}
