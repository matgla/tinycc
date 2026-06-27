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
 * used by name-gated optimizer passes (e.g. self_copy_elim, float_narrowing).
 * The unit-test harness lets individual tests populate a token→name table so
 * those passes can reach their positive folds.  CValue is opaque here (no tcc.h),
 * hence the void* parameter — the linker resolves by name regardless. */

#define UTB_MAX_TOK 256
static const char *utb_tok_names[UTB_MAX_TOK];

void utb_set_tok_str(int tok, const char *name)
{
  if (tok >= 0 && tok < UTB_MAX_TOK)
    utb_tok_names[tok] = name;
}

const char *get_tok_str(int v, void *cv)
{
  (void)cv;
  if (v >= 0 && v < UTB_MAX_TOK && utb_tok_names[v])
    return utb_tok_names[v];
  return "?";
}

/* ───── Frontend / IR link stubs pulled in by core, operand and opt modules ─────
 *
 * These symbols are referenced by functions that survive --gc-sections once
 * the Phase 2 IR-core/data-structure suites exercise tcc_ir_alloc/put/etc.
 * They are either unreachable at runtime for hand-built IR tests or have
 * trivial semantics there, so opaque stubs are enough.
 */
struct Sym;
struct TCCIRState;
struct LSLiveIntervalState;
struct BufferedFile;

/* Minimal CType compatible with tcc.h (kept opaque here so we need not pull in
 * tcc.h, which redefines malloc/free/realloc).  Must match the real layout. */
typedef struct CType
{
  int t;
  struct Sym *ref;
} CType;

/* From tccgen.c / tccpp.c — global state touched by tcc_ir_put(). */
int nocode_wanted = 0;
struct BufferedFile *file = NULL;
CType func_old_type;

/* From arm-thumb-gen.c — allocator init/shutdown. */
int tcc_gen_machine_number_of_registers(void)
{
  return 16;
}

/* From tccelf.c — symbol registration; unit tests don't emit ELF. */
typedef unsigned long addr_t;
struct Section;

int put_extern_sym2(struct Sym *sym, addr_t value, unsigned long size,
                    int info, int other, int shndx, const char *name)
{
  (void)sym; (void)value; (void)size; (void)info;
  (void)other; (void)shndx; (void)name;
  return 0;
}

/* From arm-thumb-gen.c — soft-float helper names; unit tests don't lower calls. */
struct SValue;

const char *tcc_get_abi_softcall_name(struct SValue *src1, struct SValue *src2,
                                       struct SValue *dest, int op)
{
  (void)src1; (void)src2; (void)dest; (void)op;
  return NULL;
}

/* From tcc.c — operand width helper. */
int tcc_is_64bit_operand(struct SValue *sv)
{
  (void)sv;
  return 0;
}

/* From tccgen.c — type size/alignment. */
int type_size(const struct CType *type, int *a)
{
  (void)type;
  if (a)
    *a = 4;
  return 4;
}

/* From tccgen.c — float type predicate used by operand conversion. */
int is_float(int t)
{
  (void)t;
  return 0;
}

/* From tccopt.c — FP materialization cache teardown. */
void tcc_opt_fp_mat_cache_free(struct TCCIRState *ir)
{
  (void)ir;
}

/* ───── Frontend link stubs pulled in by optimizer passes ─────
 *
 * opt_constfold.c/opt_utils.c reference the symbol-table helpers below.
 * They are unreachable at runtime for hand-built IR tests, but --gc-sections
 * keeps them reachable from pass entry points, so the linker needs a
 * definition.  Keep them opaque (no tcc.h) — pointer args/returns are enough.
 */

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

/* opt_constprop.c's global_init_prop pass calls sym_find(); it survives
 * --gc-sections once the metamorphic suite references other opt_constprop
 * passes, but it is never in the metamorphic pass list so it is not executed.
 * Hand-built IR has no frontend symbol table, so report "not found". */
struct Sym *sym_find(int v)
{
  (void)v;
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
