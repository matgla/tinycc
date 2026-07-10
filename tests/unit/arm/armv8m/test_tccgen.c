/*
 *  test_tccgen.c - suite for tccgen.c
 *
 *  This TU #includes the real tccgen.c at the bottom (the test_tccdbg.c /
 *  test_tccasm.c pattern) so the suite can reach tccgen.c's file-local
 *  `static` helpers, not just its ST_FUNC surface.  USING_GLOBALS must be
 *  defined before tcc.h so TCC_STATE_VAR(x) expands to tcc_state->x (the mode
 *  tccgen.c is written for); the stub layer below satisfies the IR/ELF/
 *  preprocessor symbols tccgen.c references but that live in other modules.
 */

#define USING_GLOBALS
#include "tcc.h"
#include "ut.h"

/* The module under test, pulled into this TU so its file-local statics are
 * reachable.  Everything below (stubs, helpers, tests) sees tccgen.c's real
 * globals and prototypes. */
#include "tccgen.c"

/* Minimal stubs for symbols referenced by vstack helpers in tccgen.c.
   The dedicated tccgen binary links only tccgen.c + the harness, so these
   definitions satisfy the linker.  The tests are written so the error/IR
   paths are never actually executed. */

struct TCCState *tcc_state = NULL;

NORETURN void expect(const char *msg)
{
  (void)msg;
  abort();
}

/* tcc.h renames _tcc_error to use_tcc_error_noabort unless USING_GLOBALS is
   defined (which is only true inside tccgen.c).  Undefine it here so the stub
   matches the symbol tccgen.c actually references. */
#ifdef _tcc_error
#undef _tcc_error
#endif

NORETURN void _tcc_error(const char *fmt, ...)
{
  (void)fmt;
  abort();
}

void tcc_ir_codegen_cmp_jmp_set(struct TCCIRState *ir)
{
  (void)ir;
}

void tcc_ir_backpatch_to_here(struct TCCIRState *ir, int t)
{
  (void)ir;
  (void)t;
}

/* ---------------------------------------------------------------------------
 * IR / ELF / diagnostic stubs pulled in by sym_push, sym_pop, label_* and
 * external_global_sym.  The isolated tccgen binary links only tccgen.c, so
 * these satisfy the linker.  The symbol-stack tests below never take the vreg
 * (VT_PARAM / local-lvalue) or ELF-emission code paths: sym_push's vreg
 * allocators return -1 (skipped via the `vreg >= 0` guards) and the rest are
 * inert no-ops.
 * --------------------------------------------------------------------------- */

int tcc_ir_get_vreg_param(TCCIRState *ir)
{
  (void)ir;
  return -1;
}

int tcc_ir_get_vreg_var(TCCIRState *ir)
{
  (void)ir;
  return -1;
}

IRLiveInterval *tcc_ir_vreg_live_interval(TCCIRState *ir, int vreg)
{
  (void)ir;
  (void)vreg;
  return NULL;
}

void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1)
{
  (void)ir;
  (void)vreg;
  (void)offset;
  (void)r0;
  (void)r1;
}

void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset)
{
  (void)ir;
  (void)vreg;
  (void)offset;
}

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double)
{
  (void)ir;
  (void)vreg;
  (void)is_float;
  (void)is_double;
}

void tcc_ir_vreg_type_set_complex(TCCIRState *ir, int vreg)
{
  (void)ir;
  (void)vreg;
}

void tcc_ir_set_llong_type(TCCIRState *ir, int vreg)
{
  (void)ir;
  (void)vreg;
}

int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx,
                const char *name)
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

void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol,
                    addr_t addend)
{
  (void)symtab;
  (void)s;
  (void)offset;
  (void)type;
  (void)symbol;
  (void)addend;
}

void tcc_ir_gen_f(TCCIRState *ir, int op)
{
  (void)ir;
  (void)op;
}

/* Declared in tccgen.c, not tcc.h. */
void gen_negf(int op);

void tcc_debug_extern_sym(TCCState *s1, Sym *sym, int sh_num, int sym_bind, int sym_type)
{
  (void)s1;
  (void)sym;
  (void)sh_num;
  (void)sym_bind;
  (void)sym_type;
}

char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
  if (buf_size > 0)
  {
    size_t l = strlen(s);
    if (l >= buf_size)
      l = buf_size - 1;
    memcpy(buf, s, l);
    buf[l] = '\0';
  }
  return buf;
}

/* Mirror of the real pstrcat (tccpp.c) — appends with truncation. Needed by
   type_to_str(), which the real module gets from tccpp.c/libtcc.c. */
char *pstrcat(char *buf, size_t buf_size, const char *s)
{
  size_t len = strlen(buf);
  if (len < buf_size)
    pstrcpy(buf + len, buf_size - len, s);
  return buf;
}

const char *get_tok_str(int v, CValue *cv)
{
  (void)v;
  (void)cv;
  return "?";
}

void _tcc_warning(const char *fmt, ...)
{
  (void)fmt;
}

/* Minimal tok_get: the real one (tccpp.c) isn't linked here.  The
   inline_body_has_* scanners only feed it single-int (value-less) tokens, so
   advancing one int per call is a faithful stand-in for those tests. */
ST_FUNC void tok_get(int *t, const int **pp, CValue *cv)
{
  const int *p = *pp;
  (void)cv;
  *t = *p++;
  *pp = p;
}

/* Runtime-only fallbacks kept reachable under --gc-sections but never executed
   by the tests: gen_inline_abs_from_vtop()'s non-constant helper path calls
   tok_alloc_const (the abs tests all fold constants), and vla_restore()'s
   non-IR path calls gen_vla_sp_restore (the VLA tests always take the IR
   branch).  Inert link satisfiers; real definitions live in tccpp.c /
   arm-thumb-gen.c. */
int tok_alloc_const(const char *str)
{
  (void)str;
  return 0;
}
void gen_vla_sp_restore(int addr) { (void)addr; }

/* ---------------------------------------------------------------------------
 * gen_op / gen_cast link cascade.  gen_opic/gen_opif/gen_cast fold constant
 * operands arithmetically in place, WITHOUT touching the IR pool or backend —
 * so those (valuable) paths are testable.  --gc-sections still keeps the
 * non-constant IR-emission / section code alive, so the stubs below just
 * satisfy the linker; they are never reached on the constant-fold paths the
 * tests exercise.  read/write*le are real little-endian (constant folds of
 * _Complex / vector literals read pool bytes through them); is_power_of_2 is
 * real (strength-reduction folds depend on it).
 * --------------------------------------------------------------------------- */
uint16_t read16le(unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
void write16le(unsigned char *p, uint16_t x)
{
  p[0] = (unsigned char)x;
  p[1] = (unsigned char)(x >> 8);
}
uint32_t read32le(unsigned char *p) { return read16le(p) | (uint32_t)read16le(p + 2) << 16; }
void write32le(unsigned char *p, uint32_t x)
{
  write16le(p, (uint16_t)x);
  write16le(p + 2, (uint16_t)(x >> 16));
}
uint64_t read64le(unsigned char *p) { return read32le(p) | (uint64_t)read32le(p + 4) << 32; }
void write64le(unsigned char *p, uint64_t x)
{
  write32le(p, (uint32_t)x);
  write32le(p + 4, (uint32_t)(x >> 32));
}

int is_power_of_2(int64_t n) { return n > 0 && (n & (n - 1)) == 0; }

/* Data tables referenced only by unexecuted cascade paths (zero-initialised). */
const int reg_classes[NB_REGS];
const IRRegistersConfig irop_config[512];

int irop_btype_to_vt_btype(int irop_btype) { return irop_btype; }
int64_t *tcc_ir_pool_get_i64_ptr(const struct TCCIRState *ir, uint32_t idx)
{
  (void)ir;
  (void)idx;
  return NULL;
}
uint64_t *tcc_ir_pool_get_f64_ptr(const struct TCCIRState *ir, uint32_t idx)
{
  (void)ir;
  (void)idx;
  return NULL;
}
int tcc_ir_get_vreg_temp(TCCIRState *ir)
{
  (void)ir;
  return -1;
}
void tcc_ir_gen_i(struct TCCIRState *ir, int op)
{
  (void)ir;
  (void)op;
}
int tcc_ir_find_defining_instruction(struct TCCIRState *ir, int32_t vreg, int before_idx)
{
  (void)ir;
  (void)vreg;
  (void)before_idx;
  return -1;
}
SValue svalue_call_id_argc(int call_id, int argc)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  (void)call_id;
  (void)argc;
  return sv;
}
size_t section_add(Section *sec, addr_t size, int align)
{
  (void)sec;
  (void)size;
  (void)align;
  return 0;
}

/* ---------------------------------------------------------------------------
 * Minimal host-side runtime for the subset of tccgen.c exercised below.
 *
 * The tccgen unit-test binary links only tccgen.c itself; libtcc.c,
 * tccpp.c and the IR core live in other unit-test binaries.  The stubs
 * below are enough to exercise the symbol-stack, vstack and type helpers
 * without dragging in the full compiler.
 * --------------------------------------------------------------------------- */

/* Bump allocator used by the symbol allocator in tccgen.c.  We avoid the
   libc malloc/free names because tcc.h redefines them to use_tcc_*. */
static char ut_mem_pool[262144];
static size_t ut_mem_used = 0;

static void *ut_pool_alloc(unsigned long size)
{
  void *p;
  size = (size + 7) & ~7UL;
  if (ut_mem_used + size > sizeof(ut_mem_pool))
    abort();
  p = ut_mem_pool + ut_mem_used;
  ut_mem_used += size;
  return p;
}

void *tcc_malloc(unsigned long size) { return ut_pool_alloc(size); }
void *tcc_mallocz(unsigned long size)
{
  void *p = ut_pool_alloc(size);
  memset(p, 0, size);
  return p;
}
void *tcc_realloc(void *ptr, unsigned long size)
{
  void *p = ut_pool_alloc(size);
  if (ptr)
    memcpy(p, ptr, size);
  return p;
}
void tcc_free(void *ptr) { (void)ptr; }

void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
  int n = *nb_ptr;
  void ***tab = ptab;
  *tab = tcc_realloc(*tab, (n + 1) * sizeof(void *));
  (*tab)[n] = data;
  *nb_ptr = n + 1;
}

/* Dummy TCCState for paths that dereference tcc_state->ir but never use it. */
static struct TCCState ut_dummy_state;

/* Dummy ELF symbol table so update_storage()/elfsym() can be exercised. */
static ElfSym ut_symtab_data[4];
static Section ut_symtab_section;

/* Token table used by symbol helpers.  The real table lives in tccpp.c;
   provide the storage here so sym_push/sym_pop/label_* resolve at link time. */
int tok_ident;
TokenSym **table_ident;

#define UT_TABLE_SIZE 16
static TokenSym *ut_table[UT_TABLE_SIZE];
static TokenSym ut_tok_a;
static TokenSym ut_tok_b;

static void reset_token_table(void)
{
  memset(ut_table, 0, sizeof(ut_table));
  memset(&ut_tok_a, 0, sizeof(ut_tok_a));
  memset(&ut_tok_b, 0, sizeof(ut_tok_b));
  table_ident = ut_table;
  tok_ident = TOK_IDENT + UT_TABLE_SIZE;
  ut_table[1] = &ut_tok_a;
  ut_table[2] = &ut_tok_b;
}

TokenSym *tok_ensure(int v)
{
  if (v >= TOK_IDENT && (v - TOK_IDENT) < UT_TABLE_SIZE)
    return ut_table[v - TOK_IDENT];
  return NULL;
}

static void reset_symbol_state(void)
{
  reset_token_table();
  global_stack = NULL;
  local_stack = NULL;
  anon_sym = SYM_FIRST_ANOM;
}

static void reset_vstack(void)
{
  memset(_vstack, 0, sizeof(SValue) * (1 + VSTACK_SIZE));
  vtop = _vstack;
  /* Suppress IR emission from vcheck_cmp(): it calls
     tcc_ir_codegen_cmp_jmp_set() when nocode_wanted has no bits other than
     CODE_OFF_BIT.  Keep a non-CODE_OFF_BIT flag set so unit tests can
     exercise the vstack helpers in isolation. */
  nocode_wanted = 0x40000000;
}

static CType simple_type(int t)
{
  CType type;
  type.t = t;
  type.ref = NULL;
  return type;
}

static Sym sym_for_type(CType type, int c, int r)
{
  Sym sym;
  memset(&sym, 0, sizeof(sym));
  sym.type = type;
  sym.c = c;
  sym.r = (unsigned short)r;
  return sym;
}

/* Wrap a caller-owned NUL-terminated int[] as a heap-backed TokenString so
   tok_str_buf() returns it (allocated_len > 0 selects data.str). */
static TokenString make_tokstr(int *toks)
{
  TokenString ts;
  memset(&ts, 0, sizeof(ts));
  ts.allocated_len = 64;
  ts.data.str = toks;
  return ts;
}

static int assert_type_size(CType type, int expected_size,
                            int expected_align)
{
  int align = -1;
  int size = type_size(&type, &align);
  UT_ASSERT_EQ(size, expected_size);
  UT_ASSERT_EQ(align, expected_align);
  return 0;
}

UT_TEST(test_type_size_scalar_armv8m_abi)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_VOID), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_BOOL), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_BYTE), 1, 1), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_SHORT), 2, 2), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_INT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LONG | VT_INT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LLONG), 8, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FLOAT), 4, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_DOUBLE), 8, 8), 0);
  return 0;
}

UT_TEST(test_type_size_complex_types)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FLOAT | VT_COMPLEX), 8, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_DOUBLE | VT_COMPLEX), 16, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_INT | VT_COMPLEX), 8, 4), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_SHORT | VT_COMPLEX), 4, 2), 0);
  return 0;
}

UT_TEST(test_type_size_pointer_and_array)
{
  CType elem = simple_type(VT_SHORT);
  Sym elem_ref = sym_for_type(elem, 7, 0);
  CType array_type = simple_type(VT_PTR | VT_ARRAY);
  CType ptr_type = simple_type(VT_PTR);

  array_type.ref = &elem_ref;
  UT_ASSERT_EQ(assert_type_size(array_type, 14, 2), 0);

  ptr_type.ref = &elem_ref;
  UT_ASSERT_EQ(assert_type_size(ptr_type, PTR_SIZE, PTR_SIZE), 0);
  return 0;
}

UT_TEST(test_type_size_struct_and_incomplete_enum)
{
  CType struct_type = simple_type(VT_STRUCT);
  Sym struct_ref = sym_for_type(simple_type(VT_INT), 12, 4);
  CType enum_type = simple_type(VT_ENUM | VT_INT);
  Sym enum_ref = sym_for_type(simple_type(VT_INT), -1, 0);

  struct_type.ref = &struct_ref;
  UT_ASSERT_EQ(assert_type_size(struct_type, 12, 4), 0);

  enum_type.ref = &enum_ref;
  UT_ASSERT_EQ(assert_type_size(enum_type, -1, 0), 0);
  return 0;
}

UT_TEST(test_exact_log2p1_alignment_encoding)
{
  UT_ASSERT_EQ(exact_log2p1(0), 0);
  UT_ASSERT_EQ(exact_log2p1(1), 1);
  UT_ASSERT_EQ(exact_log2p1(2), 2);
  UT_ASSERT_EQ(exact_log2p1(4), 3);
  UT_ASSERT_EQ(exact_log2p1(8), 4);
  UT_ASSERT_EQ(exact_log2p1(16), 5);
  UT_ASSERT_EQ(exact_log2p1(256), 9);
  return 0;
}

UT_TEST(test_exact_log2p1_non_powers_and_large)
{
  UT_ASSERT_EQ(exact_log2p1(3), 2);
  UT_ASSERT_EQ(exact_log2p1(5), 3);
  UT_ASSERT_EQ(exact_log2p1(6), 3);
  UT_ASSERT_EQ(exact_log2p1(7), 3);
  UT_ASSERT_EQ(exact_log2p1(0x100), 9);
  UT_ASSERT_EQ(exact_log2p1(0x10000), 17);
  UT_ASSERT_EQ(exact_log2p1(0x40000000), 31);
  return 0;
}

UT_TEST(test_type_size_ldouble_qlong_qfloat)
{
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_LDOUBLE), 8, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_QLONG), 16, 8), 0);
  UT_ASSERT_EQ(assert_type_size(simple_type(VT_QFLOAT), 16, 8), 0);
  return 0;
}

UT_TEST(test_type_size_complete_enum_and_func)
{
  CType enum_type = simple_type(VT_ENUM | VT_INT);
  Sym enum_ref = sym_for_type(simple_type(VT_INT), 0, 0);

  enum_type.ref = &enum_ref;
  UT_ASSERT_EQ(assert_type_size(enum_type, 4, 4), 0);

  UT_ASSERT_EQ(assert_type_size(simple_type(VT_FUNC), 1, 1), 0);
  return 0;
}

UT_TEST(test_is_float_recognizes_fp_btypes)
{
  UT_ASSERT(is_float(VT_FLOAT));
  UT_ASSERT(is_float(VT_DOUBLE));
  UT_ASSERT(is_float(VT_LDOUBLE));
  UT_ASSERT(is_float(VT_QFLOAT));
  UT_ASSERT(!is_float(VT_INT));
  UT_ASSERT(!is_float(VT_BYTE));
  UT_ASSERT(!is_float(VT_SHORT));
  UT_ASSERT(!is_float(VT_LLONG));
  UT_ASSERT(!is_float(VT_PTR));
  UT_ASSERT(!is_float(VT_STRUCT));
  UT_ASSERT(!is_float(VT_BOOL));
  return 0;
}

UT_TEST(test_get_value_type_returns_null)
{
  UT_ASSERT(get_value_type(0) == NULL);
  UT_ASSERT(get_value_type(VT_LOCAL) == NULL);
  UT_ASSERT(get_value_type(VT_CONST) == NULL);
  return 0;
}

UT_TEST(test_ieee_finite)
{
  UT_ASSERT(ieee_finite(0.0));
  UT_ASSERT(ieee_finite(-0.0));
  UT_ASSERT(ieee_finite(1.0));
  UT_ASSERT(ieee_finite(-1.0));
  UT_ASSERT(ieee_finite(1.5));
  UT_ASSERT(!ieee_finite(1.0 / 0.0));
  UT_ASSERT(!ieee_finite(-1.0 / 0.0));
  UT_ASSERT(!ieee_finite(0.0 / 0.0));
  return 0;
}

UT_TEST(test_vpushi_vpop)
{
  reset_vstack();
  vpushi(42);
  UT_ASSERT_EQ(vtop->c.i, 42);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_INT);
  vpop();
  UT_ASSERT(vtop == _vstack);
  return 0;
}

UT_TEST(test_vset)
{
  CType t = simple_type(VT_SHORT);

  reset_vstack();
  vset(&t, VT_CONST, -123);
  UT_ASSERT_EQ(vtop->c.i, -123);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_SHORT);
  return 0;
}

UT_TEST(test_vpushv)
{
  SValue sv;
  CType t = simple_type(VT_INT);

  reset_vstack();
  memset(&sv, 0, sizeof(sv));
  sv.type = t;
  sv.r = VT_CONST;
  sv.c.i = 999;
  vpushv(&sv);
  UT_ASSERT_EQ(vtop->c.i, 999);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_INT);
  return 0;
}

UT_TEST(test_vswap)
{
  reset_vstack();
  vpushi(10);
  vpushi(20);
  vswap();
  UT_ASSERT_EQ(vtop[0].c.i, 10);
  UT_ASSERT_EQ(vtop[-1].c.i, 20);
  return 0;
}

UT_TEST(test_vrotb)
{
  reset_vstack();
  vpushi(1);
  vpushi(2);
  vpushi(3);
  vrotb(3);
  UT_ASSERT_EQ(vtop[-2].c.i, 2);
  UT_ASSERT_EQ(vtop[-1].c.i, 3);
  UT_ASSERT_EQ(vtop[0].c.i, 1);
  return 0;
}

UT_TEST(test_vrott)
{
  reset_vstack();
  vpushi(1);
  vpushi(2);
  vpushi(3);
  vrott(3);
  UT_ASSERT_EQ(vtop[-2].c.i, 3);
  UT_ASSERT_EQ(vtop[-1].c.i, 1);
  UT_ASSERT_EQ(vtop[0].c.i, 2);
  return 0;
}

UT_TEST(test_vrev)
{
  reset_vstack();
  vpushi(1);
  vpushi(2);
  vpushi(3);
  vpushi(4);
  vrev(4);
  UT_ASSERT_EQ(vtop[-3].c.i, 4);
  UT_ASSERT_EQ(vtop[-2].c.i, 3);
  UT_ASSERT_EQ(vtop[-1].c.i, 2);
  UT_ASSERT_EQ(vtop[0].c.i, 1);
  return 0;
}

UT_TEST(test_vset_VT_CMP)
{
  reset_vstack();
  vpushi(0);
  vset_VT_CMP(TOK_NE);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CMP);
  UT_ASSERT_EQ(vtop->cmp_op, TOK_NE);
  UT_ASSERT_EQ(vtop->jtrue, -1);
  UT_ASSERT_EQ(vtop->jfalse, -1);
  return 0;
}

UT_TEST(test_check_vstack_empty)
{
  reset_vstack();
  check_vstack();
  return 0;
}

UT_TEST(test_test_lvalue_ok)
{
  reset_vstack();
  vpushi(0);
  vtop->r |= VT_LVAL;
  test_lvalue();
  return 0;
}

UT_TEST(test_exact_log2p1_zero_and_max_int)
{
  UT_ASSERT_EQ(exact_log2p1(0), 0);
  UT_ASSERT_EQ(exact_log2p1(0x7fffffff), 31);
  return 0;
}

UT_TEST(test_ieee_finite_subnormal)
{
  /* Smallest positive double subnormal. */
  UT_ASSERT(ieee_finite(4.9e-324));
  return 0;
}

UT_TEST(test_type_size_array_of_incomplete_enum)
{
  /* Array of an incomplete enum: element size is negative, count is negative.
     type_size() negates the element size before multiplying (tccgen.c:10104). */

  /* The element is an incomplete enum, so its own ref sym carries c < 0;
     type_size() returns -1 for the element itself (tccgen.c:10113). */
  Sym enum_ref = sym_for_type(simple_type(VT_INT), -1, 0);
  CType elem = simple_type(VT_ENUM | VT_INT);
  elem.ref = &enum_ref;

  /* The array's ref sym holds the element type in .type and the (negative)
     element count in .c. */
  Sym array_ref = sym_for_type(elem, -1, 0);
  CType array_type = simple_type(VT_PTR | VT_ARRAY);
  array_type.ref = &array_ref;

  UT_ASSERT_EQ(assert_type_size(array_type, -1, 0), 0);
  return 0;
}

UT_TEST(test_vpop_with_cmp_backpatch)
{
  /* vpop() has a separate path for VT_CMP values that back-patches jump
     chains via tcc_ir_backpatch_to_here(), which reads tcc_state->ir. */
  reset_vstack();
  tcc_state = &ut_dummy_state;
  vpushi(0);
  vtop->r = VT_CMP;
  vtop->jtrue = 0;
  vtop->jfalse = 1;
  vpop();
  UT_ASSERT(vtop == _vstack);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_vrotb_vrott_noop_for_small_n)
{
  /* vrotb/vrott short-circuit when n <= 1. */
  reset_vstack();
  vpushi(1);
  vpushi(2);
  vrotb(1);
  UT_ASSERT_EQ(vtop[0].c.i, 2);
  UT_ASSERT_EQ(vtop[-1].c.i, 1);
  vrott(1);
  UT_ASSERT_EQ(vtop[0].c.i, 2);
  UT_ASSERT_EQ(vtop[-1].c.i, 1);
  vrotb(0);
  vrott(0);
  return 0;
}

UT_TEST(test_vsetc_triggers_vcheck_cmp_when_code_on)
{
  /* When nocode_wanted is clear, vcheck_cmp() calls the IR hook. */
  CType t = simple_type(VT_INT);

  reset_vstack();
  nocode_wanted = 0;
  tcc_state = &ut_dummy_state;
  vset(&t, VT_CONST, 55);
  UT_ASSERT_EQ(vtop->c.i, 55);

  nocode_wanted = 0x40000000;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_vpushsym)
{
  CType t = simple_type(VT_INT);
  Sym s = sym_for_type(t, 0, 0);

  reset_vstack();
  vpushsym(&t, &s);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT(vtop->r & VT_SYM);
  UT_ASSERT_EQ(vtop->sym, &s);
  return 0;
}

UT_TEST(test_sym_push2_and_find2)
{
  reset_token_table();
  Sym *s = sym_push2(&global_stack, TOK_IDENT + 1, VT_INT, 42);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->v, TOK_IDENT + 1);
  UT_ASSERT_EQ(s->c, 42);
  UT_ASSERT_EQ(sym_find2(global_stack, TOK_IDENT + 1), s);
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_sym_push_and_sym_find)
{
  CType t = simple_type(VT_INT);
  reset_token_table();
  Sym *s = sym_push(TOK_IDENT + 1, &t, VT_CONST, 123);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(sym_find(TOK_IDENT + 1), s);
  UT_ASSERT_EQ(s->r, VT_CONST);
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_struct_find)
{
  CType t = simple_type(VT_STRUCT);
  reset_token_table();
  Sym *s = sym_push(SYM_STRUCT | (TOK_IDENT + 1), &t, 0, 1);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(struct_find(TOK_IDENT + 1), s);
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_label_push_pop_and_find)
{
  reset_token_table();
  Sym *s = label_push(&local_label_stack, TOK_IDENT + 1, LABEL_DEFINED);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(label_find(TOK_IDENT + 1), s);
  label_pop(&local_label_stack, NULL, 0);
  UT_ASSERT(label_find(TOK_IDENT + 1) == NULL);
  return 0;
}

UT_TEST(test_mk_pointer_creates_ref_sym)
{
  CType t = simple_type(VT_INT | VT_UNSIGNED);
  reset_token_table();
  mk_pointer(&t);
  UT_ASSERT_EQ(t.t & VT_BTYPE, VT_PTR);
  UT_ASSERT(t.ref != NULL);
  UT_ASSERT_EQ(t.ref->type.t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ(t.ref->c, -1);
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_vpush_typed_helper_func)
{
  CType t = simple_type(VT_INT);
  reset_token_table();
  reset_vstack();
  vpush_typed_helper_func(TOK_IDENT + 1, &t);
  UT_ASSERT(vtop->sym != NULL);
  UT_ASSERT_EQ(vtop->sym->v, TOK_IDENT + 1);
  UT_ASSERT_EQ(vtop->sym->type.t, (VT_INT | VT_EXTERN));
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT(vtop->r & VT_SYM);

  /* Avoid the c==0 path in sym_pop(), which would dereference tcc_state->ir. */
  vtop->sym->c = 1;
  vpop();
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_elfsym_invalid)
{
  Sym s;

  memset(&s, 0, sizeof(s));
  UT_ASSERT(elfsym(NULL) == NULL);
  UT_ASSERT(elfsym(&s) == NULL);
  s.c = -1;
  UT_ASSERT(elfsym(&s) == NULL);
  return 0;
}

UT_TEST(test_update_storage_with_esym)
{
  Sym s;
  struct TCCState *s1 = &ut_dummy_state;

  memset(ut_symtab_data, 0, sizeof(ut_symtab_data));
  ut_symtab_section.data = (unsigned char *)ut_symtab_data;
  tcc_state = s1;
  symtab_section = &ut_symtab_section;

  /* Static symbol: bind should become LOCAL. */
  memset(&s, 0, sizeof(s));
  s.c = 1;
  s.type.t = VT_INT | VT_STATIC;
  s.a.visibility = STV_HIDDEN;
  ut_symtab_data[1].st_info = ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT);
  update_storage(&s);
  UT_ASSERT_EQ(ELFW(ST_BIND)(ut_symtab_data[1].st_info), STB_LOCAL);
  UT_ASSERT_EQ((ut_symtab_data[1].st_other & 3), STV_HIDDEN);

  /* Weak non-static symbol: bind should become WEAK. */
  memset(&s, 0, sizeof(s));
  s.c = 2;
  s.type.t = VT_INT;
  s.a.weak = 1;
  ut_symtab_data[2].st_info = ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT);
  update_storage(&s);
  UT_ASSERT_EQ(ELFW(ST_BIND)(ut_symtab_data[2].st_info), STB_WEAK);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_skips_invalid_v)
{
  Sym s;

  tcc_state = &ut_dummy_state;

  memset(&s, 0, sizeof(s));
  s.v = 0;
  s.type.t = VT_INT;
  put_extern_sym2(&s, SHN_UNDEF, 0, 4, 1);
  UT_ASSERT_EQ(s.c, 0);

  memset(&s, 0, sizeof(s));
  s.v = 0xDEADBEEF;
  s.type.t = VT_INT;
  put_extern_sym2(&s, SHN_UNDEF, 0, 4, 1);
  UT_ASSERT_EQ(s.c, 0);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_creates_object_symbol)
{
  Sym s;

  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_INT;
  tcc_state = &ut_dummy_state;
  put_extern_sym2(&s, SHN_UNDEF, 0x100, 4, 1);
  UT_ASSERT_EQ(s.c, 0); /* stub put_elf_sym() always returns 0 */
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_func_and_asm_types)
{
  Sym s;

  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_FUNC;
  tcc_state = &ut_dummy_state;
  put_extern_sym2(&s, SHN_UNDEF, 0, 1, 1);
  UT_ASSERT_EQ(s.c, 0);

  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_VOID | VT_ASM_FUNC;
  put_extern_sym2(&s, SHN_UNDEF, 0, 1, 1);
  UT_ASSERT_EQ(s.c, 0);

  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym_respects_nocode_wanted)
{
  Sym s;

  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_INT;
  nocode_wanted = 1;
  tcc_state = &ut_dummy_state;
  put_extern_sym(&s, NULL, 0, 4);
  UT_ASSERT_EQ(s.c, 0);
  nocode_wanted = 0x40000000;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_get_sym_ref)
{
  CType t = simple_type(VT_INT);
  Sym *sym;

  reset_symbol_state();
  UT_ASSERT_EQ(anon_sym, SYM_FIRST_ANOM);
  tcc_state = &ut_dummy_state;
  sym = get_sym_ref(&t, NULL, 0x20, 4);
  UT_ASSERT(sym != NULL);
  UT_ASSERT(sym->type.t & VT_STATIC);
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_external_global_sym_new)
{
  CType t = simple_type(VT_INT);
  Sym *s;

  reset_symbol_state();
  s = external_global_sym(TOK_IDENT + 1, &t);
  UT_ASSERT(s != NULL);
  UT_ASSERT(s->type.t & VT_EXTERN);
  UT_ASSERT_EQ(sym_find(TOK_IDENT + 1), s);
  tcc_state = &ut_dummy_state;
  sym_pop(&global_stack, NULL, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_external_global_sym_reuses_existing)
{
  CType t1 = simple_type(VT_INT);
  CType t2 = simple_type(VT_SHORT);
  Sym *s1, *s2;

  reset_symbol_state();
  s1 = external_global_sym(TOK_IDENT + 1, &t1);
  s2 = external_global_sym(TOK_IDENT + 1, &t2);
  UT_ASSERT_EQ(s1, s2);
  tcc_state = &ut_dummy_state;
  sym_pop(&global_stack, NULL, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_external_helper_sym)
{
  Sym *s;

  reset_symbol_state();
  s = external_helper_sym(TOK_IDENT + 1);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->type.t, (VT_ASM_FUNC | VT_EXTERN));
  tcc_state = &ut_dummy_state;
  sym_pop(&global_stack, NULL, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_vpush_helper_func)
{
  reset_symbol_state();
  reset_vstack();
  vpush_helper_func(TOK_IDENT + 1);
  UT_ASSERT(vtop->sym != NULL);
  UT_ASSERT_EQ(vtop->sym->v, TOK_IDENT + 1);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT(vtop->r & VT_SYM);
  vpop();
  tcc_state = &ut_dummy_state;
  sym_pop(&global_stack, NULL, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_global_identifier_push)
{
  Sym *s;

  reset_symbol_state();
  s = global_identifier_push(TOK_IDENT + 1, VT_INT, 55);
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->v, TOK_IDENT + 1);
  UT_ASSERT_EQ(s->c, 55);
  UT_ASSERT_EQ(s->r & (VT_CONST | VT_SYM), (VT_CONST | VT_SYM));
  UT_ASSERT_EQ(sym_find(TOK_IDENT + 1), s);
  sym_pop(&global_stack, NULL, 0);
  return 0;
}

UT_TEST(test_greloca_and_greloc)
{
  static Section s;
  Sym sym;

  memset(&s, 0, sizeof(s));
  memset(&sym, 0, sizeof(sym));
  reset_symbol_state();
  sym.v = TOK_IDENT + 1;
  sym.type.t = VT_INT;
  sym.c = 2;
  tcc_state = &ut_dummy_state;
  nocode_wanted = 0;
  greloca(&s, &sym, 0x10, 0, 0);
  greloc(&s, &sym, 0x14, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_greloca_skips_under_nocode_wanted)
{
  static Section s;
  Sym sym;
  struct TCCState *s1 = &ut_dummy_state;

  memset(&s, 0, sizeof(s));
  memset(&sym, 0, sizeof(sym));
  tcc_state = s1;
  cur_text_section = &s;
  nocode_wanted = 1;
  greloca(&s, &sym, 0, 0, 0);
  nocode_wanted = 0x40000000;
  cur_text_section = NULL;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_updates_existing_esym)
{
  Sym s;
  struct TCCState *s1 = &ut_dummy_state;

  memset(ut_symtab_data, 0, sizeof(ut_symtab_data));
  ut_symtab_section.data = (unsigned char *)ut_symtab_data;
  tcc_state = s1;
  symtab_section = &ut_symtab_section;

  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_INT;
  s.c = 1;
  put_extern_sym2(&s, 5, 0x200, 8, 1);
  UT_ASSERT_EQ(ut_symtab_data[1].st_value, 0x200);
  UT_ASSERT_EQ(ut_symtab_data[1].st_size, 8);
  UT_ASSERT_EQ(ut_symtab_data[1].st_shndx, 5);

  symtab_section = NULL;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_name_variants)
{
  Sym s;
  struct TCCState *s1 = &ut_dummy_state;

  tcc_state = s1;

  /* asm_label overrides the normal name. */
  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.asm_label = TOK_IDENT + 2;
  s.type.t = VT_INT;
  put_extern_sym2(&s, SHN_UNDEF, 0, 4, 1);
  UT_ASSERT_EQ(s.c, 0);

  /* leading_underscore prepends '_' to the emitted name. */
  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_INT;
  s1->leading_underscore = 1;
  put_extern_sym2(&s, SHN_UNDEF, 0, 4, 1);
  UT_ASSERT_EQ(s.c, 0);
  s1->leading_underscore = 0;

  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym2_debug_mode)
{
  Sym s;

  tcc_state = &ut_dummy_state;
  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1;
  s.type.t = VT_INT;
  debug_modes = 1;
  put_extern_sym2(&s, SHN_UNDEF, 0, 4, 1);
  UT_ASSERT_EQ(s.c, 0);
  debug_modes = 0;
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_external_global_sym_asm_existing)
{
  CType t = simple_type(VT_INT);
  Sym *s;

  reset_symbol_state();
  s = external_helper_sym(TOK_IDENT + 1);
  UT_ASSERT(IS_ASM_SYM(s));
  s = external_global_sym(TOK_IDENT + 1, &t);
  UT_ASSERT_EQ(s->type.t, (VT_INT | VT_EXTERN));
  tcc_state = &ut_dummy_state;
  sym_pop(&global_stack, NULL, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_greloca_null_symbol)
{
  static Section s;

  memset(&s, 0, sizeof(s));
  tcc_state = &ut_dummy_state;
  nocode_wanted = 0;
  greloca(&s, NULL, 0x20, 0, 0);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_put_extern_sym_with_cur_text_section_nocode)
{
  static Section s;
  Sym sym;
  struct TCCState *s1 = &ut_dummy_state;

  memset(&s, 0, sizeof(s));
  memset(&sym, 0, sizeof(sym));
  reset_symbol_state();
  sym.v = TOK_IDENT + 1;
  sym.type.t = VT_INT;
  tcc_state = s1;
  cur_text_section = &s;
  nocode_wanted = 1;
  put_extern_sym(&sym, &s, 0, 4);
  UT_ASSERT_EQ(sym.c, 0);
  nocode_wanted = 0x40000000;
  cur_text_section = NULL;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_gen_negf)
{
  reset_vstack();
  vtop->type.t = VT_FLOAT;
  tcc_state = &ut_dummy_state;
  gen_negf(TOK_NEG);
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_sym_free)
{
  Sym *s;
  static unsigned char dummy_data[4];

  reset_symbol_state();
  s = sym_push2(&global_stack, SYM_FIRST_ANOM, VT_INT, 0);
  UT_ASSERT(s != NULL);
  s->const_init_data = dummy_data;
  sym_free(s);
  global_stack = NULL;
  UT_ASSERT(s->const_init_data == NULL);
  UT_ASSERT_EQ((unsigned int)s->v, 0xDEADBEEFU);
  return 0;
}

UT_TEST(test_is_float_quad_types)
{
  UT_ASSERT(is_float(VT_QFLOAT));
  UT_ASSERT(!is_float(VT_QLONG));
  return 0;
}

/* ===========================================================================
 * Coverage for tccgen.c's file-local `static` helpers, reachable because this
 * TU #includes tccgen.c.  These are pure/leaf routines exercised in isolation
 * (no real preprocessor / IR pipeline): btype & type classification, type
 * compatibility, struct-shape predicates, builtin constant folders, the
 * lower-level vstack pushers, and the switch/scope bookkeeping helpers.
 * ===========================================================================
 */

UT_TEST(test_is_integer_btype)
{
  UT_ASSERT(is_integer_btype(VT_BYTE));
  UT_ASSERT(is_integer_btype(VT_BOOL));
  UT_ASSERT(is_integer_btype(VT_SHORT));
  UT_ASSERT(is_integer_btype(VT_INT));
  UT_ASSERT(is_integer_btype(VT_LLONG));
  UT_ASSERT(!is_integer_btype(VT_FLOAT));
  UT_ASSERT(!is_integer_btype(VT_DOUBLE));
  UT_ASSERT(!is_integer_btype(VT_PTR));
  UT_ASSERT(!is_integer_btype(VT_STRUCT));
  UT_ASSERT(!is_integer_btype(VT_VOID));
  return 0;
}

UT_TEST(test_btype_size)
{
  UT_ASSERT_EQ(btype_size(VT_BYTE), 1);
  UT_ASSERT_EQ(btype_size(VT_BOOL), 1);
  UT_ASSERT_EQ(btype_size(VT_SHORT), 2);
  UT_ASSERT_EQ(btype_size(VT_INT), 4);
  UT_ASSERT_EQ(btype_size(VT_LLONG), 8);
  UT_ASSERT_EQ(btype_size(VT_PTR), PTR_SIZE);
  UT_ASSERT_EQ(btype_size(VT_FLOAT), 0); /* not handled -> 0 */
  UT_ASSERT_EQ(btype_size(VT_VOID), 0);
  return 0;
}

UT_TEST(test_get_int_type_bits)
{
  UT_ASSERT_EQ(get_int_type_bits(), 32);
  return 0;
}

UT_TEST(test_gcc_classify_type)
{
  CType t;
  Sym head = sym_for_type(simple_type(VT_INT), 4, 4);
  CType p;

  t = simple_type(VT_VOID);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_VOID);
  t = simple_type(VT_INT);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_INTEGER);
  t = simple_type(VT_BOOL);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_INTEGER);
  t = simple_type(VT_LLONG);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_INTEGER);
  t = simple_type(VT_FLOAT);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_REAL);
  t = simple_type(VT_DOUBLE);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_REAL);
  t = simple_type(VT_DOUBLE | VT_COMPLEX);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_COMPLEX);
  t = simple_type(VT_FUNC);
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_FUNCTION);

  p = simple_type(VT_PTR);
  UT_ASSERT_EQ(gcc_classify_type(&p), GCC_TYPE_CLASS_POINTER);
  p.t |= VT_ARRAY;
  UT_ASSERT_EQ(gcc_classify_type(&p), GCC_TYPE_CLASS_ARRAY);

  t = simple_type(VT_STRUCT);
  t.ref = &head;
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_STRUCT);
  t = simple_type(VT_UNION);
  t.ref = &head;
  UT_ASSERT_EQ(gcc_classify_type(&t), GCC_TYPE_CLASS_UNION);
  return 0;
}

UT_TEST(test_pointed_type_and_size)
{
  CType elem = simple_type(VT_INT);
  Sym ref = sym_for_type(elem, -1, 0);
  CType ptr = simple_type(VT_PTR);
  ptr.ref = &ref;
  UT_ASSERT_EQ(pointed_type(&ptr)->t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ(pointed_size(&ptr), 4);
  return 0;
}

UT_TEST(test_is_null_pointer)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));

  sv.type = simple_type(VT_INT);
  sv.r = VT_CONST;
  sv.c.i = 0;
  UT_ASSERT(is_null_pointer(&sv));
  sv.c.i = 5;
  UT_ASSERT(!is_null_pointer(&sv));

  /* non-constant r never a null pointer */
  sv.c.i = 0;
  sv.r = VT_LOCAL;
  UT_ASSERT(!is_null_pointer(&sv));

  /* long long zero is a null pointer constant */
  sv.r = VT_CONST;
  sv.type = simple_type(VT_LLONG);
  sv.c.i = 0;
  UT_ASSERT(is_null_pointer(&sv));
  return 0;
}

UT_TEST(test_compare_types_scalars)
{
  CType a = simple_type(VT_INT);
  CType b = simple_type(VT_INT);
  CType c = simple_type(VT_SHORT);
  CType cq = simple_type(VT_INT | VT_CONSTANT);

  UT_ASSERT(is_compatible_types(&a, &b));
  UT_ASSERT(!is_compatible_types(&a, &c));
  /* qualifiers matter for is_compatible_types, ignored for unqualified */
  UT_ASSERT(!is_compatible_types(&a, &cq));
  UT_ASSERT(is_compatible_unqualified_types(&a, &cq));
  return 0;
}

UT_TEST(test_compare_types_pointer_and_struct)
{
  CType elem = simple_type(VT_INT);
  Sym r1 = sym_for_type(elem, -1, 0);
  Sym r2 = sym_for_type(elem, -1, 0);
  Sym r3 = sym_for_type(simple_type(VT_SHORT), -1, 0);
  CType p1 = simple_type(VT_PTR);
  CType p2 = simple_type(VT_PTR);
  CType p3 = simple_type(VT_PTR);
  Sym sref;
  CType s1, s2;

  p1.ref = &r1;
  p2.ref = &r2;
  p3.ref = &r3;
  UT_ASSERT(is_compatible_types(&p1, &p2));  /* int* == int* */
  UT_ASSERT(!is_compatible_types(&p1, &p3)); /* int* != short* */

  /* same struct ref -> compatible; distinct refs -> not */
  sref = sym_for_type(simple_type(VT_INT), 4, 4);
  s1 = simple_type(VT_STRUCT);
  s2 = simple_type(VT_STRUCT);
  s1.ref = &sref;
  s2.ref = &sref;
  UT_ASSERT(is_compatible_types(&s1, &s2));
  return 0;
}

UT_TEST(test_is_compatible_func)
{
  Sym f1, f2;
  CType t1, t2;

  memset(&f1, 0, sizeof(f1));
  memset(&f2, 0, sizeof(f2));
  f1.type = simple_type(VT_INT);
  f2.type = simple_type(VT_INT);
  f1.f.func_type = FUNC_NEW;
  f2.f.func_type = FUNC_NEW;
  t1 = simple_type(VT_FUNC);
  t2 = simple_type(VT_FUNC);
  t1.ref = &f1;
  t2.ref = &f2;
  UT_ASSERT(is_compatible_func(&t1, &t2));

  /* calling-convention mismatch -> incompatible */
  f2.f.func_call = 1;
  UT_ASSERT(!is_compatible_func(&t1, &t2));
  f2.f.func_call = 0;

  /* return-type mismatch -> incompatible */
  f2.type = simple_type(VT_SHORT);
  UT_ASSERT(!is_compatible_func(&t1, &t2));
  f2.type = simple_type(VT_INT);

  /* one OLD-style prototype matches any new one */
  f2.f.func_type = FUNC_OLD;
  UT_ASSERT(is_compatible_func(&t1, &t2));
  return 0;
}

UT_TEST(test_is_transparent_union_type)
{
  CType i = simple_type(VT_INT);
  CType u = simple_type(VT_STRUCT);
  Sym head;

  UT_ASSERT(!is_transparent_union_type(&i));

  memset(&head, 0, sizeof(head));
  head.a.transparent_union = 1;
  head.type.t = VT_UNION;
  u.ref = &head;
  UT_ASSERT(is_transparent_union_type(&u));

  head.a.transparent_union = 0;
  UT_ASSERT(!is_transparent_union_type(&u));
  return 0;
}

UT_TEST(test_type_contains_pointer)
{
  CType i = simple_type(VT_INT);
  Sym pref = sym_for_type(simple_type(VT_INT), -1, 0);
  CType p = simple_type(VT_PTR);
  Sym f_ptr, f_int, head;
  CType st;

  UT_ASSERT(!type_contains_pointer(&i));

  p.ref = &pref;
  UT_ASSERT(type_contains_pointer(&p));

  /* struct { int a; int *b; } contains a pointer */
  f_ptr = sym_for_type(p, 4, 0);
  f_ptr.next = NULL;
  f_int = sym_for_type(simple_type(VT_INT), 0, 0);
  f_int.next = &f_ptr;
  head = sym_for_type(simple_type(VT_INT), 8, 4);
  head.next = &f_int;
  st = simple_type(VT_STRUCT);
  st.ref = &head;
  UT_ASSERT(type_contains_pointer(&st));

  /* struct { int a; } does not */
  head.next->next = NULL; /* drop the pointer field */
  UT_ASSERT(!type_contains_pointer(&st));
  return 0;
}

UT_TEST(test_struct_has_pointer_member)
{
  Sym f_int, f_int2, f_ptr, head1, head2;
  Sym pref = sym_for_type(simple_type(VT_INT), -1, 0);
  CType ptr = simple_type(VT_PTR);
  CType s1, s2, i;

  ptr.ref = &pref;

  /* struct { int a; } -> no pointer */
  f_int = sym_for_type(simple_type(VT_INT), 0, 0);
  f_int.next = NULL;
  head1 = sym_for_type(simple_type(VT_INT), 4, 4);
  head1.next = &f_int;
  s1 = simple_type(VT_STRUCT);
  s1.ref = &head1;
  UT_ASSERT(!struct_has_pointer_member(&s1));

  /* struct { int a; int *p; } -> has pointer */
  f_ptr = sym_for_type(ptr, 4, 0);
  f_ptr.next = NULL;
  f_int2 = sym_for_type(simple_type(VT_INT), 0, 0);
  f_int2.next = &f_ptr;
  head2 = sym_for_type(simple_type(VT_INT), 8, 4);
  head2.next = &f_int2;
  s2 = simple_type(VT_STRUCT);
  s2.ref = &head2;
  UT_ASSERT(struct_has_pointer_member(&s2));

  /* not a struct -> 0 */
  i = simple_type(VT_INT);
  UT_ASSERT(!struct_has_pointer_member(&i));
  return 0;
}

UT_TEST(test_struct_has_bitfield_member)
{
  Sym f = sym_for_type(simple_type(VT_INT | VT_BITFIELD), 0, 0);
  Sym head = sym_for_type(simple_type(VT_INT), 4, 4);
  CType s = simple_type(VT_STRUCT);

  f.next = NULL;
  head.next = &f;
  s.ref = &head;
  UT_ASSERT(struct_has_bitfield_member(&s));

  f.type.t = VT_INT; /* plain field */
  UT_ASSERT(!struct_has_bitfield_member(&s));
  return 0;
}

UT_TEST(test_struct_has_vla_member)
{
  Sym f = sym_for_type(simple_type(VT_PTR | VT_VLA), 0, 0);
  Sym head = sym_for_type(simple_type(VT_INT), 8, 4);
  CType s = simple_type(VT_STRUCT);

  f.next = NULL;
  head.next = &f;
  s.ref = &head;
  UT_ASSERT(struct_has_vla_member(&s));

  f.type.t = VT_INT;
  UT_ASSERT(!struct_has_vla_member(&s));
  return 0;
}

UT_TEST(test_struct_is_single_scalar_member)
{
  Sym f1 = sym_for_type(simple_type(VT_BYTE), 0, 0);
  Sym f2 = sym_for_type(simple_type(VT_SHORT), 0, 0);
  Sym head1 = sym_for_type(simple_type(VT_INT), 1, 1);
  Sym head2 = sym_for_type(simple_type(VT_INT), 2, 2);
  CType s1 = simple_type(VT_STRUCT);
  CType s2 = simple_type(VT_STRUCT);

  f1.next = NULL;
  head1.next = &f1;
  s1.ref = &head1;
  UT_ASSERT(struct_is_single_1byte_scalar_member(&s1));
  UT_ASSERT(!struct_is_single_2byte_scalar_member(&s1));

  f2.next = NULL;
  head2.next = &f2;
  s2.ref = &head2;
  UT_ASSERT(struct_is_single_2byte_scalar_member(&s2));
  UT_ASSERT(!struct_is_single_1byte_scalar_member(&s2));

  /* two fields -> not a single-member struct */
  f1.next = &f2;
  UT_ASSERT(!struct_is_single_1byte_scalar_member(&s1));
  return 0;
}

UT_TEST(test_auto_inline_type_ok)
{
  UT_ASSERT(auto_inline_type_ok(VT_INT));
  UT_ASSERT(auto_inline_type_ok(VT_LLONG));
  UT_ASSERT(auto_inline_type_ok(VT_PTR));
  UT_ASSERT(auto_inline_type_ok(VT_FLOAT));
  UT_ASSERT(auto_inline_type_ok(VT_BOOL));
  UT_ASSERT(auto_inline_type_ok(VT_STRUCT));
  UT_ASSERT(auto_inline_type_ok(VT_VOID));
  UT_ASSERT(!auto_inline_type_ok(VT_DOUBLE));
  UT_ASSERT(!auto_inline_type_ok(VT_LDOUBLE));
  return 0;
}

UT_TEST(test_is_vector_type_and_make_vector)
{
  CType i = simple_type(VT_INT);
  CType elem = simple_type(VT_INT);
  CType out;

  UT_ASSERT(!is_vector_type(&i));

  reset_symbol_state();
  make_vector_type(&out, &elem, 16);
  UT_ASSERT_EQ(out.t & VT_BTYPE, VT_STRUCT);
  UT_ASSERT(out.t & VT_VECTOR);
  UT_ASSERT(is_vector_type(&out));
  UT_ASSERT_EQ(vector_elem_count(&out), 4); /* 16 bytes / 4-byte int */
  global_stack = NULL;
  return 0;
}

UT_TEST(test_compute_aapcs_natural_alignment)
{
  CType i = simple_type(VT_INT);
  CType c = simple_type(VT_BYTE);
  CType d = simple_type(VT_DOUBLE);
  Sym f_int, f_char, head;
  CType st;

  UT_ASSERT_EQ(compute_aapcs_natural_alignment(&i), 4);
  UT_ASSERT_EQ(compute_aapcs_natural_alignment(&c), 1);
  UT_ASSERT_EQ(compute_aapcs_natural_alignment(&d), 8);

  /* struct { char; int; } -> natural alignment of widest member = 4 */
  f_int = sym_for_type(simple_type(VT_INT), 4, 0);
  f_int.next = NULL;
  f_char = sym_for_type(simple_type(VT_BYTE), 0, 0);
  f_char.next = &f_int;
  head = sym_for_type(simple_type(VT_INT), 8, 4);
  head.next = &f_char;
  st = simple_type(VT_STRUCT);
  st.ref = &head;
  UT_ASSERT_EQ(compute_aapcs_natural_alignment(&st), 4);
  return 0;
}

UT_TEST(test_try_get_constant)
{
  SValue sv;
  size_t sz;
  unsigned char uc;

  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CONST;
  sv.c.i = 258;
  UT_ASSERT(try_get_constant_size_t(&sv, &sz));
  UT_ASSERT_EQ((int)sz, 258);
  UT_ASSERT(try_get_constant_uchar(&sv, &uc));
  UT_ASSERT_EQ(uc, 2); /* 258 & 0xff */

  sv.r = VT_LOCAL; /* not a plain constant */
  UT_ASSERT(!try_get_constant_size_t(&sv, &sz));

  /* NULL guards */
  UT_ASSERT(!try_get_constant_size_t(NULL, &sz));
  UT_ASSERT(!try_get_constant_uchar(&sv, NULL));
  return 0;
}

UT_TEST(test_fold_builtin_str_and_mem)
{
  int off = -99;

  UT_ASSERT_EQ(fold_builtin_strcmp_result("abc", "abc"), 0);
  UT_ASSERT(fold_builtin_strcmp_result("abc", "abd") < 0);
  UT_ASSERT(fold_builtin_strcmp_result("abd", "abc") > 0);

  UT_ASSERT_EQ(fold_builtin_strncmp_result("abcX", "abcY", 3), 0);
  UT_ASSERT(fold_builtin_strncmp_result("abcX", "abcY", 4) < 0);
  UT_ASSERT_EQ(fold_builtin_strncmp_result("a", "b", 0), 0);

  UT_ASSERT_EQ(fold_builtin_memcmp_result("abc", "abc", 3), 0);
  UT_ASSERT(fold_builtin_memcmp_result("abc", "abd", 3) < 0);

  UT_ASSERT(fold_builtin_memchr_offset("hello", 'l', 5, &off));
  UT_ASSERT_EQ(off, 2);
  UT_ASSERT(fold_builtin_memchr_offset("hello", 'z', 5, &off));
  UT_ASSERT_EQ(off, -1);
  UT_ASSERT(!fold_builtin_memchr_offset("hello", 'l', 5, NULL));
  return 0;
}

UT_TEST(test_get_builtin_abs_info)
{
  int u;

  UT_ASSERT(get_builtin_abs_info("abs", &u));
  UT_ASSERT_EQ(u, 0);
  UT_ASSERT(get_builtin_abs_info("labs", &u));
  UT_ASSERT_EQ(u, 0);
  UT_ASSERT(get_builtin_abs_info("llabs", &u));
  UT_ASSERT_EQ(u, 0);
  UT_ASSERT(get_builtin_abs_info("uabs", &u));
  UT_ASSERT_EQ(u, 1);
  UT_ASSERT(get_builtin_abs_info("ullabs", &u));
  UT_ASSERT_EQ(u, 1);
  UT_ASSERT(!get_builtin_abs_info("foo", &u));
  return 0;
}

UT_TEST(test_mark_value_bytes_scalar_and_vla)
{
  unsigned char map[8];
  CType i = simple_type(VT_INT);
  CType v = simple_type(VT_PTR | VT_VLA);

  memset(map, 0, sizeof(map));
  UT_ASSERT_EQ(mark_value_bytes(&i, 0, map, 8), 0);
  UT_ASSERT_EQ(map[0], 1);
  UT_ASSERT_EQ(map[3], 1);
  UT_ASSERT_EQ(map[4], 0); /* only the 4 int bytes are value bytes */

  /* a VLA can't be statically mapped -> -1 */
  memset(map, 0, sizeof(map));
  UT_ASSERT_EQ(mark_value_bytes(&v, 0, map, 8), -1);
  return 0;
}

UT_TEST(test_vpush_variants)
{
  CType t = simple_type(VT_INT);

  reset_vstack();
  vpush(&t);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT_EQ(vtop->c.i, 0);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_INT);
  vpop();

  vpushs(0x1234);
  UT_ASSERT_EQ(vtop->c.i, 0x1234);
  vpop();

  vpushll(0x100000000LL);
  UT_ASSERT_EQ(vtop->c.i, 0x100000000LL);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_LLONG);
  vpop();

  vseti(VT_LOCAL, 8);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_LOCAL);
  UT_ASSERT_EQ(vtop->c.i, 8);
  return 0;
}

UT_TEST(test_vdup)
{
  reset_vstack();
  vpushi(77);
  vdup();
  UT_ASSERT_EQ(vtop[0].c.i, 77);
  UT_ASSERT_EQ(vtop[-1].c.i, 77);
  UT_ASSERT_EQ(vtop, _vstack + 2);
  return 0;
}

UT_TEST(test_vpush_ref)
{
  CType t = simple_type(VT_INT);

  reset_symbol_state();
  reset_vstack();
  tcc_state = &ut_dummy_state;
  vpush_ref(&t, NULL, 0x40, 4);
  UT_ASSERT(vtop->sym != NULL);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT(vtop->r & VT_SYM);
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_is_cond_bool)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CMP;
  UT_ASSERT(is_cond_bool(&sv));
  sv.r = VT_CONST;
  UT_ASSERT(!is_cond_bool(&sv));
  return 0;
}

UT_TEST(test_sym_scope)
{
  Sym s;
  memset(&s, 0, sizeof(s));
  s.type.t = VT_INT;
  s.sym_scope = 3;
  UT_ASSERT_EQ(sym_scope(&s), 3);
  return 0;
}

UT_TEST(test_new_prev_scope_s)
{
  struct scope o;
  int base_scope;
  Sym *base;
  Sym *pushed;

  reset_symbol_state();
  base_scope = local_scope;
  base = local_stack;

  new_scope_s(&o);
  UT_ASSERT_EQ(o.lstk, base);
  UT_ASSERT_EQ(local_scope, base_scope + 1);

  /* push a local (nonzero c, so sym_pop doesn't touch tcc_state->ir) */
  pushed = sym_push2(&local_stack, TOK_IDENT + 1, VT_INT, 1);
  UT_ASSERT_EQ(local_stack, pushed);

  prev_scope_s(&o);
  UT_ASSERT_EQ(local_stack, base);
  UT_ASSERT_EQ(local_scope, base_scope);
  return 0;
}

UT_TEST(test_case_cmp)
{
  struct switch_t sw;
  memset(&sw, 0, sizeof(sw));
  cur_switch = &sw;

  sw.sv.type.t = VT_INT; /* signed */
  UT_ASSERT_EQ(case_cmp(1, 2), -1);
  UT_ASSERT_EQ(case_cmp(2, 1), 1);
  UT_ASSERT_EQ(case_cmp(3, 3), 0);
  UT_ASSERT_EQ(case_cmp((uint64_t)-1, 0), -1); /* -1 < 0 signed */

  sw.sv.type.t = VT_INT | VT_UNSIGNED;
  UT_ASSERT_EQ(case_cmp((uint64_t)-1, 0), 1); /* huge > 0 unsigned */

  cur_switch = NULL;
  return 0;
}

UT_TEST(test_switch_can_use_jump_table)
{
  struct switch_t sw;
  struct case_t c0, c1, c2, c3;
  struct case_t *ps[4];

  memset(&sw, 0, sizeof(sw));
  c0.v1 = c0.v2 = 0;
  c1.v1 = c1.v2 = 1;
  c2.v1 = c2.v2 = 2;
  c3.v1 = c3.v2 = 3;
  ps[0] = &c0;
  ps[1] = &c1;
  ps[2] = &c2;
  ps[3] = &c3;
  sw.p = ps;
  sw.n = 4;
  sw.sv.type.t = VT_INT;

  tcc_state = &ut_dummy_state;
  ut_dummy_state.optimize = 0;
  UT_ASSERT(!switch_can_use_jump_table(&sw)); /* optimization off */

  ut_dummy_state.optimize = 1;
  UT_ASSERT(switch_can_use_jump_table(&sw)); /* dense, 4 cases */

  sw.n = 3;
  UT_ASSERT(!switch_can_use_jump_table(&sw)); /* too few cases */
  sw.n = 4;

  sw.sv.type.t = VT_LLONG;
  UT_ASSERT(!switch_can_use_jump_table(&sw)); /* long long unsupported */
  sw.sv.type.t = VT_INT;

  /* sparse: 0,1,2,100 -> range 101, 2*n=8 < 101 -> reject */
  c3.v1 = c3.v2 = 100;
  UT_ASSERT(!switch_can_use_jump_table(&sw));

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_case_sort_merges_adjacent)
{
  struct switch_t sw;
  struct case_t *c0 = tcc_malloc(sizeof *c0);
  struct case_t *c1 = tcc_malloc(sizeof *c1);
  struct case_t *c2 = tcc_malloc(sizeof *c2);
  struct case_t *ps[3];

  /* out-of-order singletons 3,1,2, all same target -> merge to one 1..3 range */
  c0->v1 = c0->v2 = 3;
  c0->ind = 7;
  c0->line = 1;
  c1->v1 = c1->v2 = 1;
  c1->ind = 7;
  c1->line = 2;
  c2->v1 = c2->v2 = 2;
  c2->ind = 7;
  c2->line = 3;
  ps[0] = c0;
  ps[1] = c1;
  ps[2] = c2;

  memset(&sw, 0, sizeof(sw));
  sw.p = ps;
  sw.n = 3;
  sw.sv.type.t = VT_INT;
  cur_switch = &sw;

  case_sort(&sw);
  UT_ASSERT_EQ(sw.n, 1);
  UT_ASSERT_EQ((int)sw.p[0]->v1, 1);
  UT_ASSERT_EQ((int)sw.p[0]->v2, 3);

  cur_switch = NULL;
  return 0;
}

UT_TEST(test_get_const_double_and_float)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));

  sv.type = simple_type(VT_INT);
  sv.c.i = (uint64_t)(int64_t)-7;
  UT_ASSERT(get_const_double(&sv) == -7.0);
  UT_ASSERT(get_const_float(&sv) == -7.0f);

  sv.type = simple_type(VT_DOUBLE);
  sv.c.d = 3.5;
  UT_ASSERT(get_const_double(&sv) == 3.5);
  UT_ASSERT(get_const_float(&sv) == 3.5f);

  sv.type = simple_type(VT_FLOAT);
  sv.c.f = 1.25f;
  UT_ASSERT(get_const_double(&sv) == 1.25);
  UT_ASSERT(get_const_float(&sv) == 1.25f);

  sv.type = simple_type(VT_BYTE);
  sv.c.i = (uint64_t)(int64_t)-3; /* (char)-3 */
  UT_ASSERT(get_const_double(&sv) == -3.0);

  sv.type = simple_type(VT_LLONG);
  sv.c.i = 1000000;
  UT_ASSERT(get_const_double(&sv) == 1000000.0);
  return 0;
}

UT_TEST(test_value64)
{
  /* signed int sign-extends bit 31 into the high word */
  UT_ASSERT_EQ(value64(0x80000000ULL, VT_INT), 0xFFFFFFFF80000000ULL);
  /* unsigned int zero-extends */
  UT_ASSERT_EQ(value64(0x80000000ULL, VT_INT | VT_UNSIGNED), 0x0000000080000000ULL);
  /* positive int untouched */
  UT_ASSERT_EQ(value64(0x00000001ULL, VT_INT), 0x0000000000000001ULL);
  /* long long keeps the full 64-bit pattern */
  UT_ASSERT_EQ(value64(0x1122334455667788ULL, VT_LLONG), 0x1122334455667788ULL);
  return 0;
}

UT_TEST(test_gen_opic_sdiv_and_lt)
{
  UT_ASSERT_EQ(gen_opic_sdiv(20, 4), 5);
  UT_ASSERT_EQ(gen_opic_sdiv((uint64_t)-20, 4), (uint64_t)-5);
  UT_ASSERT_EQ(gen_opic_sdiv((uint64_t)-20, (uint64_t)-4), 5);

  UT_ASSERT(gen_opic_lt((uint64_t)-1, 0)); /* -1 < 0 */
  UT_ASSERT(!gen_opic_lt(0, (uint64_t)-1));
  UT_ASSERT(gen_opic_lt(1, 2));
  UT_ASSERT(!gen_opic_lt(2, 1));
  return 0;
}

UT_TEST(test_is_zero_length_builtin_compare)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CONST;
  sv.c.i = 0;
  UT_ASSERT(is_zero_length_builtin_compare(&sv));
  sv.c.i = 1;
  UT_ASSERT(!is_zero_length_builtin_compare(&sv));
  sv.c.i = 0;
  sv.r = VT_LOCAL;
  UT_ASSERT(!is_zero_length_builtin_compare(&sv));
  return 0;
}

UT_TEST(test_bitfield_unit_width)
{
  Sym f;
  Sym aux;

  memset(&f, 0, sizeof(f));
  f.type.t = VT_INT;
  f.type.ref = NULL; /* no auxtype -> base type width */
  UT_ASSERT_EQ(bitfield_unit_width(&f), 4);

  memset(&aux, 0, sizeof(aux));
  aux.auxtype = VT_SHORT;
  f.type.ref = &aux; /* auxtype pins the storage unit width */
  UT_ASSERT_EQ(bitfield_unit_width(&f), 2);

  aux.auxtype = VT_STRUCT; /* special-cased to 0 */
  UT_ASSERT_EQ(bitfield_unit_width(&f), 0);
  return 0;
}

UT_TEST(test_sym_copy)
{
  Sym s0;
  Sym *ps = NULL;
  Sym *copy;

  memset(&s0, 0, sizeof(s0));
  s0.v = SYM_FIRST_ANOM; /* anonymous -> skips the table_ident path */
  s0.type.t = VT_INT;
  s0.c = 42;

  copy = sym_copy(&s0, &ps);
  UT_ASSERT(copy != NULL);
  UT_ASSERT(copy != &s0);
  UT_ASSERT_EQ((unsigned)copy->v, (unsigned)SYM_FIRST_ANOM);
  UT_ASSERT_EQ(copy->c, 42);
  UT_ASSERT_EQ(ps, copy);       /* pushed onto ps */
  UT_ASSERT(copy->prev == NULL); /* first on the (empty) stack */
  return 0;
}

UT_TEST(test_reg_return_helpers)
{
  SValue sv;

  UT_ASSERT_EQ(RC_TYPE(VT_INT), RC_INT);
  UT_ASSERT_EQ(RC_TYPE(VT_PTR), RC_INT);
  UT_ASSERT_EQ(RC_TYPE(VT_FLOAT), RC_FLOAT);
  UT_ASSERT_EQ(RC_TYPE(VT_DOUBLE), RC_FLOAT);

  UT_ASSERT_EQ(R_RET(VT_INT), REG_IRET);
  UT_ASSERT_EQ(R_RET(VT_FLOAT), REG_FRET);

  memset(&sv, 0, sizeof(sv));
  PUT_R_RET(&sv, VT_INT);
  UT_ASSERT_EQ(sv.r, REG_IRET);
  PUT_R_RET(&sv, VT_FLOAT);
  UT_ASSERT_EQ(sv.r, REG_FRET);
  return 0;
}

UT_TEST(test_merge_symattr)
{
  struct SymAttr a, b;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  b.aligned = 4;
  b.packed = 1;
  b.weak = 1;
  b.visibility = STV_HIDDEN;
  b.dllexport = 1;
  b.transparent_union = 1;
  merge_symattr(&a, &b);
  UT_ASSERT_EQ(a.aligned, 4);
  UT_ASSERT_EQ(a.packed, 1);
  UT_ASSERT_EQ(a.weak, 1);
  UT_ASSERT_EQ(a.visibility, STV_HIDDEN);
  UT_ASSERT_EQ(a.dllexport, 1);
  UT_ASSERT_EQ(a.transparent_union, 1);

  /* an already-set alignment is not overwritten */
  a.aligned = 2;
  b.aligned = 5;
  merge_symattr(&a, &b);
  UT_ASSERT_EQ(a.aligned, 2);
  return 0;
}

UT_TEST(test_merge_funcattr)
{
  struct FuncAttr a, b;

  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  b.func_call = 2;
  b.func_type = FUNC_NEW;
  b.func_noreturn = 1;
  b.func_pure = 1;
  merge_funcattr(&a, &b);
  UT_ASSERT_EQ(a.func_call, 2);
  UT_ASSERT_EQ(a.func_type, FUNC_NEW);
  UT_ASSERT_EQ(a.func_noreturn, 1);
  UT_ASSERT_EQ(a.func_pure, 1);

  /* an already-set calling convention wins */
  b.func_call = 3;
  merge_funcattr(&a, &b);
  UT_ASSERT_EQ(a.func_call, 2);
  return 0;
}

UT_TEST(test_merge_attr_and_sym_to_attr)
{
  AttributeDef ad, ad1;
  Sym s;

  memset(&ad, 0, sizeof(ad));
  memset(&ad1, 0, sizeof(ad1));
  ad1.a.weak = 1;
  ad1.f.func_noreturn = 1;
  ad1.asm_label = TOK_IDENT + 5;
  ad1.attr_mode = 3;
  ad1.vector_size = 16;
  merge_attr(&ad, &ad1);
  UT_ASSERT_EQ(ad.a.weak, 1);
  UT_ASSERT_EQ(ad.f.func_noreturn, 1);
  UT_ASSERT_EQ(ad.asm_label, TOK_IDENT + 5);
  UT_ASSERT_EQ(ad.attr_mode, 3);
  UT_ASSERT_EQ(ad.vector_size, 16);

  memset(&ad, 0, sizeof(ad));
  memset(&s, 0, sizeof(s));
  s.a.packed = 1;
  s.f.func_pure = 1;
  sym_to_attr(&ad, &s);
  UT_ASSERT_EQ(ad.a.packed, 1);
  UT_ASSERT_EQ(ad.f.func_pure, 1);
  return 0;
}

UT_TEST(test_init_prec)
{
  memset(prec, 0xff, sizeof(prec));
  init_prec();
  UT_ASSERT_EQ(prec['+'], 9);
  UT_ASSERT_EQ(prec['-'], 9);
  UT_ASSERT_EQ(prec['*'], 10);
  UT_ASSERT_EQ(prec['/'], 10);
  UT_ASSERT_EQ(prec['%'], 10);
  UT_ASSERT_EQ(prec['|'], 3);
  UT_ASSERT_EQ(prec['^'], 4);
  UT_ASSERT_EQ(prec['&'], 5);
  UT_ASSERT_EQ(prec['~'], 0);
  UT_ASSERT_EQ(prec[0], 0);
  return 0;
}

/* `precedence` is a macro from here on (tccgen.c redefines it as prec[]);
   drop it to reach the underlying function for the token-valued cases. */
#undef precedence

UT_TEST(test_precedence_fn)
{
  UT_ASSERT_EQ(precedence(TOK_LOR), 1);
  UT_ASSERT_EQ(precedence(TOK_LAND), 2);
  UT_ASSERT_EQ(precedence('|'), 3);
  UT_ASSERT_EQ(precedence(TOK_EQ), 6);
  UT_ASSERT_EQ(precedence(TOK_NE), 6);
  UT_ASSERT_EQ(precedence(TOK_ULT), 7);
  UT_ASSERT_EQ(precedence(TOK_SHL), 8);
  UT_ASSERT_EQ(precedence(TOK_SAR), 8);
  UT_ASSERT_EQ(precedence('*'), 10);
  UT_ASSERT_EQ(precedence('!'), 0);
  return 0;
}

UT_TEST(test_convert_parameter_type)
{
  CType t = simple_type(VT_INT | VT_CONSTANT | VT_VOLATILE);
  Sym aref;
  CType at;
  CType ft;
  Sym fref;

  /* const/volatile qualifiers are dropped from a parameter type */
  convert_parameter_type(&t);
  UT_ASSERT_EQ(t.t & (VT_CONSTANT | VT_VOLATILE), 0);
  UT_ASSERT_EQ(t.t & VT_BTYPE, VT_INT);

  /* array parameter decays to pointer (VT_ARRAY stripped) */
  aref = sym_for_type(simple_type(VT_INT), 4, 0);
  at = simple_type(VT_PTR | VT_ARRAY);
  at.ref = &aref;
  convert_parameter_type(&at);
  UT_ASSERT_EQ(at.t & VT_ARRAY, 0);
  UT_ASSERT_EQ(at.t & VT_BTYPE, VT_PTR);

  /* function parameter decays to pointer-to-function */
  reset_symbol_state();
  memset(&fref, 0, sizeof(fref));
  ft = simple_type(VT_FUNC);
  ft.ref = &fref;
  convert_parameter_type(&ft);
  UT_ASSERT_EQ(ft.t & VT_BTYPE, VT_PTR);
  UT_ASSERT(ft.ref != NULL);
  global_stack = NULL;
  return 0;
}

UT_TEST(test_parse_btype_qualify)
{
  CType t = simple_type(VT_INT);
  Sym eref;
  CType at;

  /* non-array: qualifier is OR'd straight into the type */
  parse_btype_qualify(&t, VT_CONSTANT);
  UT_ASSERT(t.t & VT_CONSTANT);

  /* array: qualifier lands on the (copied) element type */
  reset_symbol_state();
  eref = sym_for_type(simple_type(VT_INT), 4, 3);
  at = simple_type(VT_PTR | VT_ARRAY);
  at.ref = &eref;
  parse_btype_qualify(&at, VT_VOLATILE);
  UT_ASSERT(at.ref->type.t & VT_VOLATILE);
  global_stack = NULL;
  return 0;
}

UT_TEST(test_is_const_for_folding)
{
  SValue sv;

  memset(&sv, 0, sizeof(sv));
  tcc_state = &ut_dummy_state;
  ut_dummy_state.in_inline_expansion = 0;

  sv.r = VT_CONST;
  sv.type.t = VT_INT;
  UT_ASSERT(is_const_for_folding(&sv));
  sv.type.t = VT_DOUBLE;
  UT_ASSERT(is_const_for_folding(&sv));
  sv.type.t = VT_LLONG;
  UT_ASSERT(is_const_for_folding(&sv));

  /* symbolic constant is not foldable */
  sv.type.t = VT_INT;
  sv.r = VT_CONST | VT_SYM;
  UT_ASSERT(!is_const_for_folding(&sv));

  /* struct is not a foldable scalar */
  sv.r = VT_CONST;
  sv.type.t = VT_STRUCT;
  UT_ASSERT(!is_const_for_folding(&sv));

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_check_nonvoid_value_ok)
{
  reset_vstack();
  vpushi(1); /* non-void -> must return without aborting */
  check_nonvoid_value();
  return 0;
}

UT_TEST(test_inline_body_scanners)
{
  int s_ret[] = {TOK_IDENT, TOK_RETURN, 0};
  int s_none[] = {TOK_IDENT, TOK_IDENT, 0};
  int s_while[] = {TOK_IDENT, TOK_WHILE, 0};
  int s_do[] = {TOK_DO, 0};
  int s_static[] = {TOK_STATIC, 0};
  int s_apply[] = {TOK_builtin_apply_args, 0};
  TokenString ts;

  UT_ASSERT(!inline_body_has_return_stmt(NULL)); /* NULL guard */

  ts = make_tokstr(s_ret);
  UT_ASSERT(inline_body_has_return_stmt(&ts));
  ts = make_tokstr(s_none);
  UT_ASSERT(!inline_body_has_return_stmt(&ts));

  ts = make_tokstr(s_while);
  UT_ASSERT(inline_body_has_loops(&ts));
  ts = make_tokstr(s_do);
  UT_ASSERT(inline_body_has_loops(&ts));
  ts = make_tokstr(s_none);
  UT_ASSERT(!inline_body_has_loops(&ts));

  ts = make_tokstr(s_static);
  UT_ASSERT(inline_body_has_static_local(&ts));
  ts = make_tokstr(s_none);
  UT_ASSERT(!inline_body_has_static_local(&ts));

  ts = make_tokstr(s_apply);
  UT_ASSERT(inline_body_has_apply_args(&ts));
  ts = make_tokstr(s_none);
  UT_ASSERT(!inline_body_has_apply_args(&ts));
  return 0;
}

UT_TEST(test_find_sv_const_init)
{
  static unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  Sym s;
  SValue sv;

  reset_symbol_state();
  memset(&s, 0, sizeof(s));
  s.v = TOK_IDENT + 1; /* a named local */
  s.c = 0x20;          /* stack offset */
  s.const_init_data = data;
  s.const_init_valid = 1;
  s.const_init_size = 8;
  s.prev = NULL;
  local_stack = &s;

  memset(&sv, 0, sizeof(sv));
  sv.r = VT_LOCAL | VT_LVAL;
  sv.c.i = 0x20;
  UT_ASSERT_EQ(find_sv_const_init(&sv, 4), data);
  UT_ASSERT_EQ(find_sv_const_init(&sv, 8), data);
  UT_ASSERT(find_sv_const_init(&sv, 16) == NULL); /* min_size too large */

  sv.r = VT_CONST; /* wrong storage class */
  UT_ASSERT(find_sv_const_init(&sv, 4) == NULL);

  /* the vec-literal variant rejects a *named* local... */
  sv.r = VT_LOCAL | VT_LVAL;
  UT_ASSERT(find_sv_vec_literal_init(&sv, 4) == NULL);
  /* ...but accepts an anonymous one */
  s.v = SYM_FIRST_ANOM;
  UT_ASSERT_EQ(find_sv_vec_literal_init(&sv, 4), data);

  local_stack = NULL;
  return 0;
}

UT_TEST(test_funcall_scratch)
{
  FuncallScratch *a = tcc_mallocz(sizeof *a);
  FuncallScratch *b = tcc_mallocz(sizeof *b);
  FuncallScratch *c = tcc_mallocz(sizeof *c);

  funcall_scratch_free(NULL); /* NULL guard */

  /* stack: a -> b -> c */
  funcall_scratch_stack = a;
  a->next = b;
  b->next = c;
  c->next = NULL;

  funcall_scratch_pop_free(b); /* unlink the middle node */
  UT_ASSERT_EQ(funcall_scratch_stack, a);
  UT_ASSERT_EQ(a->next, c);

  funcall_scratch_free_all(); /* drains the rest */
  UT_ASSERT(funcall_scratch_stack == NULL);
  return 0;
}

UT_TEST(test_find_nested_func_by_sym)
{
  static NestedFunc nfs[3];
  Sym s0, s1, s2, other;

  memset(nfs, 0, sizeof(nfs));
  nfs[0].sym = &s0;
  nfs[1].sym = &s1;
  nfs[2].sym = &s2;

  tcc_state = &ut_dummy_state;
  ut_dummy_state.nested_funcs = nfs;
  ut_dummy_state.nb_nested_funcs = 3;

  UT_ASSERT_EQ(find_nested_func_by_sym(&s1), &nfs[1]);
  UT_ASSERT_EQ(find_nested_func_by_sym(&s2), &nfs[2]);
  UT_ASSERT(find_nested_func_by_sym(&other) == NULL);

  ut_dummy_state.nested_funcs = NULL;
  ut_dummy_state.nb_nested_funcs = 0;
  tcc_state = NULL;
  return 0;
}

/* ---------------------------------------------------------------------------
 * IR-emission surface: gind/gjmp_acs/vset_VT_JMP hand work to the IR layer
 * (ir/gen sources, ir/codegen.c, svalue.c) that isn't linked here.  The capturing
 * stubs below record what tccgen.c passed down so the tests can assert on it,
 * and a fake TCCIRState stands in for tcc_state->ir.
 * --------------------------------------------------------------------------- */
static TCCIRState ut_fake_ir;
static TccIrOp ut_last_ir_op;
static SValue ut_last_ir_dest;
static int ut_ir_put_ret;
static int ut_test_gen_invert;
static int ut_test_gen_ret;

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  (void)ir;
  (void)src1;
  (void)src2;
  ut_last_ir_op = op;
  if (dest)
    ut_last_ir_dest = *dest;
  return ut_ir_put_ret;
}

void tcc_ir_gen_return_value(TCCIRState *ir, SValue *val)
{
  (void)ir;
  ut_last_ir_op = TCCIR_OP_RETURNVALUE;
  if (val)
    ut_last_ir_dest = *val;
}

void tcc_ir_gen_vla_alloc(TCCIRState *ir, SValue *size, int align)
{
  (void)ir;
  (void)align;
  ut_last_ir_op = TCCIR_OP_VLA_ALLOC;
  if (size)
    ut_last_ir_dest = *size;
}

void tcc_ir_gen_vla_sp_save(TCCIRState *ir, int slot)
{
  (void)ir;
  ut_last_ir_op = TCCIR_OP_VLA_SP_SAVE;
  ut_last_ir_dest.c.i = slot;
}

void tcc_ir_gen_vla_sp_restore(TCCIRState *ir, int slot)
{
  (void)ir;
  ut_last_ir_op = TCCIR_OP_VLA_SP_RESTORE;
  ut_last_ir_dest.c.i = slot;
}

void svalue_init(SValue *sv)
{
  memset(sv, 0, sizeof(*sv));
}

int tcc_ir_codegen_test_gen(struct TCCIRState *ir, int invert, int test)
{
  (void)ir;
  (void)test;
  ut_test_gen_invert = invert;
  return ut_test_gen_ret;
}

void tcc_tcov_block_begin(struct TCCState *s1)
{
  (void)s1;
}

UT_TEST(test_gind)
{
  int t;

  reset_vstack();
  memset(&ut_fake_ir, 0, sizeof(ut_fake_ir));
  ut_fake_ir.next_instruction_index = 7;
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;
  debug_modes = 0;
  nocode_wanted = CODE_OFF_BIT;

  t = gind();
  UT_ASSERT_EQ(t, 7);                          /* returns the current IR index */
  UT_ASSERT_EQ(nocode_wanted & CODE_OFF_BIT, 0); /* CODE_ON() cleared the bit */

  tcc_state = NULL;
  nocode_wanted = 0x40000000;
  return 0;
}

UT_TEST(test_gjmp_acs)
{
  int r;

  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;
  nocode_wanted = 0; /* so CODE_OFF() actually sets the bit */
  ut_ir_put_ret = 99;
  ut_last_ir_op = (TccIrOp)-1;

  r = gjmp_acs(42);
  UT_ASSERT_EQ(r, 99);                          /* returns tcc_ir_put's index */
  UT_ASSERT_EQ(ut_last_ir_op, TCCIR_OP_JUMP);   /* emitted a JUMP */
  UT_ASSERT_EQ((int)ut_last_ir_dest.c.i, 42);   /* to target 42 */
  UT_ASSERT_EQ(ut_last_ir_dest.r, VT_CONST);
  UT_ASSERT(nocode_wanted & CODE_OFF_BIT);      /* code suppressed after jump */

  tcc_state = NULL;
  nocode_wanted = 0x40000000;
  return 0;
}

UT_TEST(test_vset_VT_JMP)
{
  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* non-VT_CMP top of stack: early return, vtop untouched */
  vpushi(5);
  vset_VT_JMP();
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_CONST);
  UT_ASSERT_EQ(vtop->c.i, 5);
  vpop();

  /* VT_CMP with a real comparison op (>= 2) -> invert 0, becomes VT_JMP */
  vpushi(0);
  vtop->r = VT_CMP;
  vtop->cmp_op = TOK_NE;
  vtop->type.t = VT_INT;
  ut_test_gen_ret = 55;
  ut_test_gen_invert = -1;
  vset_VT_JMP();
  UT_ASSERT_EQ(ut_test_gen_invert, 0);
  UT_ASSERT_EQ(vtop->r & VT_VALMASK, VT_JMP & VT_VALMASK);
  UT_ASSERT_EQ(vtop->c.i, 55);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_type_to_str)
{
  char buf[256];
  CType t;
  Sym r;

  t = simple_type(VT_INT | VT_CONSTANT);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "const int") == 0);

  t = simple_type(VT_INT | VT_UNSIGNED);
  type_to_str(buf, sizeof(buf), &t, "x");
  UT_ASSERT(strcmp(buf, "unsigned int x") == 0);

  t = simple_type(VT_INT | VT_EXTERN);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "extern int") == 0);

  t = simple_type(VT_INT | VT_LONG);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "long") == 0);

  t = simple_type(VT_VOID);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "void") == 0);

  t = simple_type(VT_BYTE | VT_DEFSIGN);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "signed char") == 0);

  t = simple_type(VT_LLONG);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "long long") == 0);

  t = simple_type(VT_DOUBLE | VT_COMPLEX);
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "double _Complex") == 0);

  /* pointer to int, named */
  r = sym_for_type(simple_type(VT_INT), -1, 0);
  t = simple_type(VT_PTR);
  t.ref = &r;
  type_to_str(buf, sizeof(buf), &t, "p");
  UT_ASSERT(strcmp(buf, "int *p") == 0);

  /* array of int[3] */
  r = sym_for_type(simple_type(VT_INT), 3, 0);
  t = simple_type(VT_PTR | VT_ARRAY);
  t.ref = &r;
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "int [3]") == 0);

  /* anonymous struct */
  r = sym_for_type(simple_type(VT_INT), 4, 4);
  r.v = SYM_FIRST_ANOM;
  t = simple_type(VT_STRUCT);
  t.ref = &r;
  type_to_str(buf, sizeof(buf), &t, NULL);
  UT_ASSERT(strcmp(buf, "struct <anonymous>") == 0);
  return 0;
}

UT_TEST(test_type_to_str_function)
{
  char buf[256];
  Sym fhead;
  CType ft;

  /* int (void): a no-parameter function */
  memset(&fhead, 0, sizeof(fhead));
  fhead.type = simple_type(VT_INT);
  fhead.next = NULL;
  fhead.f.func_type = FUNC_NEW;
  ft = simple_type(VT_FUNC);
  ft.ref = &fhead;
  type_to_str(buf, sizeof(buf), &ft, NULL);
  UT_ASSERT(strcmp(buf, "int ()") == 0);

  /* function pointer: varstr begins with '*' */
  type_to_str(buf, sizeof(buf), &ft, "*fp");
  UT_ASSERT(strcmp(buf, "int (*fp)()") == 0);

  /* variadic: FUNC_ELLIPSIS appends ", ..." — with one int parameter */
  {
    Sym param;
    memset(&param, 0, sizeof(param));
    param.type = simple_type(VT_INT);
    param.next = NULL;
    fhead.next = &param;
    fhead.f.func_type = FUNC_ELLIPSIS;
    type_to_str(buf, sizeof(buf), &ft, NULL);
    UT_ASSERT(strcmp(buf, "int (int, ...)") == 0);
  }
  return 0;
}

UT_TEST(test_type_incompatibility_warning)
{
  CType a = simple_type(VT_INT);
  CType b = simple_type(VT_SHORT);
  /* exercises type_to_str twice; tcc_warning is a no-op stub (no abort) */
  type_incompatibility_warning(&a, &b, "%s vs %s");
  return 0;
}

UT_TEST(test_compare_types_structural)
{
  CType a = simple_type(VT_INT);
  CType b = simple_type(VT_INT);
  CType c = simple_type(VT_SHORT);
  Sym r1 = sym_for_type(simple_type(VT_INT), -1, 0);
  Sym r2 = sym_for_type(simple_type(VT_INT), -1, 0);
  CType p1 = simple_type(VT_PTR);
  CType p2 = simple_type(VT_PTR);
  Sym f1a, h1, f2a, h2, f2b;
  CType s1, s2;

  UT_ASSERT(compare_types_structural(&a, &b));
  UT_ASSERT(!compare_types_structural(&a, &c));

  p1.ref = &r1;
  p2.ref = &r2;
  UT_ASSERT(compare_types_structural(&p1, &p2)); /* int* vs int* */

  /* two structs, distinct refs, identical layout -> structurally equal */
  f1a = sym_for_type(simple_type(VT_INT), 0, 0);
  f1a.next = NULL;
  h1 = sym_for_type(simple_type(VT_INT), 4, 4);
  h1.next = &f1a;
  f2a = sym_for_type(simple_type(VT_INT), 0, 0);
  f2a.next = NULL;
  h2 = sym_for_type(simple_type(VT_INT), 4, 4);
  h2.next = &f2a;
  s1 = simple_type(VT_STRUCT);
  s1.ref = &h1;
  s2 = simple_type(VT_STRUCT);
  s2.ref = &h2;
  UT_ASSERT(compare_types_structural(&s1, &s2));

  /* differing field type -> not equal */
  f2a.type = simple_type(VT_SHORT);
  UT_ASSERT(!compare_types_structural(&s1, &s2));

  /* differing field count -> not equal */
  f2a.type = simple_type(VT_INT);
  f2b = sym_for_type(simple_type(VT_INT), 4, 0);
  f2b.next = NULL;
  f2a.next = &f2b;
  UT_ASSERT(!compare_types_structural(&s1, &s2));
  return 0;
}

UT_TEST(test_auto_inline_sig_ok)
{
  Sym ref, p1, fs, nref_fs;
  CType ft;

  /* function with no ref -> rejected */
  memset(&nref_fs, 0, sizeof(nref_fs));
  nref_fs.type = simple_type(VT_FUNC);
  nref_fs.type.ref = NULL;
  UT_ASSERT_EQ(auto_inline_sig_ok(&nref_fs), 0);

  memset(&ref, 0, sizeof(ref));
  memset(&p1, 0, sizeof(p1));
  memset(&fs, 0, sizeof(fs));
  ref.type = simple_type(VT_INT); /* return type */
  ref.f.func_type = FUNC_NEW;
  p1.type = simple_type(VT_INT);
  p1.v = TOK_IDENT + 1;
  p1.next = NULL;
  ref.next = &p1;
  ft = simple_type(VT_FUNC);
  ft.ref = &ref;
  fs.type = ft;
  fs.v = TOK_IDENT + 2;

  /* int foo(int) -> ok */
  UT_ASSERT_EQ(auto_inline_sig_ok(&fs), 1);

  /* double return -> rejected */
  ref.type = simple_type(VT_DOUBLE);
  UT_ASSERT_EQ(auto_inline_sig_ok(&fs), 0);

  /* void return + long long param -> 2 (special "needs 64-bit slot") */
  ref.type = simple_type(VT_VOID);
  p1.type = simple_type(VT_LLONG);
  UT_ASSERT_EQ(auto_inline_sig_ok(&fs), 2);

  /* complex return -> rejected */
  ref.type = simple_type(VT_DOUBLE | VT_COMPLEX);
  UT_ASSERT_EQ(auto_inline_sig_ok(&fs), 0);

  /* unnamed parameter (v == 0) -> rejected */
  ref.type = simple_type(VT_INT);
  p1.type = simple_type(VT_INT);
  p1.v = 0;
  UT_ASSERT_EQ(auto_inline_sig_ok(&fs), 0);
  return 0;
}

UT_TEST(test_builtin_abs_decl_matches)
{
  Sym ref, param, fs;
  CType ft;

  UT_ASSERT_EQ(builtin_abs_decl_matches(NULL, "abs"), 1); /* no sym -> pass */

  memset(&ref, 0, sizeof(ref));
  memset(&param, 0, sizeof(param));
  memset(&fs, 0, sizeof(fs));
  ref.type = simple_type(VT_INT);
  ref.f.func_type = FUNC_NEW;
  param.type = simple_type(VT_INT);
  param.next = NULL;
  ref.next = &param;
  ft = simple_type(VT_FUNC);
  ft.ref = &ref;
  fs.type = ft;

  UT_ASSERT_EQ(builtin_abs_decl_matches(&fs, "abs"), 1);   /* int abs(int) */
  UT_ASSERT_EQ(builtin_abs_decl_matches(&fs, "nope"), 0);  /* unknown name */
  UT_ASSERT_EQ(builtin_abs_decl_matches(&fs, "llabs"), 0); /* wrong widths */

  /* unsigned int uabs(int) */
  ref.type = simple_type(VT_INT | VT_UNSIGNED);
  UT_ASSERT_EQ(builtin_abs_decl_matches(&fs, "uabs"), 1);
  return 0;
}

UT_TEST(test_try_get_constant_string_guards)
{
  SValue sv;
  Sym sym;
  int len = -1;

  memset(&sv, 0, sizeof(sv));

  /* not a constant symbol reference -> NULL */
  sv.r = VT_LOCAL;
  UT_ASSERT(try_get_constant_string(&sv, &len) == NULL);

  /* CONST|SYM but no backing sym -> NULL */
  sv.r = VT_CONST | VT_SYM;
  sv.sym = NULL;
  UT_ASSERT(try_get_constant_string(&sv, &len) == NULL);

  /* sym with no ELF entry (c <= 0) -> elfsym() NULL -> NULL */
  memset(&sym, 0, sizeof(sym));
  sym.c = 0;
  sv.sym = &sym;
  UT_ASSERT(try_get_constant_string(&sv, &len) == NULL);
  return 0;
}

/* Fold a binary op over two int constants and return the result. */
static long long fold_ii(int a, int b, int op)
{
  reset_vstack();
  vpushi(a);
  vpushi(b);
  gen_op(op);
  return vtop->c.i;
}

/* ...over two unsigned int constants. */
static long long fold_uu(unsigned a, unsigned b, int op)
{
  reset_vstack();
  vpushi((int)a);
  vtop->type.t |= VT_UNSIGNED;
  vpushi((int)b);
  vtop->type.t |= VT_UNSIGNED;
  gen_op(op);
  return vtop->c.i;
}

/* ...over two long long constants (folded via gen_opl). */
static long long fold_ll(long long a, long long b, int op)
{
  reset_vstack();
  vpushll(a);
  vpushll(b);
  gen_op(op);
  return vtop->c.i;
}

static void push_dbl(double d)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_DOUBLE;
  sv.r = VT_CONST;
  sv.c.d = d;
  vpushv(&sv);
}

static void push_flt(float f)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_FLOAT;
  sv.r = VT_CONST;
  sv.c.f = f;
  vpushv(&sv);
}

/* ...over two double constants (folded via gen_opif). */
static double fold_dd(double a, double b, int op)
{
  reset_vstack();
  push_dbl(a);
  push_dbl(b);
  gen_op(op);
  return vtop->c.d;
}

UT_TEST(test_gen_op_const_fold_int)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  UT_ASSERT_EQ(fold_ii(6, 4, '+'), 10);
  UT_ASSERT_EQ(fold_ii(6, 4, '-'), 2);
  UT_ASSERT_EQ(fold_ii(6, 4, '*'), 24);
  UT_ASSERT_EQ(fold_ii(20, 4, '/'), 5);
  UT_ASSERT_EQ(fold_ii(21, 4, '%'), 1);
  UT_ASSERT_EQ(fold_ii(3, 2, '&'), 2);
  UT_ASSERT_EQ(fold_ii(1, 2, '|'), 3);
  UT_ASSERT_EQ(fold_ii(5, 1, '^'), 4);
  UT_ASSERT_EQ(fold_ii(1, 4, TOK_SHL), 16);
  UT_ASSERT_EQ(fold_ii(-16, 2, TOK_SAR), -4);

  /* after each fold the stack holds exactly one value */
  reset_vstack();
  vpushi(2);
  vpushi(3);
  gen_op('+');
  UT_ASSERT_EQ(vtop, _vstack + 1);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_gen_op_compare_int)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* constant comparisons fold to a 0/1 constant */
  UT_ASSERT_EQ(fold_ii(3, 5, TOK_LT), 1);
  UT_ASSERT_EQ(fold_ii(5, 3, TOK_LT), 0);
  UT_ASSERT_EQ(fold_ii(5, 5, TOK_EQ), 1);
  UT_ASSERT_EQ(fold_ii(5, 3, TOK_NE), 1);
  UT_ASSERT_EQ(fold_ii(3, 5, TOK_LE), 1);
  UT_ASSERT_EQ(fold_ii(5, 5, TOK_LE), 1);
  UT_ASSERT_EQ(fold_ii(5, 5, TOK_GE), 1);
  UT_ASSERT_EQ(fold_ii(5, 3, TOK_GT), 1);
  UT_ASSERT_EQ(fold_ii(-1, 1, TOK_LT), 1); /* signed: -1 < 1 */
  return 0;
}

UT_TEST(test_gen_op_const_fold_unsigned)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  UT_ASSERT_EQ(fold_uu(0xFFFFFFFFu, 2, TOK_UDIV), 0x7FFFFFFF);
  UT_ASSERT_EQ(fold_uu(7, 3, TOK_UMOD), 1);
  UT_ASSERT_EQ(fold_uu(0xFFFFFFFFu, 4, TOK_SHR), 0x0FFFFFFF); /* logical shift */
  UT_ASSERT_EQ(fold_uu(0xFFFFFFFFu, 1, TOK_ULT), 0);          /* huge < 1 -> false */
  UT_ASSERT_EQ(fold_uu(1, 0xFFFFFFFFu, TOK_ULT), 1);
  return 0;
}

UT_TEST(test_gen_op_const_fold_llong)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  UT_ASSERT_EQ(fold_ll(0x100000000LL, 5, '+'), 0x100000005LL);
  UT_ASSERT_EQ(fold_ll(0x7fffffffffffffffLL, 1, '+'), (long long)0x8000000000000000ULL);
  UT_ASSERT_EQ(fold_ll(6LL, 7LL, '*'), 42LL);
  UT_ASSERT_EQ(fold_ll(1LL, 40, TOK_SHL), 0x10000000000LL);
  UT_ASSERT_EQ(fold_ll(-100LL, 7LL, '%'), -100LL % 7LL);
  return 0;
}

UT_TEST(test_gen_op_const_fold_double)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  UT_ASSERT(fold_dd(1.5, 2.25, '+') == 3.75);
  UT_ASSERT(fold_dd(5.0, 2.0, '-') == 3.0);
  UT_ASSERT(fold_dd(1.5, 4.0, '*') == 6.0);
  UT_ASSERT(fold_dd(9.0, 2.0, '/') == 4.5);
  return 0;
}

UT_TEST(test_gen_cast_const)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* int -> signed char: 300 truncates to 44 */
  reset_vstack();
  vpushi(300);
  gen_cast_s(VT_BYTE);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_BYTE);
  UT_ASSERT_EQ((int)(signed char)vtop->c.i, 44);

  /* int -> short: 0x1FFFF truncates to -1 */
  reset_vstack();
  vpushi(0x1FFFF);
  gen_cast_s(VT_SHORT);
  UT_ASSERT_EQ((int)(short)vtop->c.i, -1);

  /* int -> long long: sign-extends */
  reset_vstack();
  vpushi(-5);
  gen_cast_s(VT_LLONG);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_LLONG);
  UT_ASSERT_EQ(vtop->c.i, -5);

  /* int -> double */
  reset_vstack();
  vpushi(7);
  gen_cast_s(VT_DOUBLE);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_DOUBLE);
  UT_ASSERT(vtop->c.d == 7.0);

  /* double -> int: truncates toward zero */
  reset_vstack();
  push_dbl(-3.9);
  gen_cast_s(VT_INT);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ((int)vtop->c.i, -3);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_gen_cast_more)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* int -> _Bool: any nonzero -> 1, zero -> 0 */
  reset_vstack();
  vpushi(42);
  gen_cast_s(VT_BOOL);
  UT_ASSERT_EQ(vtop->c.i, 1);
  reset_vstack();
  vpushi(0);
  gen_cast_s(VT_BOOL);
  UT_ASSERT_EQ(vtop->c.i, 0);

  /* long long -> int: low word only */
  reset_vstack();
  vpushll(0x100000005LL);
  gen_cast_s(VT_INT);
  UT_ASSERT_EQ((int)vtop->c.i, 5);

  /* unsigned int -> double: 0xFFFFFFFF is 4294967295, not -1 */
  reset_vstack();
  vpushi(-1);
  vtop->type.t |= VT_UNSIGNED;
  gen_cast_s(VT_DOUBLE);
  UT_ASSERT(vtop->c.d == 4294967295.0);

  /* double -> long long */
  reset_vstack();
  push_dbl(1e10);
  gen_cast_s(VT_LLONG);
  UT_ASSERT_EQ(vtop->c.i, 10000000000LL);

  /* float -> double preserves value */
  reset_vstack();
  push_flt(1.5f);
  gen_cast_s(VT_DOUBLE);
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_DOUBLE);
  UT_ASSERT(vtop->c.d == 1.5);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_gen_op_llong_more)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  UT_ASSERT_EQ(fold_ll(0xFF00FF00LL, 0x00FF00FFLL, '&'), 0LL);
  UT_ASSERT_EQ(fold_ll(0xFF00LL, 0x00FFLL, '|'), 0xFFFFLL);
  UT_ASSERT_EQ(fold_ll(0xFFFFLL, 0x0F0FLL, '^'), 0xF0F0LL);
  UT_ASSERT_EQ(fold_ll(3LL, 5LL, TOK_LT), 1);
  UT_ASSERT_EQ(fold_ll(5LL, 3LL, TOK_LT), 0);
  UT_ASSERT_EQ(fold_ll(-1LL, 0LL, TOK_LT), 1); /* signed 64-bit */
  UT_ASSERT_EQ(fold_ll(7LL, 7LL, TOK_EQ), 1);
  UT_ASSERT_EQ(fold_ll(0x7fffffffffffffffLL, 4, TOK_SAR), 0x07ffffffffffffffLL);
  return 0;
}

UT_TEST(test_gen_op_fp_compare)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* double comparisons fold to a 0/1 integer constant */
  reset_vstack();
  push_dbl(1.0);
  push_dbl(2.0);
  gen_op(TOK_LT);
  UT_ASSERT_EQ((int)vtop->c.i, 1);

  reset_vstack();
  push_dbl(2.0);
  push_dbl(1.0);
  gen_op(TOK_LT);
  UT_ASSERT_EQ((int)vtop->c.i, 0);

  reset_vstack();
  push_dbl(1.5);
  push_dbl(1.5);
  gen_op(TOK_EQ);
  UT_ASSERT_EQ((int)vtop->c.i, 1);

  /* float arithmetic folds too */
  reset_vstack();
  push_flt(1.5f);
  push_flt(2.5f);
  gen_op('+');
  UT_ASSERT(vtop->c.f == 4.0f);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_eval_vec_const_op)
{
  UT_ASSERT_EQ(eval_vec_const_op('+', 6, 4, 0), 10);
  UT_ASSERT_EQ(eval_vec_const_op('-', 6, 4, 0), 2);
  UT_ASSERT_EQ(eval_vec_const_op('*', 6, 4, 0), 24);
  UT_ASSERT_EQ(eval_vec_const_op('/', 20, 4, 0), 5);
  UT_ASSERT_EQ(eval_vec_const_op('/', 5, 0, 0), 0); /* div-by-zero guard */
  UT_ASSERT_EQ(eval_vec_const_op('%', 21, 4, 0), 1);
  UT_ASSERT_EQ(eval_vec_const_op('%', 5, 0, 0), 0);
  UT_ASSERT_EQ(eval_vec_const_op('^', 0xF0, 0x0F, 0), 0xFF);
  UT_ASSERT_EQ(eval_vec_const_op('|', 0xF0, 0x0F, 0), 0xFF);
  UT_ASSERT_EQ(eval_vec_const_op('&', 0xF0, 0x3C, 0), 0x30);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_SHL, 1, 4, 0), 16);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_SAR, -16, 2, 0), -4);                              /* signed */
  UT_ASSERT_EQ(eval_vec_const_op(TOK_SAR, -16, 2, 1), (int64_t)((uint64_t)(-16) >> 2)); /* unsigned */
  /* comparisons use the vector-mask convention: all-ones (-1) / 0 */
  UT_ASSERT_EQ(eval_vec_const_op(TOK_EQ, 5, 5, 0), -1);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_NE, 5, 5, 0), 0);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_LT, 3, 5, 0), -1);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_GT, 3, 5, 0), 0);
  UT_ASSERT_EQ(eval_vec_const_op(TOK_ULT, -1, 1, 1), 0); /* (huge) < 1 unsigned -> 0 */
  UT_ASSERT_EQ(eval_vec_const_op(TOK_UGE, -1, 1, 1), -1);
  UT_ASSERT_EQ(eval_vec_const_op(999999, 1, 2, 0), 0); /* unknown op -> 0 */
  return 0;
}

UT_TEST(test_try_fold_math_call)
{
  SValue args[2];

  tcc_state = &ut_dummy_state;
  ut_dummy_state.in_inline_expansion = 0;
  reset_vstack();

  memset(args, 0, sizeof(args));
  args[0].type.t = VT_DOUBLE;
  args[0].r = VT_CONST;
  args[1].type.t = VT_DOUBLE;
  args[1].r = VT_CONST;

  /* sqrt(4.0) -> 2.0 */
  args[0].c.d = 4.0;
  UT_ASSERT_EQ(try_fold_math_call("sqrt", args, 1), 1);
  UT_ASSERT(vtop->c.d == 2.0);
  vpop();

  /* pow(2.0, 10.0) -> 1024.0 */
  args[0].c.d = 2.0;
  args[1].c.d = 10.0;
  UT_ASSERT_EQ(try_fold_math_call("pow", args, 2), 1);
  UT_ASSERT(vtop->c.d == 1024.0);
  vpop();

  /* fabs(-3.0) -> 3.0 */
  args[0].c.d = -3.0;
  UT_ASSERT_EQ(try_fold_math_call("fabs", args, 1), 1);
  UT_ASSERT(vtop->c.d == 3.0);
  vpop();

  /* rejections: unknown name, wrong arg count, non-constant arg */
  args[0].c.d = 4.0;
  UT_ASSERT_EQ(try_fold_math_call("not_a_math_fn", args, 1), 0);
  UT_ASSERT_EQ(try_fold_math_call("sqrt", args, 2), 0);
  args[0].r = VT_LOCAL;
  UT_ASSERT_EQ(try_fold_math_call("sqrt", args, 1), 0);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_read_write_vec_const_elem)
{
  unsigned char buf[8];

  /* round-trip each element width through write then read */
  memset(buf, 0, sizeof(buf));
  write_vec_const_elem(buf, 1, 0, 0x7F);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 1, 0, 0), 0x7F);
  write_vec_const_elem(buf, 1, 0, 0xFF);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 1, 0, 1), 0xFF);        /* unsigned byte */
  UT_ASSERT_EQ(read_vec_const_elem(buf, 1, 0, 0), (int64_t)-1); /* signed byte */

  write_vec_const_elem(buf, 2, 0, 0xFFFF);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 2, 0, 1), 0xFFFF);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 2, 0, 0), (int64_t)-1);

  write_vec_const_elem(buf, 4, 0, 0xFFFFFFFFLL);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 4, 0, 1), 0xFFFFFFFFLL);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 4, 0, 0), (int64_t)-1);

  write_vec_const_elem(buf, 8, 0, 0x1122334455667788LL);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 8, 0, 0), 0x1122334455667788LL);

  /* indexed access: two shorts side by side */
  memset(buf, 0, sizeof(buf));
  write_vec_const_elem(buf, 2, 0, 0x0102);
  write_vec_const_elem(buf, 2, 1, 0x0304);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 2, 0, 1), 0x0102);
  UT_ASSERT_EQ(read_vec_const_elem(buf, 2, 1, 1), 0x0304);

  /* unsupported element size -> 0, and write is a no-op */
  UT_ASSERT_EQ(read_vec_const_elem(buf, 3, 0, 0), 0);
  return 0;
}

UT_TEST(test_condition_3way)
{
  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* a nonzero constant folds to a true (1) condition */
  vpushi(5);
  UT_ASSERT_EQ(condition_3way(), 1);
  UT_ASSERT_EQ(vtop, _vstack + 1); /* vdup/vpop leave the stack balanced */
  vpop();

  /* a zero constant folds to false (0) */
  vpushi(0);
  UT_ASSERT_EQ(condition_3way(), 0);
  vpop();

  /* a non-constant value is unknown (-1) */
  vpushi(0);
  vtop->r = VT_LOCAL;
  UT_ASSERT_EQ(condition_3way(), -1);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_auto_inline_param_count)
{
  Sym ref, p1, p2, fs;
  CType ft;

  /* no ref -> 0 */
  memset(&fs, 0, sizeof(fs));
  fs.type = simple_type(VT_FUNC);
  fs.type.ref = NULL;
  UT_ASSERT_EQ(auto_inline_param_count(&fs), 0);

  /* int f(int, int) -> 2 */
  memset(&ref, 0, sizeof(ref));
  memset(&p1, 0, sizeof(p1));
  memset(&p2, 0, sizeof(p2));
  ref.type = simple_type(VT_INT);
  p1.type = simple_type(VT_INT);
  p2.type = simple_type(VT_INT);
  p1.next = &p2;
  p2.next = NULL;
  ref.next = &p1;
  ft = simple_type(VT_FUNC);
  ft.ref = &ref;
  fs.type = ft;
  UT_ASSERT_EQ(auto_inline_param_count(&fs), 2);

  /* a lone (void) parameter counts as zero parameters */
  p1.type = simple_type(VT_VOID);
  p1.next = NULL;
  ref.next = &p1;
  UT_ASSERT_EQ(auto_inline_param_count(&fs), 0);

  /* a VLA parameter poisons the count -> -1 */
  p1.type = simple_type(VT_PTR | VT_VLA);
  p1.next = NULL;
  UT_ASSERT_EQ(auto_inline_param_count(&fs), -1);
  return 0;
}

UT_TEST(test_struct_is_small_bitfield_word)
{
  Sym bf = sym_for_type(simple_type(VT_INT | VT_BITFIELD), 0, 0);
  Sym head = sym_for_type(simple_type(VT_INT), 4, 4); /* size 4 */
  CType s = simple_type(VT_STRUCT);
  CType notstruct = simple_type(VT_INT);

  bf.next = NULL;
  head.next = &bf;
  s.ref = &head;
  UT_ASSERT(struct_is_small_bitfield_word(&s));

  /* a plain (non-bitfield) field disqualifies it */
  bf.type.t = VT_INT;
  UT_ASSERT(!struct_is_small_bitfield_word(&s));
  bf.type.t = VT_INT | VT_BITFIELD;

  /* size 8 is too large */
  head.c = 8;
  UT_ASSERT(!struct_is_small_bitfield_word(&s));
  head.c = 4;

  /* not a struct at all */
  UT_ASSERT(!struct_is_small_bitfield_word(&notstruct));
  return 0;
}

UT_TEST(test_promote_bitfield_expr_type)
{
  /* build a bitfield btype: base | VT_BITFIELD | (size << (STRUCT_SHIFT+6)) */
#define BF(base, size) ((base) | VT_BITFIELD | ((size) << (VT_STRUCT_SHIFT + 6)))

  /* signed int:5 -> int (bitfield bits stripped) */
  UT_ASSERT_EQ(promote_bitfield_expr_type(BF(VT_INT, 5)), VT_INT);

  /* unsigned int:16 -> plain int (narrower than a full int) */
  UT_ASSERT_EQ(promote_bitfield_expr_type(BF(VT_INT | VT_UNSIGNED, 16)), VT_INT);

  /* unsigned int:32 -> unsigned int (full-width unsigned bitfield stays unsigned) */
  UT_ASSERT_EQ(promote_bitfield_expr_type(BF(VT_INT | VT_UNSIGNED, 32)), (VT_INT | VT_UNSIGNED));

  /* long long:40 keeps its long long base */
  UT_ASSERT_EQ(promote_bitfield_expr_type(BF(VT_LLONG, 40)), VT_LLONG);
  UT_ASSERT_EQ(promote_bitfield_expr_type(BF(VT_LLONG | VT_UNSIGNED, 40)), (VT_LLONG | VT_UNSIGNED));
#undef BF
  return 0;
}

UT_TEST(test_inline_arg_is_constant_like)
{
  SValue sv;

  memset(&sv, 0, sizeof(sv));
  sv.r = VT_CONST;
  UT_ASSERT(inline_arg_is_constant_like(&sv));

  /* an lvalue const (i.e. a memory reference) is not constant-like */
  sv.r = VT_CONST | VT_LVAL;
  UT_ASSERT(!inline_arg_is_constant_like(&sv));

  sv.r = VT_LOCAL;
  UT_ASSERT(!inline_arg_is_constant_like(&sv));
  return 0;
}

UT_TEST(test_force_charshort_cast)
{
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* mustcast==1 (from int): 300 truncated into a signed char == 44 */
  reset_vstack();
  vpushi(300);
  vtop->type.t = VT_BYTE;
  vtop->r |= BFVAL(VT_MUSTCAST, 1);
  force_charshort_cast();
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_BYTE);
  UT_ASSERT_EQ((int)(signed char)vtop->c.i, 44);
  UT_ASSERT_EQ(vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1)), 0); /* bits cleared */

  /* mustcast==2 (from long long): 0x1FFFF truncated into a short == -1 */
  reset_vstack();
  vpushll(0x1FFFFLL);
  vtop->type.t = VT_SHORT;
  vtop->r |= BFVAL(VT_MUSTCAST, 2);
  force_charshort_cast();
  UT_ASSERT_EQ(vtop->type.t & VT_BTYPE, VT_SHORT);
  UT_ASSERT_EQ((int)(short)vtop->c.i, -1);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_objsize_vreg_facts)
{
  static TCCIRState other_ir;
  unsigned long long m;

  /* start from a clean fact table bound to no IR */
  objsize_fact_ir = NULL;
  objsize_vreg_fact_count = 0;

  /* record a max-only fact for vreg 5 */
  objsize_vreg_fact_record(&ut_fake_ir, 5, 1, 100, 0, 0);
  m = 0;
  UT_ASSERT(objsize_vreg_fact_get_max(&ut_fake_ir, 5, &m));
  UT_ASSERT_EQ((int)m, 100);
  UT_ASSERT(!objsize_vreg_fact_get_strlen(&ut_fake_ir, 5, &m)); /* strlen invalid */

  /* updating the same vreg overwrites in place (no new slot) */
  objsize_vreg_fact_record(&ut_fake_ir, 5, 1, 200, 1, 7);
  UT_ASSERT_EQ(objsize_vreg_fact_count, 1);
  UT_ASSERT(objsize_vreg_fact_get_max(&ut_fake_ir, 5, &m));
  UT_ASSERT_EQ((int)m, 200);
  UT_ASSERT(objsize_vreg_fact_get_strlen(&ut_fake_ir, 5, &m));
  UT_ASSERT_EQ((int)m, 7);

  /* an unknown vreg is not found */
  UT_ASSERT(!objsize_vreg_fact_get_max(&ut_fake_ir, 99, &m));

  /* NULL ir / negative vreg guards on the getters and recorder */
  UT_ASSERT(!objsize_vreg_fact_get_max(NULL, 5, &m));
  UT_ASSERT(!objsize_vreg_fact_get_strlen(NULL, 5, &m));
  objsize_vreg_fact_record(NULL, 5, 1, 1, 0, 0); /* no-op */
  objsize_vreg_fact_record(&ut_fake_ir, -1, 1, 1, 0, 0);
  UT_ASSERT_EQ(objsize_vreg_fact_count, 1); /* unchanged */

  /* switching to a different IR flushes the table */
  UT_ASSERT(!objsize_vreg_fact_get_max(&other_ir, 5, &m));
  UT_ASSERT_EQ(objsize_vreg_fact_count, 0);

  objsize_fact_ir = NULL;
  objsize_vreg_fact_count = 0;
  return 0;
}

UT_TEST(test_find_local_scalar_sym)
{
  Sym scalar, aggregate;
  SValue sv;

  reset_symbol_state();
  memset(&scalar, 0, sizeof(scalar));
  scalar.r = VT_LOCAL;
  scalar.type.t = VT_INT;
  scalar.v = TOK_IDENT + 1;
  scalar.c = 0x20;
  scalar.prev = NULL;
  local_stack = &scalar;

  UT_ASSERT_EQ(find_local_scalar_sym_by_offset(0x20), &scalar);
  UT_ASSERT(find_local_scalar_sym_by_offset(0x99) == NULL);

  /* a struct local is skipped by the scalar filter */
  scalar.type.t = VT_STRUCT;
  UT_ASSERT(find_local_scalar_sym_by_offset(0x20) == NULL);
  /* an array local is skipped too */
  scalar.type.t = VT_PTR | VT_ARRAY;
  UT_ASSERT(find_local_scalar_sym_by_offset(0x20) == NULL);
  scalar.type.t = VT_INT;

  /* a struct-field / struct-tag symbol at the same offset is skipped */
  scalar.v = SYM_FIELD | (TOK_IDENT + 1);
  UT_ASSERT(find_local_scalar_sym_by_offset(0x20) == NULL);
  scalar.v = TOK_IDENT + 1;

  /* a non-VT_LOCAL entry on the stack is skipped */
  scalar.r = VT_CONST;
  UT_ASSERT(find_local_scalar_sym_by_offset(0x20) == NULL);
  scalar.r = VT_LOCAL;

  /* find_local_scalar_sym_for_svalue: offset lookup when sv has no sym */
  memset(&sv, 0, sizeof(sv));
  sv.r = VT_LOCAL;
  sv.c.i = 0x20;
  sv.sym = NULL;
  UT_ASSERT_EQ(find_local_scalar_sym_for_svalue(&sv), &scalar);

  /* a directly attached local sym is returned as-is */
  memset(&aggregate, 0, sizeof(aggregate));
  aggregate.r = VT_LOCAL;
  sv.sym = &aggregate;
  UT_ASSERT_EQ(find_local_scalar_sym_for_svalue(&sv), &aggregate);

  /* a non-local svalue resolves to nothing */
  sv.r = VT_CONST;
  sv.sym = NULL;
  UT_ASSERT(find_local_scalar_sym_for_svalue(&sv) == NULL);

  local_stack = NULL;
  return 0;
}

UT_TEST(test_svalue_get_conservative_max_u64)
{
  SValue sv;
  Sym local;
  unsigned long long m;

  tcc_state = NULL; /* keep the vreg-fact fallback inert */

  /* a plain non-negative int constant is its own max */
  memset(&sv, 0, sizeof(sv));
  sv.vr = -1;
  sv.r = VT_CONST;
  sv.type.t = VT_INT;
  sv.c.i = 42;
  UT_ASSERT(svalue_get_conservative_max_u64(&sv, &m));
  UT_ASSERT_EQ((int)m, 42);

  /* A signed int holding the -1 bit pattern is rejected (no usable bound):
     the negative-value guard reinterprets the value as (int64_t) so the
     comparison actually fires. */
  sv.c.i = (uint64_t)(int64_t)-1;
  UT_ASSERT(!svalue_get_conservative_max_u64(&sv, &m));

  /* the same bit pattern read as unsigned is a (huge) valid max */
  sv.type.t = VT_INT | VT_UNSIGNED;
  UT_ASSERT(svalue_get_conservative_max_u64(&sv, &m));
  UT_ASSERT_EQ(m, (unsigned long long)(int64_t)-1);

  /* a pointer constant is likewise reported, not rejected */
  sv.type.t = VT_PTR;
  UT_ASSERT(svalue_get_conservative_max_u64(&sv, &m));

  /* a local scalar with a recorded objsize max reports it */
  reset_symbol_state();
  memset(&local, 0, sizeof(local));
  local.r = VT_LOCAL;
  local.type.t = VT_INT;
  local.v = TOK_IDENT + 1;
  local.c = 0x30;
  local.objsize_max_valid = 1;
  local.objsize_max_value = 500;
  local.prev = NULL;
  local_stack = &local;

  memset(&sv, 0, sizeof(sv));
  sv.vr = -1;
  sv.r = VT_LOCAL;
  sv.c.i = 0x30;
  UT_ASSERT(svalue_get_conservative_max_u64(&sv, &m));
  UT_ASSERT_EQ((int)m, 500);

  /* an unbounded local yields nothing */
  local.objsize_max_valid = 0;
  UT_ASSERT(!svalue_get_conservative_max_u64(&sv, &m));

  local_stack = NULL;
  return 0;
}

UT_TEST(test_svalue_get_conservative_string_bytes_u64)
{
  SValue sv;
  Sym local;
  unsigned long long m;

  tcc_state = NULL;

  /* a local scalar with a recorded strlen fact reports (len) directly */
  reset_symbol_state();
  memset(&local, 0, sizeof(local));
  local.r = VT_LOCAL;
  local.type.t = VT_INT;
  local.v = TOK_IDENT + 1;
  local.c = 0x40;
  local.objsize_strlen_valid = 1;
  local.objsize_strlen_value = 9;
  local.prev = NULL;
  local_stack = &local;

  memset(&sv, 0, sizeof(sv));
  sv.vr = -1;
  sv.r = VT_LOCAL;
  sv.c.i = 0x40;
  UT_ASSERT(svalue_get_conservative_string_bytes_u64(&sv, &m));
  UT_ASSERT_EQ((int)m, 9);

  /* without a strlen fact there is no bound */
  local.objsize_strlen_valid = 0;
  UT_ASSERT(!svalue_get_conservative_string_bytes_u64(&sv, &m));

  /* a bare (non-local, non-string) constant has no string bound */
  memset(&sv, 0, sizeof(sv));
  sv.vr = -1;
  sv.r = VT_CONST;
  sv.type.t = VT_INT;
  sv.c.i = 5;
  UT_ASSERT(!svalue_get_conservative_string_bytes_u64(&sv, &m));

  local_stack = NULL;
  return 0;
}

UT_TEST(test_adjust_bf)
{
  SValue sv;
  Sym ref;

  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_INT;

  /* no auxtype ref -> 0, nothing touched */
  sv.type.ref = NULL;
  UT_ASSERT_EQ(adjust_bf(&sv, 0, 8), 0);

  memset(&ref, 0, sizeof(ref));
  sv.type.ref = &ref;

  /* auxtype -1 ("use the field's own base type") -> returned as-is, no relabel */
  ref.auxtype = -1;
  sv.type.t = VT_INT;
  sv.r = 0;
  UT_ASSERT_EQ(adjust_bf(&sv, 0, 8), -1);
  UT_ASSERT_EQ(sv.type.t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ(sv.r & VT_LVAL, 0);

  /* auxtype VT_STRUCT (byte-wise access) -> returned, no relabel */
  ref.auxtype = VT_STRUCT;
  sv.type.t = VT_INT;
  UT_ASSERT_EQ(adjust_bf(&sv, 0, 8), VT_STRUCT);
  UT_ASSERT_EQ(sv.type.t & VT_BTYPE, VT_INT);

  /* a concrete access type relabels the btype and forces an lvalue */
  ref.auxtype = VT_SHORT;
  sv.type.t = VT_INT;
  sv.r = 0;
  UT_ASSERT_EQ(adjust_bf(&sv, 0, 8), VT_SHORT);
  UT_ASSERT_EQ(sv.type.t & VT_BTYPE, VT_SHORT);
  UT_ASSERT(sv.r & VT_LVAL);
  return 0;
}

UT_TEST(test_struct_member_copy_safe)
{
  Sym bf, other, head;
  CType s = simple_type(VT_STRUCT);
  CType notstruct = simple_type(VT_INT);

  /* struct { int x:4; } -> bitfield word, unit width 4 -> safe */
  bf = sym_for_type(simple_type(VT_INT | VT_BITFIELD), 0, 0);
  bf.type.ref = NULL; /* bitfield_unit_width falls back to the base type (4) */
  bf.next = NULL;
  head = sym_for_type(simple_type(VT_INT), 4, 4);
  head.next = &bf;
  s.ref = &head;
  UT_ASSERT(struct_member_copy_safe(&s));

  /* an 8-byte scalar member (double) makes the copy unsafe */
  other = sym_for_type(simple_type(VT_DOUBLE), 8, 0);
  other.next = NULL;
  bf.next = &other;
  UT_ASSERT(!struct_member_copy_safe(&s));

  /* a 64-bit bitfield (unit width 8) is likewise an unsafe straddle */
  {
    Sym llbf = sym_for_type(simple_type(VT_LLONG | VT_BITFIELD), 0, 0);
    llbf.type.ref = NULL; /* unit width falls back to the base type (8) */
    llbf.next = NULL;
    head.next = &llbf;
    UT_ASSERT(!struct_member_copy_safe(&s));
    head.next = &bf;
  }

  /* only plain scalar members (no bitfield seen) -> not this shape (0) */
  other.type = simple_type(VT_INT);
  head.next = &other;
  other.next = NULL;
  UT_ASSERT(!struct_member_copy_safe(&s));

  /* not a struct at all */
  UT_ASSERT(!struct_member_copy_safe(&notstruct));
  return 0;
}

UT_TEST(test_find_assignable_transparent_union_member)
{
  Sym member, head;
  CType u = simple_type(VT_STRUCT);
  CType plain = simple_type(VT_INT);

  reset_vstack();
  vpushi(0); /* vtop is an int rvalue */

  member = sym_for_type(simple_type(VT_INT), 0, 0);
  member.next = NULL;
  memset(&head, 0, sizeof(head));
  head.a.transparent_union = 1;
  head.type.t = VT_UNION;
  head.next = &member;
  u.ref = &head;

  /* int rvalue matches the union's int member */
  UT_ASSERT_EQ(find_assignable_transparent_union_member(&u), &member.type);

  /* a non-transparent union has no assignable member */
  head.a.transparent_union = 0;
  UT_ASSERT(find_assignable_transparent_union_member(&u) == NULL);
  head.a.transparent_union = 1;

  /* no member is type-compatible with the int -> NULL */
  member.type = simple_type(VT_DOUBLE);
  UT_ASSERT(find_assignable_transparent_union_member(&u) == NULL);

  /* a pointer member matches a null-pointer-constant rvalue via the ptr path */
  {
    Sym pref = sym_for_type(simple_type(VT_INT), -1, 0);
    Sym pmember;
    CType ptrmem = simple_type(VT_PTR);
    ptrmem.ref = &pref;
    pmember = sym_for_type(ptrmem, 0, 0);
    pmember.next = NULL;
    head.next = &pmember;

    reset_vstack();
    vpushi(0); /* a null pointer constant (VT_CONST, value 0) */
    UT_ASSERT_EQ(find_assignable_transparent_union_member(&u), &pmember.type);
  }

  /* a plain (non-union) type is never transparent */
  UT_ASSERT(find_assignable_transparent_union_member(&plain) == NULL);
  return 0;
}

UT_TEST(test_token_stream_references_local_object)
{
  Sym loc;
  int s_yes[] = {TOK_IDENT + 1, 0};
  int s_punct[] = {'+', '*', 0};

  reset_token_table();
  memset(&loc, 0, sizeof(loc));
  loc.type.t = VT_INT;
  loc.sym_scope = 1; /* an in-scope local */
  ut_tok_a.sym_identifier = &loc;

  UT_ASSERT(token_stream_references_local_object(s_yes));
  /* punctuation tokens (< TOK_IDENT) are ignored */
  UT_ASSERT(!token_stream_references_local_object(s_punct));

  /* typedef / function / file-scope symbols are not "local objects" */
  loc.type.t = VT_INT | VT_TYPEDEF;
  UT_ASSERT(!token_stream_references_local_object(s_yes));
  loc.type.t = VT_FUNC;
  UT_ASSERT(!token_stream_references_local_object(s_yes));
  loc.type.t = VT_INT;
  loc.sym_scope = 0; /* global scope */
  UT_ASSERT(!token_stream_references_local_object(s_yes));

  ut_tok_a.sym_identifier = NULL;
  return 0;
}

UT_TEST(test_inline_body_shadowed_ident)
{
  Sym loc;
  int toks[] = {TOK_IDENT + 1, 0};
  int punct[] = {'+', 0};
  TokenString ts;

  reset_token_table();
  memset(&loc, 0, sizeof(loc));
  loc.type.t = VT_INT;
  loc.sym_scope = 1;
  loc.prev_tok = &loc; /* a matching global also exists for this token */
  ut_tok_a.sym_identifier = &loc;

  UT_ASSERT(!inline_body_has_shadowed_ident(NULL)); /* NULL guard */

  ts = make_tokstr(toks);
  UT_ASSERT(inline_body_has_shadowed_ident(&ts)); /* local shadows a global */

  /* no shadowed global (prev_tok NULL) -> not flagged */
  loc.prev_tok = NULL;
  UT_ASSERT(!inline_body_has_shadowed_ident(&ts));
  loc.prev_tok = &loc;

  /* file-scope symbol (sym_scope 0) -> not a shadow */
  loc.sym_scope = 0;
  UT_ASSERT(!inline_body_has_shadowed_ident(&ts));
  loc.sym_scope = 1;

  /* only punctuation -> nothing to shadow */
  ts = make_tokstr(punct);
  UT_ASSERT(!inline_body_has_shadowed_ident(&ts));

  /* the "unsafe" wrapper defers to the strict check at the top level, but
     stays quiet while already inside an inline expansion */
  ts = make_tokstr(toks);
  tcc_state = &ut_dummy_state;
  ut_dummy_state.in_inline_expansion = 0;
  UT_ASSERT(inline_body_has_unsafe_shadowed_ident(&ts, NULL));
  ut_dummy_state.in_inline_expansion = 1;
  UT_ASSERT(!inline_body_has_unsafe_shadowed_ident(&ts, NULL));
  ut_dummy_state.in_inline_expansion = 0;
  tcc_state = NULL;

  ut_tok_a.sym_identifier = NULL;
  return 0;
}

UT_TEST(test_sym_copy_ref)
{
  Sym s, pointee;
  Sym *ps = NULL;

  /* a pointer type: its type.ref chain is deep-copied onto ps */
  memset(&s, 0, sizeof(s));
  memset(&pointee, 0, sizeof(pointee));
  pointee.v = SYM_FIRST_ANOM; /* anonymous -> skips the table_ident path */
  pointee.type.t = VT_INT;
  pointee.next = NULL;
  s.type.t = VT_PTR;
  s.type.ref = &pointee;

  sym_copy_ref(&s, &ps);
  UT_ASSERT(ps != NULL);
  UT_ASSERT(s.type.ref != &pointee);            /* ref now points at the copy */
  UT_ASSERT_EQ(s.type.ref, ps);                 /* which is what was pushed */
  UT_ASSERT_EQ(s.type.ref->type.t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ((unsigned)s.type.ref->v, (unsigned)SYM_FIRST_ANOM);

  /* a plain scalar type has no ref chain to copy -> ps untouched */
  {
    Sym scalar;
    Sym *ps2 = NULL;
    memset(&scalar, 0, sizeof(scalar));
    scalar.type.t = VT_INT;
    sym_copy_ref(&scalar, &ps2);
    UT_ASSERT(ps2 == NULL);
  }
  return 0;
}

UT_TEST(test_patch_storage)
{
  Sym sym;
  AttributeDef ad;

  reset_symbol_state();
  tcc_state = &ut_dummy_state;
  symtab_section = &ut_symtab_section;
  ut_symtab_section.data = (unsigned char *)ut_symtab_data;

  memset(&sym, 0, sizeof(sym));
  sym.v = TOK_IDENT + 1;
  sym.type.t = VT_INT;
  sym.c = 0; /* no ELF entry yet -> update_storage() is a no-op */

  memset(&ad, 0, sizeof(ad));
  ad.a.weak = 1;
  ad.a.visibility = STV_HIDDEN;
  ad.asm_label = TOK_IDENT + 2;

  /* type == NULL skips patch_type; the attribute merge + asm_label still run */
  patch_storage(&sym, &ad, NULL);
  UT_ASSERT_EQ(sym.a.weak, 1);
  UT_ASSERT_EQ(sym.a.visibility, STV_HIDDEN);
  UT_ASSERT_EQ(sym.asm_label, TOK_IDENT + 2);

  symtab_section = NULL;
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_gen_test_zero)
{
  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* VT_CMP + TOK_EQ inverts the comparison: swap jtrue/jfalse, flip cmp_op b0 */
  vpushi(0);
  vtop->r = VT_CMP;
  vtop->cmp_op = TOK_NE;
  vtop->jtrue = 11;
  vtop->jfalse = 22;
  gen_test_zero(TOK_EQ);
  UT_ASSERT_EQ(vtop->jtrue, 22);
  UT_ASSERT_EQ(vtop->jfalse, 11);
  UT_ASSERT_EQ(vtop->cmp_op, TOK_NE ^ 1);

  /* VT_CMP + TOK_NE leaves the comparison untouched */
  reset_vstack();
  vpushi(0);
  vtop->r = VT_CMP;
  vtop->cmp_op = TOK_NE;
  vtop->jtrue = 5;
  vtop->jfalse = 6;
  gen_test_zero(TOK_NE);
  UT_ASSERT_EQ(vtop->jtrue, 5);
  UT_ASSERT_EQ(vtop->jfalse, 6);
  UT_ASSERT_EQ(vtop->cmp_op, TOK_NE);

  /* a plain constant compares against 0 via the fold path */
  reset_vstack();
  vpushi(5);
  gen_test_zero(TOK_EQ);
  UT_ASSERT_EQ((int)vtop->c.i, 0); /* 5 == 0 -> false */
  reset_vstack();
  vpushi(0);
  gen_test_zero(TOK_EQ);
  UT_ASSERT_EQ((int)vtop->c.i, 1); /* 0 == 0 -> true */

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_check_fields)
{
  Sym field, head;
  Sym anon_field, nested_field, nested_head;
  CType s = simple_type(VT_STRUCT);

  reset_token_table();
  ut_tok_a.tok = TOK_IDENT + 1; /* no SYM_FIELD bit yet */

  /* struct { int a; } -> check_fields toggles the member's SYM_FIELD marker */
  field = sym_for_type(simple_type(VT_INT), 0, 0);
  field.v = TOK_IDENT + 1;
  field.next = NULL;
  memset(&head, 0, sizeof(head));
  head.next = &field;
  s.ref = &head;

  check_fields(&s, 1); /* first pass: set the marker (no duplicate error) */
  UT_ASSERT(ut_tok_a.tok & SYM_FIELD);
  check_fields(&s, 0); /* clearing pass: toggle it back off */
  UT_ASSERT(!(ut_tok_a.tok & SYM_FIELD));

  /* an anonymous (v >= SYM_FIRST_ANOM) nested struct field recurses; a plain
     anonymous field is skipped.  Give the nested struct a named member. */
  ut_tok_b.tok = TOK_IDENT + 2;
  nested_field = sym_for_type(simple_type(VT_INT), 0, 0);
  nested_field.v = TOK_IDENT + 2;
  nested_field.next = NULL;
  memset(&nested_head, 0, sizeof(nested_head));
  nested_head.next = &nested_field;

  anon_field = sym_for_type(simple_type(VT_STRUCT), 0, 0);
  anon_field.v = SYM_FIRST_ANOM;
  anon_field.type.ref = &nested_head;
  anon_field.next = NULL;
  head.next = &anon_field;

  check_fields(&s, 0); /* recurses into the anonymous struct, toggles member 2 */
  UT_ASSERT(ut_tok_b.tok & SYM_FIELD);
  ut_tok_b.tok = TOK_IDENT + 2; /* restore */
  return 0;
}

UT_TEST(test_find_global_alias_target_sym)
{
  Sym g, gb, loc, loc2, ga;

  /* (A) an unscoped identifier resolves directly */
  reset_symbol_state();
  memset(&g, 0, sizeof(g));
  g.sym_scope = 0;
  g.v = TOK_IDENT + 1;
  ut_tok_a.sym_identifier = &g;
  UT_ASSERT_EQ(find_global_alias_target_sym(TOK_IDENT + 1), &g);

  /* (B) a local hides the global; the prev_tok chain walks up to it */
  memset(&gb, 0, sizeof(gb));
  gb.sym_scope = 0;
  memset(&loc, 0, sizeof(loc));
  loc.sym_scope = 2;
  loc.prev_tok = &gb;
  ut_tok_a.sym_identifier = &loc;
  UT_ASSERT_EQ(find_global_alias_target_sym(TOK_IDENT + 1), &gb);

  /* (C) no visible global by name -> fall back to an asm_label match on the
     global stack */
  memset(&loc2, 0, sizeof(loc2));
  loc2.sym_scope = 1;
  loc2.prev_tok = NULL;
  ut_tok_a.sym_identifier = &loc2;
  memset(&ga, 0, sizeof(ga));
  ga.sym_scope = 0;
  ga.asm_label = TOK_IDENT + 1;
  ga.prev = NULL;
  global_stack = &ga;
  UT_ASSERT_EQ(find_global_alias_target_sym(TOK_IDENT + 1), &ga);

  /* (D) nothing matches -> NULL */
  ut_tok_a.sym_identifier = NULL;
  global_stack = NULL;
  UT_ASSERT(find_global_alias_target_sym(TOK_IDENT + 1) == NULL);
  return 0;
}

UT_TEST(test_alias_resolution)
{
  Sym alias1, alias2, alias3, alias4, target;

  reset_symbol_state();
  tcc_state = &ut_dummy_state;
  symtab_section = &ut_symtab_section;
  memset(ut_symtab_data, 0, sizeof(ut_symtab_data));
  ut_symtab_section.data = (unsigned char *)ut_symtab_data;
  pending_aliases = NULL;
  nb_pending_aliases = 0;

  /* queue_alias_symbol appends to the pending list */
  memset(&alias1, 0, sizeof(alias1));
  alias1.v = TOK_IDENT + 1;
  queue_alias_symbol(&alias1, TOK_IDENT + 3);
  UT_ASSERT_EQ(nb_pending_aliases, 1);
  UT_ASSERT_EQ(pending_aliases[0].alias_sym, &alias1);
  UT_ASSERT_EQ(pending_aliases[0].target_tok, TOK_IDENT + 3);

  /* resolve_alias_symbol against an undefined target -> 0 (report suppressed) */
  memset(&alias2, 0, sizeof(alias2));
  alias2.v = TOK_IDENT + 1;
  UT_ASSERT_EQ(resolve_alias_symbol(&alias2, TOK_IDENT + 3, 0), 0);

  /* apply_alias_attribute queues an unresolvable alias */
  nb_pending_aliases = 0;
  apply_alias_attribute(&alias2, TOK_IDENT + 3);
  UT_ASSERT_EQ(nb_pending_aliases, 1);

  /* set up a defined target (ELF sym #1 in a real section) and resolve it */
  memset(&target, 0, sizeof(target));
  target.sym_scope = 0;
  target.v = TOK_IDENT + 2;
  target.c = 1;
  ut_tok_b.sym_identifier = &target;
  ut_symtab_data[1].st_shndx = 5;
  ut_symtab_data[1].st_value = 0x1000;
  ut_symtab_data[1].st_size = 16;

  memset(&alias3, 0, sizeof(alias3));
  alias3.v = TOK_IDENT + 1;
  alias3.type.t = VT_INT;
  UT_ASSERT_EQ(resolve_alias_symbol(&alias3, TOK_IDENT + 2, 0), 1);

  /* resolve_pending_aliases drains a resolvable queue to empty */
  nb_pending_aliases = 0;
  pending_aliases = NULL;
  memset(&alias4, 0, sizeof(alias4));
  alias4.v = TOK_IDENT + 1;
  alias4.type.t = VT_INT;
  queue_alias_symbol(&alias4, TOK_IDENT + 2);
  resolve_pending_aliases();
  UT_ASSERT_EQ(nb_pending_aliases, 0);
  UT_ASSERT(pending_aliases == NULL);

  /* an empty queue is a clean no-op */
  resolve_pending_aliases();
  UT_ASSERT_EQ(nb_pending_aliases, 0);

  ut_tok_b.sym_identifier = NULL;
  symtab_section = NULL;
  tcc_state = NULL;
  global_stack = NULL;
  return 0;
}

UT_TEST(test_try_inline_builtin_call)
{
  SValue args[1];

  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  memset(args, 0, sizeof(args));
  args[0].r = VT_CONST;
  args[0].type.t = VT_INT;
  args[0].c.i = (uint64_t)(int64_t)-5;

  /* abs(-5) folds to 5 */
  UT_ASSERT_EQ(try_inline_builtin_call("abs", args, 1), 1);
  UT_ASSERT_EQ((int)vtop->c.i, 5);

  /* wrong argument count is not inlined */
  UT_ASSERT_EQ(try_inline_builtin_call("abs", args, 2), 0);
  /* an unrelated function name is not an abs builtin */
  UT_ASSERT_EQ(try_inline_builtin_call("frobnicate", args, 1), 0);

  /* llabs(-10) folds via the 64-bit path */
  reset_vstack();
  args[0].type.t = VT_LLONG;
  args[0].c.i = (uint64_t)(int64_t)-10;
  UT_ASSERT_EQ(try_inline_builtin_call("llabs", args, 1), 1);
  UT_ASSERT_EQ((long long)vtop->c.i, 10);

  /* uabs(-5) folds to 5 and yields an unsigned result */
  reset_vstack();
  args[0].type.t = VT_INT;
  args[0].c.i = (uint64_t)(int64_t)-5;
  UT_ASSERT_EQ(try_inline_builtin_call("uabs", args, 1), 1);
  UT_ASSERT_EQ((int)vtop->c.i, 5);
  UT_ASSERT(vtop->type.t & VT_UNSIGNED);

  /* ullabs(-10LL) folds via the 64-bit unsigned path */
  reset_vstack();
  args[0].type.t = VT_LLONG;
  args[0].c.i = (uint64_t)(int64_t)-10;
  UT_ASSERT_EQ(try_inline_builtin_call("ullabs", args, 1), 1);
  UT_ASSERT_EQ((long long)vtop->c.i, 10);
  UT_ASSERT(vtop->type.t & VT_UNSIGNED);

  tcc_state = NULL;
  return 0;
}

UT_TEST(test_scope_new_and_leave)
{
  struct scope root, child;
  int base_scope;

  reset_vstack();
  reset_symbol_state();

  memset(&root, 0, sizeof(root));
  cur_scope = &root;
  base_scope = local_scope;

  /* new_scope copies+links the parent, resets VLA state, snapshots the stacks.
     (leave_scope() is intentionally not exercised here: its cleanup-inline path
     keeps decl() and the whole declaration parser reachable under
     --gc-sections, which this isolated binary deliberately excludes.) */
  new_scope(&child);
  UT_ASSERT_EQ(cur_scope, &child);
  UT_ASSERT_EQ(child.prev, &root);
  UT_ASSERT_EQ(child.vla.num, 0);
  UT_ASSERT_EQ(child.vla.loc, 0);
  UT_ASSERT_EQ(child.lstk, local_stack);
  UT_ASSERT_EQ(child.llstk, local_label_stack);
  UT_ASSERT_EQ(local_scope, base_scope + 1);

  cur_scope = NULL;
  local_scope = base_scope;
  return 0;
}

UT_TEST(test_vla_restore_and_leave)
{
  struct scope stop_scope, mid, top;

  reset_vstack();
  tcc_state = &ut_dummy_state;
  ut_dummy_state.ir = &ut_fake_ir;

  /* vla_restore(0) is a no-op (no slot to restore) */
  ut_last_ir_op = (TccIrOp)-1;
  vla_restore(0);
  UT_ASSERT_EQ((int)ut_last_ir_op, -1);

  /* vla_restore(loc) emits a VLA_SP_RESTORE against the given slot */
  vla_restore(0x40);
  UT_ASSERT_EQ(ut_last_ir_op, TCCIR_OP_VLA_SP_RESTORE);

  /* vla_leave restores SP when a scope in the unwound range declared a VLA */
  memset(&stop_scope, 0, sizeof(stop_scope));
  memset(&mid, 0, sizeof(mid));
  memset(&top, 0, sizeof(top));
  mid.prev = &stop_scope;
  top.prev = &mid;
  mid.vla.num = 1;
  mid.vla.locorig = 0x88;

  cur_scope = &top;
  ut_last_ir_op = (TccIrOp)-1;
  vla_leave(&stop_scope);
  UT_ASSERT_EQ(ut_last_ir_op, TCCIR_OP_VLA_SP_RESTORE);

  /* no VLA in the range -> nothing emitted */
  mid.vla.num = 0;
  cur_scope = &top;
  ut_last_ir_op = (TccIrOp)-1;
  vla_leave(&stop_scope);
  UT_ASSERT_EQ((int)ut_last_ir_op, -1);

  cur_scope = NULL;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_get_temp_local_var)
{
  int vr, l0, l1, l2, l3;
  int i;

  reset_vstack();
  nb_temp_local_vars = 0;
  loc = 0;

  /* first request allocates a fresh temp slot just below the frame */
  l0 = get_temp_local_var(4, 4, &vr);
  UT_ASSERT_EQ(nb_temp_local_vars, 1);
  UT_ASSERT_EQ(vr, VR_TEMP_LOCAL(0));
  UT_ASSERT_EQ(l0, -4); /* (0 - 4) & -4 */

  /* an identical request with the slot idle reuses slot 0 (no growth) */
  l1 = get_temp_local_var(4, 4, &vr);
  UT_ASSERT_EQ(nb_temp_local_vars, 1);
  UT_ASSERT_EQ(vr, VR_TEMP_LOCAL(0));
  UT_ASSERT_EQ(l1, -4);

  /* mark slot 0 as live on the value stack -> next request must grow */
  reset_vstack();
  vpushi(0);
  vtop->r = VT_LOCAL;
  vtop->vr = VR_TEMP_LOCAL(0);
  l2 = get_temp_local_var(4, 4, &vr);
  UT_ASSERT_EQ(nb_temp_local_vars, 2);
  UT_ASSERT_EQ(vr, VR_TEMP_LOCAL(1));
  UT_ASSERT_EQ(l2, -8);

  /* a larger request fits neither existing slot -> grows again */
  l3 = get_temp_local_var(8, 8, &vr);
  UT_ASSERT_EQ(nb_temp_local_vars, 3);
  UT_ASSERT_EQ(vr, VR_TEMP_LOCAL(2));
  UT_ASSERT_EQ(l3, -16); /* (-8 - 8) & -8 */

  /* exhaustion: keep every slot live so no reuse happens, fill to the cap,
     then the next request has no slot to hand out (vr_out = -1) */
  reset_vstack();
  for (i = 0; i < MAX_TEMP_LOCAL_VARIABLE_NUMBER; i++)
  {
    vpushi(0);
    vtop->r = VT_LOCAL;
    vtop->vr = VR_TEMP_LOCAL(i);
  }
  nb_temp_local_vars = 0;
  loc = 0;
  for (i = 0; i < MAX_TEMP_LOCAL_VARIABLE_NUMBER; i++)
    get_temp_local_var(4, 4, &vr);
  UT_ASSERT_EQ(nb_temp_local_vars, MAX_TEMP_LOCAL_VARIABLE_NUMBER);
  get_temp_local_var(4, 4, &vr);
  UT_ASSERT_EQ(vr, -1);

  nb_temp_local_vars = 0;
  return 0;
}

UT_TEST(test_nested_callee_has_genuine_capture)
{
  static NestedFunc nf;
  Sym fsym, fref, param, other;
  int body[] = {TOK_INT, TOK_IDENT + 5, 0};
  TokenString ts;

  tcc_state = &ut_dummy_state;
  reset_token_table();

  memset(&nf, 0, sizeof(nf));
  memset(&fsym, 0, sizeof(fsym));
  memset(&fref, 0, sizeof(fref));
  memset(&param, 0, sizeof(param));
  fsym.type.ref = &fref; /* function with no parameters yet */
  fref.next = NULL;
  nf.sym = &fsym;
  nf.nb_captured = 1;
  nf.captured_tokens[0] = TOK_IDENT + 5;
  nf.func_str = NULL;

  ut_dummy_state.nested_funcs = &nf;
  ut_dummy_state.nb_nested_funcs = 1;

  /* an unshadowed captured variable is a genuine capture */
  UT_ASSERT_EQ(nested_callee_has_genuine_capture(&ut_dummy_state, &fsym), 1);

  /* shadowed by a same-named parameter -> not genuine */
  param.v = TOK_IDENT + 5;
  param.next = NULL;
  fref.next = &param;
  UT_ASSERT_EQ(nested_callee_has_genuine_capture(&ut_dummy_state, &fsym), 0);
  fref.next = NULL;

  /* shadowed by a body-local declaration ("int x;") -> not genuine */
  ts = make_tokstr(body);
  nf.func_str = &ts;
  UT_ASSERT_EQ(nested_callee_has_genuine_capture(&ut_dummy_state, &fsym), 0);
  nf.func_str = NULL;

  /* a callee absent from the nested table -> 0 */
  memset(&other, 0, sizeof(other));
  UT_ASSERT_EQ(nested_callee_has_genuine_capture(&ut_dummy_state, &other), 0);

  /* no captures at all -> 0 */
  nf.nb_captured = 0;
  UT_ASSERT_EQ(nested_callee_has_genuine_capture(&ut_dummy_state, &fsym), 0);

  ut_dummy_state.nested_funcs = NULL;
  ut_dummy_state.nb_nested_funcs = 0;
  tcc_state = NULL;
  return 0;
}

UT_TEST(test_nested_callee_captures_reachable)
{
  static NestedFunc nfs[3];
  Sym symA, symB, symC, sym_unrel;

  tcc_state = &ut_dummy_state;

  /* chain: A (top) <- B <- C */
  memset(nfs, 0, sizeof(nfs));
  nfs[0].sym = &symA;
  nfs[1].sym = &symB;
  nfs[2].sym = &symC;
  nfs[0].parent_nf = NULL;
  nfs[1].parent_nf = &nfs[0];
  nfs[2].parent_nf = &nfs[1];

  ut_dummy_state.nested_funcs = nfs;
  ut_dummy_state.nb_nested_funcs = 3;

  /* callee not in the table -> unreachable */
  UT_ASSERT_EQ(nested_callee_captures_reachable(&ut_dummy_state, &sym_unrel, NULL), 0);

  /* top-level caller (current_nf NULL): any nested callee is reachable */
  UT_ASSERT_EQ(nested_callee_captures_reachable(&ut_dummy_state, &symB, NULL), 1);

  /* direct child: callee B's parent A == current A */
  UT_ASSERT_EQ(nested_callee_captures_reachable(&ut_dummy_state, &symB, &nfs[0]), 1);

  /* ancestor: callee B's parent A is on current C's parent chain (C->B->A) */
  UT_ASSERT_EQ(nested_callee_captures_reachable(&ut_dummy_state, &symB, &nfs[2]), 1);

  /* sibling/unreachable: callee C's parent B is not on current A's chain */
  UT_ASSERT_EQ(nested_callee_captures_reachable(&ut_dummy_state, &symC, &nfs[0]), 0);

  ut_dummy_state.nested_funcs = NULL;
  ut_dummy_state.nb_nested_funcs = 0;
  tcc_state = NULL;
  return 0;
}

UT_SUITE(tccgen)
{
  UT_RUN(test_type_size_scalar_armv8m_abi);
  UT_RUN(test_type_size_complex_types);
  UT_RUN(test_type_size_pointer_and_array);
  UT_RUN(test_type_size_struct_and_incomplete_enum);
  UT_RUN(test_type_size_ldouble_qlong_qfloat);
  UT_RUN(test_type_size_complete_enum_and_func);
  UT_RUN(test_type_size_array_of_incomplete_enum);
  UT_RUN(test_exact_log2p1_alignment_encoding);
  UT_RUN(test_exact_log2p1_non_powers_and_large);
  UT_RUN(test_exact_log2p1_zero_and_max_int);
  UT_RUN(test_is_float_recognizes_fp_btypes);
  UT_RUN(test_get_value_type_returns_null);
  UT_RUN(test_ieee_finite);
  UT_RUN(test_ieee_finite_subnormal);
  UT_RUN(test_vpushi_vpop);
  UT_RUN(test_vset);
  UT_RUN(test_vpushv);
  UT_RUN(test_vswap);
  UT_RUN(test_vrotb);
  UT_RUN(test_vrott);
  UT_RUN(test_vrotb_vrott_noop_for_small_n);
  UT_RUN(test_vrev);
  UT_RUN(test_vset_VT_CMP);
  UT_RUN(test_vpop_with_cmp_backpatch);
  UT_RUN(test_vsetc_triggers_vcheck_cmp_when_code_on);
  UT_RUN(test_check_vstack_empty);
  UT_RUN(test_test_lvalue_ok);
  UT_RUN(test_vpushsym);
  UT_RUN(test_sym_push2_and_find2);
  UT_RUN(test_sym_push_and_sym_find);
  UT_RUN(test_struct_find);
  UT_RUN(test_label_push_pop_and_find);
  UT_RUN(test_mk_pointer_creates_ref_sym);
  UT_RUN(test_vpush_typed_helper_func);
  UT_RUN(test_elfsym_invalid);
  UT_RUN(test_update_storage_with_esym);
  UT_RUN(test_put_extern_sym2_skips_invalid_v);
  UT_RUN(test_put_extern_sym2_creates_object_symbol);
  UT_RUN(test_put_extern_sym2_func_and_asm_types);
  UT_RUN(test_put_extern_sym_respects_nocode_wanted);
  UT_RUN(test_get_sym_ref);
  UT_RUN(test_external_global_sym_new);
  UT_RUN(test_external_global_sym_reuses_existing);
  UT_RUN(test_external_helper_sym);
  UT_RUN(test_vpush_helper_func);
  UT_RUN(test_global_identifier_push);
  UT_RUN(test_greloca_and_greloc);
  UT_RUN(test_greloca_skips_under_nocode_wanted);
  UT_RUN(test_put_extern_sym2_updates_existing_esym);
  UT_RUN(test_put_extern_sym2_name_variants);
  UT_RUN(test_put_extern_sym2_debug_mode);
  UT_RUN(test_external_global_sym_asm_existing);
  UT_RUN(test_greloca_null_symbol);
  UT_RUN(test_put_extern_sym_with_cur_text_section_nocode);
  UT_RUN(test_gen_negf);
  UT_RUN(test_sym_free);
  UT_RUN(test_is_float_quad_types);

  /* static-helper coverage (reachable via #include of tccgen.c) */
  UT_RUN(test_is_integer_btype);
  UT_RUN(test_btype_size);
  UT_RUN(test_get_int_type_bits);
  UT_RUN(test_gcc_classify_type);
  UT_RUN(test_pointed_type_and_size);
  UT_RUN(test_is_null_pointer);
  UT_RUN(test_compare_types_scalars);
  UT_RUN(test_compare_types_pointer_and_struct);
  UT_RUN(test_is_compatible_func);
  UT_RUN(test_is_transparent_union_type);
  UT_RUN(test_type_contains_pointer);
  UT_RUN(test_struct_has_pointer_member);
  UT_RUN(test_struct_has_bitfield_member);
  UT_RUN(test_struct_has_vla_member);
  UT_RUN(test_struct_is_single_scalar_member);
  UT_RUN(test_auto_inline_type_ok);
  UT_RUN(test_is_vector_type_and_make_vector);
  UT_RUN(test_compute_aapcs_natural_alignment);
  UT_RUN(test_try_get_constant);
  UT_RUN(test_fold_builtin_str_and_mem);
  UT_RUN(test_get_builtin_abs_info);
  UT_RUN(test_mark_value_bytes_scalar_and_vla);
  UT_RUN(test_vpush_variants);
  UT_RUN(test_vdup);
  UT_RUN(test_vpush_ref);
  UT_RUN(test_is_cond_bool);
  UT_RUN(test_sym_scope);
  UT_RUN(test_new_prev_scope_s);
  UT_RUN(test_case_cmp);
  UT_RUN(test_switch_can_use_jump_table);
  UT_RUN(test_case_sort_merges_adjacent);
  UT_RUN(test_get_const_double_and_float);
  UT_RUN(test_value64);
  UT_RUN(test_gen_opic_sdiv_and_lt);
  UT_RUN(test_is_zero_length_builtin_compare);
  UT_RUN(test_bitfield_unit_width);
  UT_RUN(test_sym_copy);
  UT_RUN(test_reg_return_helpers);
  UT_RUN(test_merge_symattr);
  UT_RUN(test_merge_funcattr);
  UT_RUN(test_merge_attr_and_sym_to_attr);
  UT_RUN(test_init_prec);
  UT_RUN(test_precedence_fn);
  UT_RUN(test_convert_parameter_type);
  UT_RUN(test_parse_btype_qualify);
  UT_RUN(test_is_const_for_folding);
  UT_RUN(test_check_nonvoid_value_ok);
  UT_RUN(test_inline_body_scanners);
  UT_RUN(test_find_sv_const_init);
  UT_RUN(test_funcall_scratch);
  UT_RUN(test_find_nested_func_by_sym);
  UT_RUN(test_gind);
  UT_RUN(test_gjmp_acs);
  UT_RUN(test_vset_VT_JMP);
  UT_RUN(test_type_to_str);
  UT_RUN(test_type_to_str_function);
  UT_RUN(test_type_incompatibility_warning);
  UT_RUN(test_compare_types_structural);
  UT_RUN(test_auto_inline_sig_ok);
  UT_RUN(test_builtin_abs_decl_matches);
  UT_RUN(test_try_get_constant_string_guards);
  UT_RUN(test_gen_op_const_fold_int);
  UT_RUN(test_gen_op_compare_int);
  UT_RUN(test_gen_op_const_fold_unsigned);
  UT_RUN(test_gen_op_const_fold_llong);
  UT_RUN(test_gen_op_const_fold_double);
  UT_RUN(test_gen_cast_const);
  UT_RUN(test_gen_cast_more);
  UT_RUN(test_gen_op_llong_more);
  UT_RUN(test_gen_op_fp_compare);
  UT_RUN(test_eval_vec_const_op);
  UT_RUN(test_try_fold_math_call);
  UT_RUN(test_read_write_vec_const_elem);
  UT_RUN(test_condition_3way);
  UT_RUN(test_auto_inline_param_count);
  UT_RUN(test_struct_is_small_bitfield_word);
  UT_RUN(test_promote_bitfield_expr_type);
  UT_RUN(test_inline_arg_is_constant_like);
  UT_RUN(test_force_charshort_cast);
  UT_RUN(test_objsize_vreg_facts);
  UT_RUN(test_find_local_scalar_sym);
  UT_RUN(test_svalue_get_conservative_max_u64);
  UT_RUN(test_svalue_get_conservative_string_bytes_u64);
  UT_RUN(test_adjust_bf);
  UT_RUN(test_struct_member_copy_safe);
  UT_RUN(test_find_assignable_transparent_union_member);
  UT_RUN(test_token_stream_references_local_object);
  UT_RUN(test_inline_body_shadowed_ident);
  UT_RUN(test_sym_copy_ref);
  UT_RUN(test_patch_storage);
  UT_RUN(test_gen_test_zero);
  UT_RUN(test_check_fields);
  UT_RUN(test_find_global_alias_target_sym);
  UT_RUN(test_alias_resolution);
  UT_RUN(test_try_inline_builtin_call);
  UT_RUN(test_scope_new_and_leave);
  UT_RUN(test_vla_restore_and_leave);
  UT_RUN(test_get_temp_local_var);
  UT_RUN(test_nested_callee_has_genuine_capture);
  UT_RUN(test_nested_callee_captures_reachable);
}
