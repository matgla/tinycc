/*
 *  test_tccgen.c - suite for tccgen.c
 *
 *  Covers exported frontend type helpers against the real tccgen.c object.
 */

#include "tcc.h"
#include "ut.h"

/* Not declared in tcc.h, but exported from tccgen.c. */
const char *get_value_type(int r);

/* vstack storage lives in tccgen.c; the macro `vstack` is local there. */
extern SValue _vstack[];

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
}
