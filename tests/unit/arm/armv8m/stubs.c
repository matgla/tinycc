/*
 *  stubs.c - shared UT stub, dual-build split.
 *
 *  This file is linked into several unit-test binaries with different needs:
 *    - The main (run_unit_tests), backend (UT2) and other binaries do NOT
 *      link libtcc.c, so they need the full stub layer below (allocators,
 *      the tcc_state global, ELF/section fakes, etc.).
 *    - build_ssaopt (UT11) links the REAL libtcc.c + ir/opt/ssa_opt*.c, which
 *      already own those symbols; it compiles every TU with -DUT_SSA_OPT_REAL
 *      (see the build_ssaopt rules in the Makefile), so the shared stubs must
 *      be skipped there to avoid multiple-definition clashes.
 *
 *  Same guard idiom as ra_link_stubs.c. Keep the two branches in sync when a
 *  new shared symbol is genuinely needed by BOTH builds (define it outside
 *  the guard in that case).
 */
#ifndef UT_SSA_OPT_REAL
/* ===== shared builds (main / backend / tccpp / ... ): full stub layer ===== */
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

/* libtcc.c's libc_free() (declared in tcc.h) is the established escape hatch
 * for releasing memory that came from a real libc allocator (e.g. realpath(),
 * or here open_memstream() in test_ir_dump.c) rather than tcc's own
 * allocator -- tcc.h #defines plain `free` to an intentionally-undefined
 * `use_tcc_free` to catch accidental raw frees of tcc_malloc'd memory. The
 * real definition lives in libtcc.c, which is deliberately not linked into
 * this UT binary (see UT_COVERAGE_ONLY_SRCS in Makefile: linking it would
 * collide with this file's tcc_malloc/tcc_free/etc. stubs). Provide the
 * same minimal passthrough here so callers that need to free libc-allocated
 * buffers have a symbol to link against. */
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

/* expect() is declared `ST_FUNC NORETURN void expect(const char *msg)` in
 * tcc.h (tccpp.c: `tcc_error("%s expected", msg)`). It's referenced by a
 * handful of operand-validation error paths in arm-thumb-asm.c (e.g.
 * thumb_generate_opcode_for_data_processing's clz/bfc operand checks) that
 * survive --gc-sections once test_arm_thumb_asm.c calls into those
 * dispatchers directly (bypassing the full lexer-driven asm_opcode() entry
 * point tccpp.c/tccasm.c would normally reach it through). Every unit test
 * that exercises this file sticks to well-formed operands, so this path is
 * unreachable at runtime; abort loudly (matching _tcc_error above) if that
 * ever changes. */
void expect(const char *msg)
{
  fprintf(stderr, "[test stub] expect: '%s' expected\n", msg);
  abort();
}

/* ───── CString helpers for arm-thumb-asm.c's subst_asm_operand() ─────
 *
 * tccpp.c (not linked into the main UT binary) owns the real cstr_* family.
 * subst_asm_operand() is reachable from this harness once these minimal
 * implementations are provided.  They deliberately avoid libc malloc/free
 * (tcc.h redefines them when included) and use the tcc_realloc/tcc_free
 * stubs defined above.
 *
 * The layout below must match tcc.h's `typedef struct CString` exactly.
 */

typedef struct CString
{
  int size;
  int size_allocated;
  char *data;
} CString;

static void cstr_realloc(CString *cstr, int new_size)
{
  int size = cstr->size_allocated;
  if (size < 8)
    size = 8;
  while (size < new_size)
    size = size * 2;
  cstr->data = (char *)tcc_realloc(cstr->data, size);
  cstr->size_allocated = size;
}

/* Weak: test_tccdbg.c provides its own (non-stub) implementations when both are
 * linked into the main unit-test binary; in other binaries that link stubs.c but
 * not test_tccdbg.c, these weak symbols still satisfy the linker. */
__attribute__((weak)) void cstr_new(CString *cstr)
{
  memset(cstr, 0, sizeof(CString));
}

__attribute__((weak)) void cstr_free(CString *cstr)
{
  tcc_free(cstr->data);
}

void cstr_reset(CString *cstr)
{
  cstr->size = 0;
}

void cstr_ccat(CString *cstr, int ch)
{
  int size = cstr->size + 1;
  if (size > cstr->size_allocated)
    cstr_realloc(cstr, size);
  cstr->data[size - 1] = (char)ch;
  cstr->size = size;
}

void cstr_cat(CString *cstr, const char *str, int len)
{
  int size;
  if (len <= 0)
    len = (int)strlen(str) + 1 + len;
  size = cstr->size + len;
  if (size > cstr->size_allocated)
    cstr_realloc(cstr, size);
  memmove(cstr->data + cstr->size, str, len);
  cstr->size = size;
}

int cstr_vprintf(CString *cstr, const char *fmt, va_list ap)
{
  va_list v;
  int len, size = 80;
  for (;;)
  {
    size += cstr->size;
    if (size > cstr->size_allocated)
      cstr_realloc(cstr, size);
    size = cstr->size_allocated - cstr->size;
    va_copy(v, ap);
    len = vsnprintf(cstr->data + cstr->size, size, fmt, v);
    va_end(v);
    if (len < 0)
      return -1;
    if (len < size)
      break;
  }
  cstr->size += len;
  return len;
}

__attribute__((weak)) int cstr_printf(CString *cstr, const char *fmt, ...)
{
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = cstr_vprintf(cstr, fmt, ap);
  va_end(ap);
  return len;
}

/* `get_asm_sym()` is referenced by subst_asm_operand()'s anonymous-symbol
 * branch (even though our tests avoid that branch, the function body still
 * needs the symbol at link time).  Returning NULL is sufficient: the caller
 * ignores the return value and only needs the symbol table side effect, which
 * is irrelevant in this harness. */
struct Sym;
struct Sym *get_asm_sym(int name, struct Sym *csym)
{
  (void)name;
  (void)csym;
  return NULL;
}

/* `ind` is declared ST_DATA int rsym, anon_sym, ind, loc; in tcc.h.
 * In unit-test builds ST_DATA=extern, so we provide the definition. */
int ind;

/* find_section() is referenced by tccasm.c's section-stack helpers when unit
 * tests exercise use_section/push_section/pop_section.  The main UT binary does
 * not link tccelf.c, so provide a minimal allocator that returns a zeroed
 * Section-sized block.  Tests treat the result as opaque and only read/write
 * the data_offset/prev fields they set up themselves. */
struct Section;
struct TCCState;
struct Section *find_section(struct TCCState *s1, const char *name)
{
  struct Section *sec;
  (void)s1;
  (void)name;
  sec = (struct Section *)tcc_mallocz(1024);
  return sec;
}

/* tok_str_free() is referenced by tccasm.c's asm_macros_free.  The main UT
 * binary does not link tccpp.c; unit tests only hand asm_macros_free simple
 * malloc'd TokenString shells, so a plain wrapper is enough. */
struct TokenString;
void tok_str_free(struct TokenString *s)
{
  tcc_free(s);
}

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

/* put_elf_sym is declared in tcc.h; tccdbg.c uses it for DWARF section symbols.
 * Unit tests don't emit ELF, so return a deterministic symbol index derived
 * from the section number. */
int put_elf_sym(struct Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  (void)value;
  (void)size;
  (void)info;
  (void)other;
  (void)name;
  /* Keep the Section pointer alive for the caller so it can verify the right
   * section was passed; the return value is arbitrary but deterministic. */
  (void)s;
  return shndx + 1;
}

/* get_tok_str is declared `const char *get_tok_str(int, CValue*)` in tcc.h and
 * used by name-gated optimizer passes (e.g. self_copy_elim, float_narrowing).
 * The unit-test harness lets individual tests populate a token→name table so
 * those passes can reach their positive folds.  CValue is opaque here (no tcc.h),
 * hence the void* parameter — the linker resolves by name regardless. */

/* Must be large enough to hold TOK_IDENT-relative tokens used by tests
 * (e.g. TOK_IDENT + 101 in test_opt_licm.c); TOK_IDENT itself is 256, so
 * 256 alone truncated every "TOK_IDENT + N" test token to out-of-range. */
#define UTB_TOKEN_BASE 256
#define UTB_MAX_TOK 1024
static const char *utb_tok_names[UTB_MAX_TOK];
static int utb_next_tok = 512;

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

/* From arm-thumb-gen.c: tcc_gen_machine_number_of_registers,
 * tcc_get_abi_softcall_name. Split into stubs_gen_machine_fallback.c (linked
 * here, but NOT into the backend/ binary, which links the real
 * arm-thumb-gen.c and would otherwise get a multiple-definition error for
 * both) -- see that file. */

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

struct SValue;

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

/* opt_dce.c's volatile-vreg checks (ir_opt_param_vreg_is_volatile,
 * ir_opt_vreg_sym_is_volatile) walk local_stack when tcc_state->ir is unset.
 * Hand-built IR has no frontend symbol table, so an empty list is correct:
 * the walk finds nothing and the vreg is reported non-volatile. */
struct Sym *local_stack = NULL;

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
  int i;
  for (i = UTB_TOKEN_BASE; i < UTB_MAX_TOK; i++)
  {
    if (utb_tok_names[i] && strcmp(utb_tok_names[i], str) == 0)
      return i;
  }
  if (utb_next_tok >= UTB_MAX_TOK)
    return 0;
  utb_tok_names[utb_next_tok] = tcc_strdup(str);
  return utb_next_tok++;
}

typedef struct UtbTokenSym
{
  struct UtbTokenSym *hash_next;
  void *sym_define;
  void *sym_label;
  void *sym_struct;
  void *sym_identifier;
  int tok;
  int len;
  char str[1];
} UtbTokenSym;

void *tok_alloc(const char *str, int len)
{
  int tok;
  UtbTokenSym *ts;
  char buf[128];
  if (len < 0)
    len = (int)strlen(str);
  if ((unsigned)len >= sizeof(buf))
    len = (int)sizeof(buf) - 1;
  memcpy(buf, str, len);
  buf[len] = '\0';
  tok = tok_alloc_const(buf);
  ts = (UtbTokenSym *)tcc_mallocz(sizeof(*ts) + (unsigned)len);
  ts->tok = tok;
  ts->len = len;
  memcpy(ts->str, buf, (unsigned)len + 1);
  return ts;
}

/* opt_dce.c (pulled in by the cmpfold suite) calls elfsym() on callee symbols.
 * Hand-built IR has no real ELF symbols, so return NULL. */
void *elfsym(void *s)
{
  (void)s;
  return 0;
}

/* opt_memory.c's entry_store_prop calls read32le() on rodata bytes when
 * expanding a BLOCK_COPY's constant source; hand-built IR tests don't exercise
 * that path but the linker still needs the symbol (--gc-sections keeps it
 * reachable from the pass entry point). Same little-endian semantics as the
 * real tcctools.c definition. */
uint32_t read32le(unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ir/codegen.c's tcc_ir_codegen_test_gen() calls gv(RC_INT) on the
 * VT_BITFIELD-typed-vtop path (extracting a bit-field before testing it for
 * zero). That branch is unreachable for every test in this harness --
 * svalue_init() zero-inits SValue.type.t and no test constructs a
 * VT_BITFIELD-typed vtop entry -- but the call site is compiled
 * unconditionally, so the linker still needs the symbol. Trap loudly (like
 * _tcc_error above) rather than silently faking a register: if this is ever
 * actually invoked it means a test exercises a path this stub layer doesn't
 * support, and a silent wrong-value return would be worse than a crash. */
int gv(int rc)
{
  (void)rc;
  fprintf(stderr, "[test stub] gv: unexpectedly called (VT_BITFIELD test-gen "
                   "path is not supported by this harness)\n");
  abort();
}

#else /* UT_SSA_OPT_REAL — build_ssaopt (UT11) ===================== */
/*
 *  stubs.c - stubs for the SSA optimizer test binary
 *
 *  Provides stubs for functions that are not needed in the isolated unit
 *  test environment.
 *
 *  Note: tcc_malloc, tcc_free, tcc_realloc, tcc_strdup, _tcc_error,
 *  _tcc_warning are all provided by libtcc.c which is linked into this
 *  binary.
 */

#include <stddef.h>

struct CType;

/* Settable token→name table so name-gated passes (ssa:narrow's demotion
 * fold) can be driven from unit tests.  get_tok_str() itself lives in
 * elfsec_stubs.c and consults this table via utb_tok_name_lookup(). */
#define UTB_TOKEN_BASE 256
#define UTB_MAX_TOK 1024
static const char *utb_tok_names[UTB_MAX_TOK];

void utb_set_tok_str(int tok, const char *name)
{
  if (tok >= 0 && tok < UTB_MAX_TOK)
    utb_tok_names[tok] = name;
}

const char *utb_tok_name_lookup(int tok)
{
  if (tok >= 0 && tok < UTB_MAX_TOK)
    return utb_tok_names[tok];
  return NULL;
}

/* Frontend symbol-table stubs referenced by opt_utils.c's change_callee_sym
 * (pulled in via ssa:narrow).  Returning NULL makes the rename decline
 * safely — the positive callee swap is covered by ir_tests instead. */
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
#endif /* UT_SSA_OPT_REAL */

/* tccgen's current-function name, read by narrow.c's float-demote
 * self-call guard.  Only the ssaopt build needs it from here: the main and
 * backend builds link ra_link_stubs.c, which already defines it (defining
 * it in both files is a multiple-definition link error under -fno-common). */
#ifdef UT_SSA_OPT_REAL
const char *funcname;
#endif
