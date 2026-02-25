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

#define USING_GLOBALS
#include "tcc.h"

#include "ir/codegen.h"
#include "ir/core.h"
#include "ir/licm.h"
#include "ir/opt.h"
#include "tccir.h"

// #define DEBUG_IR_GEN

/* Debug output for TCCGEN FUNCPARAMVAL processing - disabled by default
 * Enable with: -DTCCGEN_DEBUG_ENABLED or #define TCCGEN_DEBUG_ENABLED */
#ifdef TCCGEN_DEBUG_ENABLED
#define TCCGEN_DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define TCCGEN_DEBUG(...) ((void)0)
#endif

/********************************************************/
/* global variables */

/* loc : local variable index
   ind : output code index
   rsym: return symbol
   anon_sym: anonymous symbol index
*/
ST_DATA int rsym, anon_sym, ind, loc;

ST_DATA Sym *global_stack;
ST_DATA Sym *local_stack;
ST_DATA Sym *define_stack;
ST_DATA Sym *global_label_stack;
ST_DATA Sym *local_label_stack;

static Sym *sym_free_first;
static void **sym_pools;
static int nb_sym_pools;

static Sym *all_cleanups, *pending_gotos;
static int local_scope;
ST_DATA char debug_modes;

ST_DATA SValue *vtop;
ST_DATA SValue _vstack[1 + VSTACK_SIZE];
#define vstack (_vstack + 1)

ST_DATA int nocode_wanted;                /* no code generation wanted */
#define NODATA_WANTED (nocode_wanted > 0) /* no static data output wanted either */
#define DATA_ONLY_WANTED 0x80000000       /* ON outside of functions and for static initializers */

/* no code output after unconditional jumps such as with if (0) ... */
#define CODE_OFF_BIT 0x20000000
#define CODE_OFF()                                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!nocode_wanted)                                                                                                \
    {                                                                                                                  \
      nocode_wanted |= CODE_OFF_BIT;                                                                                   \
    }                                                                                                                  \
  } while (0)
#define CODE_ON() (nocode_wanted &= ~CODE_OFF_BIT)

/* no code output when parsing sizeof()/typeof() etc. (using nocode_wanted++/--)
 */
#define NOEVAL_MASK 0x0000FFFF
#define NOEVAL_WANTED (nocode_wanted & NOEVAL_MASK)

/* no code output when parsing constant expressions */
#define CONST_WANTED_BIT 0x00010000
#define CONST_WANTED_MASK 0x0FFF0000
#define CONST_WANTED (nocode_wanted & CONST_WANTED_MASK)

ST_DATA int global_expr; /* true if compound literals must be allocated globally
                            (used during initializers parsing */
ST_DATA CType func_vt;   /* current function return type (used by return instruction) */
ST_DATA int func_var;    /* true if current function is variadic (used by return
                            instruction) */
ST_DATA int func_vc;
ST_DATA int func_ind;
ST_DATA const char *funcname;
ST_DATA CType int_type, func_old_type, char_type, char_pointer_type;
static CString initstr;

#if PTR_SIZE == 4
#define VT_SIZE_T (VT_INT | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_INT
#elif LONG_SIZE == 4
#define VT_SIZE_T (VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_LLONG
#else
#define VT_SIZE_T (VT_LONG | VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T (VT_LONG | VT_LLONG)
#endif

const char *get_value_type(int r)
{
  return NULL;
}

static struct switch_t
{
  struct case_t
  {
    int64_t v1, v2;
    int ind, line;
  } **p;
  int n;       /* list of case ranges */
  int def_sym; /* default symbol */
  int nocode_wanted;
  int *bsym;
  struct scope *scope;
  struct switch_t *prev;
  SValue sv;
} *cur_switch; /* current switch */

#define MAX_TEMP_LOCAL_VARIABLE_NUMBER 8
/*list of temporary local variables on the stack in current function. */
static struct temp_local_variable
{
  int location; // offset on stack. Svalue.c.i
  short size;
  short align;
} arr_temp_local_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
static int nb_temp_local_vars;

static struct scope
{
  struct scope *prev;
  struct
  {
    int loc, locorig, num;
  } vla;
  struct
  {
    Sym *s;
    int n;
  } cl;
  int *bsym, *csym;
  Sym *lstk, *llstk;
} *cur_scope, *loop_scope, *root_scope;

typedef struct
{
  Section *sec;
  int local_offset;
  Sym *flex_array_ref;
} init_params;

#if 1
#define precedence_parser
static void init_prec(void);
#endif

static void block(int flags);
#define STMT_EXPR 1
#define STMT_COMPOUND 2

static void gen_cast(CType *type);
static void gen_cast_s(int t);
static inline CType *pointed_type(CType *type);
static int is_compatible_types(CType *type1, CType *type2);
static int parse_btype(CType *type, AttributeDef *ad, int ignore_label);
static CType *type_decl(CType *type, AttributeDef *ad, int *v, int td);
static void parse_expr_type(CType *type);
static void init_putv(init_params *p, CType *type, unsigned long c, int vreg);
static void decl_initializer(init_params *p, CType *type, unsigned long c, int flags, int vreg);
static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v, int scope);
static int decl(int l);
static void expr_eq(void);
static void vpush_type_size(CType *type, int *a);
static int is_compatible_unqualified_types(CType *type1, CType *type2);
static inline int64_t expr_const64(void);
static void vpush64(int ty, unsigned long long v);
static void vpush(CType *type);
static void gen_inline_functions(TCCState *s);
static void free_inline_functions(TCCState *s);
static void skip_or_save_block(TokenString **str);
static void gv_dup(void);
static int get_temp_local_var(int size, int align, int *vr_out);
static void cast_error(CType *st, CType *dt);
static void end_switch(void);
static void do_Static_assert(void);
static void vset_VT_JMP(void);
/* ------------------------------------------------------------------------- */
/* Automagical code suppression */

/* Clear 'nocode_wanted' at forward label if it was used */
ST_FUNC void gsym(int t)
{
  if (t > 0) /* -1 = no chain, 0 = instruction 0 (but gsym is for machine code, not IR) */
  {
    gsym_addr(t, ind);
    CODE_ON();
  }
}

/* Forward declaration for nested function handling */
static NestedFunc *find_nested_func_by_sym(Sym *sym);
static void setup_nested_func_trampoline(Sym *s);

/* Clear 'nocode_wanted' if current pc is a label */
static int gind()
{
  int t = tcc_state->ir->next_instruction_index;
  CODE_ON();
  if (debug_modes)
    tcc_tcov_block_begin(tcc_state);
  return t;
}

/* Set 'nocode_wanted' after unconditional (forwards) jump */
static int gjmp_acs(int t)
{
  // t = gjmp(t);
  SValue dest;
  svalue_init(&dest);
  dest.vr = -1;
  dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
  dest.c.i = t;
  t = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);

  CODE_OFF();
  return t;
}

/* These are #undef'd at the end of this file */
#define gjmp_addr gjmp_addr_acs
#define gjmp gjmp_acs
/* ------------------------------------------------------------------------- */

ST_INLN int is_float(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_LDOUBLE || bt == VT_DOUBLE || bt == VT_FLOAT || bt == VT_QFLOAT;
}

static inline int is_integer_btype(int bt)
{
  return bt == VT_BYTE || bt == VT_BOOL || bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG;
}

static int btype_size(int bt)
{
  return bt == VT_BYTE || bt == VT_BOOL ? 1
         : bt == VT_SHORT               ? 2
         : bt == VT_INT                 ? 4
         : bt == VT_LLONG               ? 8
         : bt == VT_PTR                 ? PTR_SIZE
                                        : 0;
}

/* returns function return register from type */
static int R_RET(int t)
{
  if (!is_float(t))
    return REG_IRET;
#ifdef TCC_TARGET_X86_64
  if ((t & VT_BTYPE) == VT_LDOUBLE)
    return TREG_ST0;
#elif defined TCC_TARGET_RISCV64
  if ((t & VT_BTYPE) == VT_LDOUBLE)
    return REG_IRET;
#endif
  return REG_FRET;
}

/* put function return registers to stack value */
static void PUT_R_RET(SValue *sv, int t)
{
  sv->r = R_RET(t);
}

/* returns function return register class for type t */
static int RC_RET(int t)
{
  return reg_classes[R_RET(t)] & ~(RC_FLOAT | RC_INT);
}

/* returns generic register class for type t */
static int RC_TYPE(int t)
{
  if (!is_float(t))
    return RC_INT;
  return RC_FLOAT;
}

// /* returns 2nd register class corresponding to t and rc */
// static int RC2_TYPE(int t, int rc)
// {
//   if (!USING_TWO_WORDS(t))
//     return 0;
// #ifdef RC_IRE2
//   if (rc == RC_IRET)
//     return RC_IRE2;
// #endif
// #ifdef RC_FRE2
//   if (rc == RC_FRET)
//     return RC_FRE2;
// #endif
//   if (rc & RC_FLOAT)
//     return RC_FLOAT;
//   return RC_INT;
// }

/* we use our own 'finite' function to avoid potential problems with
   non standard math libs */
/* XXX: endianness dependent */
ST_FUNC int ieee_finite(double d)
{
  int p[4];
  memcpy(p, &d, sizeof(double));
  return ((unsigned)((p[1] | 0x800fffff) + 1)) >> 31;
}

/* compiling intel long double natively */
#if (defined __i386__ || defined __x86_64__) && (defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64)
#define TCC_IS_NATIVE_387
#endif

ST_FUNC void test_lvalue(void)
{
  if (!(vtop->r & VT_LVAL))
    expect("lvalue");
}

ST_FUNC void check_vstack(void)
{
  if (vtop != vstack - 1)
    tcc_error("internal compiler error: vstack leak (%d)", (int)(vtop - vstack + 1));
}

/* vstack debugging aid */
#if 0
void pv(const char *lbl, int a, int b) {
  int i;
  for (i = a; i < a + b; ++i) {
    SValue *p = &vtop[-i];
    printf("%s vtop[-%d] : type.t:%04x  r:%04x  r2:%04x  c.i:%d\n", lbl, i,
           p->type.t, p->r, p->r2, (int)p->c.i);
  }
}
#endif

// debugging aid when stack is corrupted
#if 0
void dbg_print_vstack(const char *msg, const char *file, int line) {
  printf("print_vstack '%s' vtop: %p, elements: %d, at: %s:%d\n", msg, vtop,
         (vtop - vstack) + 1, file, line);
}
#endif

/* ------------------------------------------------------------------------- */
/* initialize vstack and types.  This must be done also for tcc -E */
ST_FUNC void tccgen_init(TCCState *s1)
{
  vtop = vstack - 1;
  memset(vtop, 0, sizeof *vtop);

  /* define some often used types */
  int_type.t = VT_INT;

  char_type.t = VT_BYTE;
  if (s1->char_is_unsigned)
    char_type.t |= VT_UNSIGNED;
  char_pointer_type = char_type;
  mk_pointer(&char_pointer_type);

  func_old_type.t = VT_FUNC;
  func_old_type.ref = sym_push(SYM_FIELD, &int_type, 0, 0);
  func_old_type.ref->f.func_call = FUNC_CDECL;
  func_old_type.ref->f.func_type = FUNC_OLD;
#ifdef precedence_parser
  init_prec();
#endif
  cstr_new(&initstr);
}

ST_FUNC int tccgen_compile(TCCState *s1)
{
  funcname = "";
  func_ind = -1;
  anon_sym = SYM_FIRST_ANOM;
  nocode_wanted = DATA_ONLY_WANTED; /* no code outside of functions */
  debug_modes = (s1->do_debug ? 1 : 0) | s1->test_coverage << 1;

  tcc_debug_start(s1);
  tcc_tcov_start(s1);
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  arm_init(s1);
#endif
#ifdef INC_DEBUG
  printf("%s: **** new file\n", file->filename);
#endif
  parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR;
  next();
  decl(VT_CONST);
  gen_inline_functions(s1);
  check_vstack();
  /* end of translation unit info */
#if TCC_EH_FRAME
  tcc_eh_frame_end(s1);
#endif
  tcc_debug_end(s1);
  tcc_tcov_end(s1);
  return 0;
}

ST_FUNC void tccgen_finish(TCCState *s1)
{
  tcc_debug_end(s1); /* just in case of errors: free memory */

  /* If compilation aborted while generating a function, the per-function IR
     block allocated in gen_function() may not have been released (because we
     unwind via longjmp). Free it here to avoid leaks on compile errors. */
  if (s1->ir)
  {
    tcc_ir_free(s1->ir);
    s1->ir = NULL;
  }

  free_inline_functions(s1);
  sym_pop(&global_stack, NULL, 0);
  sym_pop(&local_stack, NULL, 0);
  /* free preprocessor macros */
  free_defines(NULL);
  /* free sym_pools */
  dynarray_reset(&sym_pools, &nb_sym_pools);
  cstr_free(&initstr);
  dynarray_reset(&stk_data, &nb_stk_data);
  while (cur_switch)
    end_switch();
  local_scope = 0;
  loop_scope = NULL;
  all_cleanups = NULL;
  pending_gotos = NULL;
  nb_temp_local_vars = 0;
  global_label_stack = NULL;
  local_label_stack = NULL;
  cur_text_section = NULL;
  sym_free_first = NULL;
}

/* ------------------------------------------------------------------------- */
ST_FUNC ElfSym *elfsym(Sym *s)
{
  if (!s || s->c <= 0) /* s->c < 0 used for special values like -2 for "being defined" */
    return NULL;
  return &((ElfSym *)symtab_section->data)[s->c];
}

/* apply storage attributes to Elf symbol */
ST_FUNC void update_storage(Sym *sym)
{
  ElfSym *esym;
  int sym_bind, old_sym_bind;

  esym = elfsym(sym);
  if (!esym)
    return;

  if (sym->a.visibility)
    esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | sym->a.visibility;

  if (sym->type.t & (VT_STATIC | VT_INLINE))
    sym_bind = STB_LOCAL;
  else if (sym->a.weak)
    sym_bind = STB_WEAK;
  else
    sym_bind = STB_GLOBAL;
  old_sym_bind = ELFW(ST_BIND)(esym->st_info);
  if (sym_bind != old_sym_bind)
  {
    esym->st_info = ELFW(ST_INFO)(sym_bind, ELFW(ST_TYPE)(esym->st_info));
  }

#ifdef TCC_TARGET_PE
  if (sym->a.dllimport)
    esym->st_other |= ST_PE_IMPORT;
  if (sym->a.dllexport)
    esym->st_other |= ST_PE_EXPORT;
#endif

#if 0
    printf("storage %s: bind=%c vis=%d exp=%d imp=%d\n",
        get_tok_str(sym->v, NULL),
        sym_bind == STB_WEAK ? 'w' : sym_bind == STB_LOCAL ? 'l' : 'g',
        sym->a.visibility,
        sym->a.dllexport,
        sym->a.dllimport
        );
#endif
}

/* ------------------------------------------------------------------------- */
/* update sym->c so that it points to an external symbol in section
   'section' with value 'value' */

ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size, int can_add_underscore)
{
  int sym_type, sym_bind, info, other, t;
  ElfSym *esym;
  const char *name;
  char buf1[256];

  if (sym->c <= 0)
  {
    /* DEBUG: Validate sym->v before calling get_tok_str */
    /* Valid v values are: TOK_* constants, identifiers (TOK_IDENT..tok_ident), or anonymous (SYM_FIRST_ANOM..) */
    if (sym->v == 0xDEADBEEF)
    {
      /* Use-after-free detected - sym was freed but still referenced */
      return;
    }
    if (sym->v == 0 || (sym->v > 0x20000000 && sym->v < SYM_FIRST_ANOM))
    {
      /* sym->v looks like a garbage pointer - skip */
      return;
    }
    name = get_tok_str(sym->v, NULL);
    /* Detect garbage symbol names early */
    if (name && (name[0] == 'L' && name[1] == '.'))
    {
      /* This is likely a garbage anonymous symbol - L.XXXXX format */
      /* Check if the v value looks suspicious */
      if (sym->v >= SYM_FIRST_ANOM && (sym->v - SYM_FIRST_ANOM) > 100000)
      {
        tcc_error("internal error: put_extern_sym2 called with garbage anonymous symbol (v=0x%x, name='%s')", sym->v,
                  name);
      }
    }
    t = sym->type.t;
    if ((t & VT_BTYPE) == VT_FUNC)
    {
      sym_type = STT_FUNC;
    }
    else if ((t & VT_BTYPE) == VT_VOID)
    {
      sym_type = STT_NOTYPE;
      if ((t & (VT_BTYPE | VT_ASM_FUNC)) == VT_ASM_FUNC)
        sym_type = STT_FUNC;
    }
    else
    {
      sym_type = STT_OBJECT;
    }
    if (t & (VT_STATIC | VT_INLINE))
      sym_bind = STB_LOCAL;
    else
      sym_bind = STB_GLOBAL;
    other = 0;

#ifdef TCC_TARGET_PE
    if (sym_type == STT_FUNC && sym->type.ref)
    {
      Sym *ref = sym->type.ref;
      if (ref->a.nodecorate)
      {
        can_add_underscore = 0;
      }
      if (ref->f.func_call == FUNC_STDCALL && can_add_underscore)
      {
        sprintf(buf1, "_%s@%d", name, ref->f.func_args * PTR_SIZE);
        name = buf1;
        other |= ST_PE_STDCALL;
        can_add_underscore = 0;
      }
    }
#endif

    if (sym->asm_label)
    {
      name = get_tok_str(sym->asm_label, NULL);
      can_add_underscore = 0;
    }

    if (tcc_state->leading_underscore && can_add_underscore)
    {
      buf1[0] = '_';
      pstrcpy(buf1 + 1, sizeof(buf1) - 1, name);
      name = buf1;
    }

    info = ELFW(ST_INFO)(sym_bind, sym_type);
    sym->c = put_elf_sym(symtab_section, value, size, info, other, sh_num, name);

    if (debug_modes)
      tcc_debug_extern_sym(tcc_state, sym, sh_num, sym_bind, sym_type);
  }
  else
  {
    esym = elfsym(sym);
    esym->st_value = value;
    esym->st_size = size;
    esym->st_shndx = sh_num;
  }
  update_storage(sym);
}

ST_FUNC void put_extern_sym(Sym *sym, Section *s, addr_t value, unsigned long size)
{
  if (nocode_wanted && (NODATA_WANTED || (s && s == cur_text_section)))
    return;
  put_extern_sym2(sym, s ? s->sh_num : SHN_UNDEF, value, size, 1);
}

/* add a new relocation entry to symbol 'sym' in section 's' */
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend)
{
  int c = 0;

  if (nocode_wanted && s == cur_text_section)
    return;

  if (sym)
  {
    /* Debug: detect garbage symbols early */
    if (sym->v >= SYM_FIRST_ANOM && (sym->v - SYM_FIRST_ANOM) > 100000)
    {
      tcc_error("internal error: greloca called with garbage symbol (v=0x%x, c=%d, likely invalid pointer)", sym->v,
                sym->c);
    }
    /* Create ELF symbol if not yet created.
     * sym->c == 0: no ELF symbol yet
     * sym->c == -3: LABEL_ADDR_TAKEN marker (&&label), need to create symbol
     * sym->c > 0: valid ELF symbol index */
    if (sym->c <= 0)
      put_extern_sym(sym, NULL, 0, 0);
    c = sym->c;
    if (c <= 0)
    {
      /* sym->c should be a valid positive ELF symbol index at this point.
       * c = 0: put_extern_sym failed or was skipped (NODATA_WANTED?)
       * c = -1: type descriptor symbol (from mk_pointer) - should not be here
       * c = -2: struct/union being defined - should not be here
       * c = -3: LABEL_ADDR_TAKEN but put_extern_sym didn't create symbol
       * This indicates a bug where we're trying to create a relocation for
       * a symbol that was never properly registered in ELF. */
      tcc_error("internal error: greloca called with invalid symbol (c=%d, v=0x%x, type.t=0x%x, r=0x%x)", c, sym->v,
                sym->type.t, sym->r);
    }
  }

  /* now we can add ELF relocation info */
  put_elf_reloca(symtab_section, s, offset, type, c, addend);
}

#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
  greloca(s, sym, offset, type, 0);
}
#endif

/* ------------------------------------------------------------------------- */
/* symbol allocator */
static Sym *__sym_malloc(void)
{
  Sym *sym_pool, *sym, *last_sym;
  int i;

  sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
  dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

  last_sym = sym_free_first;
  sym = sym_pool;
  for (i = 0; i < SYM_POOL_NB; i++)
  {
    sym->next = last_sym;
    last_sym = sym;
    sym++;
  }
  sym_free_first = last_sym;
  return last_sym;
}

static inline Sym *sym_malloc(void)
{
  Sym *sym;
#ifndef SYM_DEBUG
  sym = sym_free_first;
  if (!sym)
    sym = __sym_malloc();
  sym_free_first = sym->next;
  return sym;
#else
  sym = tcc_malloc(sizeof(Sym));
  return sym;
#endif
}

ST_INLN void sym_free(Sym *sym)
{
#ifndef SYM_DEBUG
  /* Poison freed symbols to detect use-after-free */
  sym->v = 0xDEADBEEF;
  sym->next = sym_free_first;
  sym_free_first = sym;
#else
  tcc_free(sym);
#endif
}

/* push, without hashing */
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c)
{
  Sym *s;

  s = sym_malloc();
  memset(s, 0, sizeof *s);
  s->v = v;
  s->type.t = t;
  s->c = c;
  /* add in stack */
  s->prev = *ps;
  *ps = s;
  return s;
}

/* find a symbol and return its associated structure. 's' is the top
   of the symbol stack */
ST_FUNC Sym *sym_find2(Sym *s, int v)
{
  while (s)
  {
    if (s->v == v)
      return s;
    s = s->prev;
  }
  return NULL;
}

/* structure lookup */
ST_INLN Sym *struct_find(int v)
{
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  return table_ident[v]->sym_struct;
}

/* find an identifier */
ST_INLN Sym *sym_find(int v)
{
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  return table_ident[v]->sym_identifier;
}

static int sym_scope(Sym *s)
{
  int scope;
  if (IS_ENUM_VAL(s->type.t))
    scope = s->type.ref->sym_scope;
  else
    scope = s->sym_scope;
  return scope;
}

/* push a given symbol on the symbol stack */
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c)
{
  Sym *s, **ps;
  TokenSym *ts;
  int vreg = -1;
  /* register local variable at IR code generator, get Vreg number */
  int valmask = r & VT_VALMASK;

  if (r & VT_PARAM)
  {
    /* Create PARAM vreg for ALL parameters, including stack-passed ones */
    vreg = tcc_ir_get_vreg_param(tcc_state->ir);
    /* For stack-passed params (VT_LOCAL), c is the stack offset;
     * for register params, c is the parameter index */
    tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
    /* Store original parameter offset for prolog code generation */
    tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
    /* Mark float/double parameters */
    if (is_float(type->t))
    {
      int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
    }
    /* Mark complex parameters - needs register pairs */
    if (type->t & VT_COMPLEX)
      tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
    /* Mark long long parameters */
    if ((type->t & VT_BTYPE) == VT_LLONG)
    {
      tcc_ir_set_llong_type(tcc_state->ir, vreg);
    }
  }
  else
  {
    if (((valmask == VT_LOCAL) || (valmask == VT_LLOCAL)) && (r & VT_LVAL) && ((type->t & VT_BTYPE) != VT_STRUCT) &&
        !(type->t & (VT_ARRAY | VT_VLA)))
    {
      vreg = tcc_ir_get_vreg_var(tcc_state->ir);
      /* Set the variable's stack offset so LEA operations can find it */
      if (vreg >= 0)
      {
        tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
        tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
      }
      /* Mark float/double variables */
      if (is_float(type->t))
      {
        int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
        tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
      }
      /* Mark complex variables - needs register pairs */
      if (type->t & VT_COMPLEX)
      {
        tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
      }
      /* Mark long long variables */
      if ((type->t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vreg);
      }
    }
  }
  // }
  // r &= ~VT_PARAM;

  if (local_stack)
    ps = &local_stack;
  else
    ps = &global_stack;
  s = sym_push2(ps, v, type->t, c);
  s->type.ref = type->ref;
  s->r = r;
  s->vreg = vreg;
  /* don't record fields or anonymous symbols */
  /* XXX: simplify */
  if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
  {
    /* record symbol in token array */
    ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
    if (v & SYM_STRUCT)
      ps = &ts->sym_struct;
    else
      ps = &ts->sym_identifier;
    s->prev_tok = *ps;
    *ps = s;
    s->sym_scope = local_scope;
    if (s->prev_tok && sym_scope(s->prev_tok) == s->sym_scope)
      tcc_error("redeclaration of '%s'", get_tok_str(v & ~SYM_STRUCT, NULL));
  }
  return s;
}

/* push a global identifier */
ST_FUNC Sym *global_identifier_push(int v, int t, int c)
{
  Sym *s, **ps;
  s = sym_push2(&global_stack, v, t, c);
  s->r = VT_CONST | VT_SYM;
  /* don't record anonymous symbol */
  if (v < SYM_FIRST_ANOM)
  {
    ps = &table_ident[v - TOK_IDENT]->sym_identifier;
    /* modify the top most local identifier, so that sym_identifier will
       point to 's' when popped; happens when called from inline asm */
    while (*ps != NULL && (*ps)->sym_scope)
      ps = &(*ps)->prev_tok;
    s->prev_tok = *ps;
    *ps = s;
  }
  return s;
}

/* pop symbols until top reaches 'b'.  If KEEP is non-zero don't really
   pop them yet from the list, but do remove them from the token array.  */
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep)
{
  Sym *s, *ss, **ps;
  TokenSym *ts;
  int v;

  s = *ptop;
  while (s != b)
  {
    ss = s->prev;
    v = s->v;
    /* remove symbol in token array */
    /* XXX: simplify */
    if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
    {
      ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
      if (v & SYM_STRUCT)
        ps = &ts->sym_struct;
      else
        ps = &ts->sym_identifier;
      *ps = s->prev_tok;
    }
    /* Don't free symbols that have been exported to ELF (sym->c != 0)
       as they may still be referenced by IR instructions */
    if (!keep && s->c == 0)
    {
      /* In IR mode the backend may still need Sym pointers (notably for
       * VT_SYM address materialization and relocations). Block-scope extern
       * declarations create temporary Sym copies that can be referenced by IR
       * after the scope ends; freeing them here can lead to missing relocations
       * and loads/stores from address 0 at runtime.
       */
      if (!(tcc_state->ir && (s->r & VT_SYM)))
        sym_free(s);
    }
    s = ss;
  }
  if (!keep)
    *ptop = b;
}

/* label lookup */
ST_FUNC Sym *label_find(int v)
{
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  return table_ident[v]->sym_label;
}

ST_FUNC Sym *label_push(Sym **ptop, int v, int flags)
{
  Sym *s, **ps;
  s = sym_push2(ptop, v, VT_STATIC, 0);
  s->r = flags;
  s->jnext = -1; /* Initialize to -1 so we know if there's an actual forward goto */
  ps = &table_ident[v - TOK_IDENT]->sym_label;
  if (ptop == &global_label_stack)
  {
    /* modify the top most local identifier, so that
       sym_identifier will point to 's' when popped */
    while (*ps != NULL)
      ps = &(*ps)->prev_tok;
  }
  s->prev_tok = *ps;
  *ps = s;
  return s;
}

/* pop labels until element last is reached. Look if any labels are
   undefined. Define symbols if '&&label' was used. */
ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep)
{
  Sym *s, *s1;
  for (s = *ptop; s != slast; s = s1)
  {
    s1 = s->prev;
    int addr_taken = (s->c == -3 || s->c > 0); /* Remember if address was taken before modifying s->c */
    if (s->r == LABEL_DECLARED)
    {
      tcc_warning_c(warn_all)("label '%s' declared but not used", get_tok_str(s->v, NULL));
    }
    else if (s->r == LABEL_FORWARD)
    {
      tcc_error("label '%s' used but not defined", get_tok_str(s->v, NULL));
    }
    else
    {
      if (s->c)
      {
        /* Define corresponding symbol for &&label.
           In IR mode, the label position is recorded as an IR instruction index
           (s->jind) BEFORE DCE/IR compaction, so we must translate it using the
           original-index mapping.
           Also set Thumb bit (+1) so computed goto uses correct state.

           Note: s->c can be:
           - -3: LABEL_ADDR_TAKEN marker, need to reset to 0 for put_extern_sym to create symbol
           - > 0: valid ELF symbol index, put_extern_sym will UPDATE the existing symbol */
        if (s->c == -3)
          s->c = 0; /* Reset marker so put_extern_sym creates new symbol */

        if (tcc_state->ir && tcc_state->ir->orig_ir_to_code_mapping && s->jind >= 0 &&
            s->jind < tcc_state->ir->orig_ir_to_code_mapping_size)
        {
          uint32_t off = tcc_state->ir->orig_ir_to_code_mapping[s->jind];
          /* If the instruction at jind was deleted by DSE/optimization, find the next
             valid mapping. The sentinel value 0xFFFFFFFF indicates no instruction. */
          if (off == 0xFFFFFFFF)
          {
            for (int idx = s->jind + 1; idx < tcc_state->ir->orig_ir_to_code_mapping_size; idx++)
            {
              if (tcc_state->ir->orig_ir_to_code_mapping[idx] != 0xFFFFFFFF)
              {
                off = tcc_state->ir->orig_ir_to_code_mapping[idx];
                break;
              }
            }
          }
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else if (tcc_state->ir && tcc_state->ir->ir_to_code_mapping && s->jind >= 0 &&
                 s->jind < tcc_state->ir->ir_to_code_mapping_size)
        {
          /* Backward-compatible fallback for older IR mapping */
          uint32_t off = tcc_state->ir->ir_to_code_mapping[s->jind];
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else
        {
          /* Fallback for non-IR codegen */
          put_extern_sym(s, cur_text_section, s->jnext, 1);
        }
      }
    }
    /* remove label */
    if (s->r != LABEL_GONE)
      table_ident[s->v - TOK_IDENT]->sym_label = s->prev_tok;
    /* Don't free local label symbols whose address was taken (&&label) until
       after IR codegen, as the IR instructions still reference them. The symbol
       will be freed later with global labels after code generation. */
    if (!keep && !addr_taken)
      sym_free(s);
    else
      s->r = LABEL_GONE;
  }
  if (!keep)
    *ptop = slast;
}

/* ------------------------------------------------------------------------- */
static void vcheck_cmp(void)
{
  /* cannot let cpu flags if other instruction are generated. Also
     avoid leaving VT_JMP anywhere except on the top of the stack
     because it would complicate the code generator.

     Don't do this when nocode_wanted.  vtop might come from
     !nocode_wanted regions (see 88_codeopt.c) and transforming
     it to a register without actually generating code is wrong
     as their value might still be used for real.  All values
     we push under nocode_wanted will eventually be popped
     again, so that the VT_CMP/VT_JMP value will be in vtop
     when code is unsuppressed again. */

  /* However if it's just automatic suppression via CODE_OFF/ON()
     then it seems that we better let things work undisturbed.
     How can it work at all under nocode_wanted?  Well, gv() will
     actually clear it at the gsym() in load()/VT_JMP in the
     generator backends */

  // if (vtop->r == VT_CMP && 0 == (nocode_wanted & ~CODE_OFF_BIT))
  // gv(RC_INT);
  if (vtop >= vstack && (0 == (nocode_wanted & ~CODE_OFF_BIT)))
  {
    // if (vtop->r == VT_CMP) {
    // vset_VT_JMP();
    // }
    tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
  }
}

static void vsetc(CType *type, int r, CValue *vc)
{
  if (vtop >= vstack + (VSTACK_SIZE - 1))
    tcc_error("memory full (vstack)");

  vcheck_cmp();
  vtop++;
  print_vstack("vsetc");
  vtop->type = *type;
  vtop->r = r;
  vtop->c = *vc;
  vtop->vr = -1;
  vtop->pr0_reg = PREG_REG_NONE;
  vtop->pr0_spilled = 0;
  vtop->pr1_reg = PREG_REG_NONE;
  vtop->pr1_spilled = 0;
  vtop->sym = NULL;
  /* Note: jtrue/jfalse are in a union with c, so we DON'T initialize them here.
     They should only be used when r == VT_CMP, and c is used otherwise. */
}

ST_FUNC void vswap(void)
{
  SValue tmp;

  vcheck_cmp();
  tmp = vtop[0];
  vtop[0] = vtop[-1];
  vtop[-1] = tmp;
}

/* pop stack value */
ST_FUNC void vpop(void)
{
  int v;
  v = vtop->r & VT_VALMASK;
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)
  /* for x86, we need to pop the FP stack */
  if (v == TREG_ST0)
  {
    o(0xd8dd); /* fstp %st(0) */
  }
  else
#endif
      if (v == VT_CMP)
  {
    /* need to put correct jump if && or || without test */
    /* Use IR backpatching - jtrue/jfalse use -1 as "no chain" sentinel */
    if (vtop->jtrue >= 0)
      tcc_ir_backpatch_to_here(tcc_state->ir, vtop->jtrue);
    if (vtop->jfalse >= 0)
      tcc_ir_backpatch_to_here(tcc_state->ir, vtop->jfalse);
  }
  vtop--;
  print_vstack("vpop");
}

/* push constant of type "type" with useless value */
static void vpush(CType *type)
{
  vset(type, VT_CONST, 0);
}

/* push arbitrary 64bit constant */
static void vpush64(int ty, unsigned long long v)
{
  CValue cval;
  CType ctype;
  ctype.t = ty;
  ctype.ref = NULL;
  cval.i = v;
  vsetc(&ctype, VT_CONST, &cval);
}

/* push integer constant */
ST_FUNC void vpushi(int v)
{
  vpush64(VT_INT, v);
}

/* push a pointer sized constant */
static void vpushs(addr_t v)
{
  vpush64(VT_SIZE_T, v);
}

/* push long long constant */
static inline void vpushll(long long v)
{
  vpush64(VT_LLONG, v);
}

ST_FUNC void vset(CType *type, int r, int v)
{
  CValue cval;
  cval.i = v;
  vsetc(type, r, &cval);
}

static void vseti(int r, int v)
{
  CType type;
  type.t = VT_INT;
  type.ref = NULL;
  vset(&type, r, v);
}

ST_FUNC void vpushv(SValue *v)
{
  if (vtop >= vstack + (VSTACK_SIZE - 1))
    tcc_error("memory full (vstack)");
  vtop++;
  print_vstack("vpushv");
  *vtop = *v;
}

static void vdup(void)
{
  vpushv(vtop);
}

/* rotate the stack element at position n-1 to the top */
ST_FUNC void vrotb(int n)
{
  SValue tmp;
  if (--n < 1)
    return;
  vcheck_cmp();
  tmp = vtop[-n];
  memmove(vtop - n, vtop - n + 1, sizeof *vtop * n);
  vtop[0] = tmp;
}

/* rotate the top stack element into position n-1 */
ST_FUNC void vrott(int n)
{
  SValue tmp;
  if (--n < 1)
    return;
  vcheck_cmp();
  tmp = vtop[0];
  memmove(vtop - n + 1, vtop - n, sizeof *vtop * n);
  vtop[-n] = tmp;
}

/* reverse order of the the first n stack elements */
ST_FUNC void vrev(int n)
{
  int i;
  SValue tmp;
  vcheck_cmp();
  for (i = 0, n = -n; i > ++n; --i)
    tmp = vtop[i], vtop[i] = vtop[n], vtop[n] = tmp;
}

/* ------------------------------------------------------------------------- */
/* vtop->r = VT_CMP means CPU-flags have been set from comparison or test. */

/* called from generators to set the result from relational ops  */
ST_FUNC void vset_VT_CMP(int op)
{
  vtop->r = VT_CMP;
  vtop->cmp_op = op;
  vtop->jfalse = -1; /* -1 = no chain */
  vtop->jtrue = -1;  /* -1 = no chain */
}

/* called once before asking generators to load VT_CMP to a register */
static void vset_VT_JMP(void)
{
  if (vtop->r != VT_CMP)
    return;

  int op = vtop->cmp_op;

  // if (vtop->jtrue || vtop->jfalse) {
  int origt = vtop->type.t;
  /* we need to jump to 'mov $0,%R' or 'mov $1,%R' */
  int inv = op & (op < 2); /* small optimization */
  int test = tcc_ir_codegen_test_gen(tcc_state->ir, inv, 0);
  vseti(VT_JMP + inv, test);
  vtop->type.t |= origt & (VT_UNSIGNED | VT_DEFSIGN);
  // } else {
  /* otherwise convert flags (rsp. 0/1) to register */
  // vtop->c.i = op;
  // if (op < 2) /* doesn't seem to happen */
  // vtop->r = VT_CONST;
  // }
}

/* Set CPU Flags, doesn't yet jump */
static void gvtst_set(int inv, int t)
{
  int *p;
  // SValue dest;

  if (vtop->r != VT_CMP)
  {
    vpushi(0);
    gen_op(TOK_NE);
    if (vtop->r != VT_CMP) /* must be VT_CONST then */
      vset_VT_CMP(vtop->c.i != 0 ? TOK_NE : TOK_EQ);
  }

  p = inv ? &vtop->jfalse : &vtop->jtrue;
  *p = tcc_ir_gjmp_append(tcc_state->ir, *p, t);
  // tcc_ir_codegen_test_gen(tcc_state->ir, inv, t);
  // if (vtop->)
  // *p = tcc_ir_gjmp_append(tcc_state->ir, *p, t);
  // tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
}

/* generate a zero or nozero test */
static void gen_test_zero(int op)
{
  if (vtop->r == VT_CMP)
  {
    int j;
    if (op == TOK_EQ)
    {
      j = vtop->jfalse;
      vtop->jfalse = vtop->jtrue;
      vtop->jtrue = j;
      vtop->cmp_op ^= 1;
    }
  }
  else
  {
    vpushi(0);
    gen_op(op);
  }
}

/* ------------------------------------------------------------------------- */
/* push a symbol value of TYPE */
ST_FUNC void vpushsym(CType *type, Sym *sym)
{
  CValue cval;
  cval.i = 0;
  vsetc(type, VT_CONST | VT_SYM, &cval);
  vtop->sym = sym;
}

/* Return a static symbol pointing to a section */
ST_FUNC Sym *get_sym_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
  int v;
  Sym *sym;

  v = anon_sym++;
  sym = sym_push(v, type, VT_CONST | VT_SYM, 0);
  sym->type.t |= VT_STATIC;
  put_extern_sym(sym, sec, offset, size);
  return sym;
}

/* push a reference to a section offset by adding a dummy symbol */
static void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
  vpushsym(type, get_sym_ref(type, sec, offset, size));
}

/* define a new external reference to a symbol 'v' of type 'u' */
ST_FUNC Sym *external_global_sym(int v, CType *type)
{
  Sym *s;

  s = sym_find(v);
  if (!s)
  {
    /* push forward reference */
    s = global_identifier_push(v, type->t | VT_EXTERN, 0);
    s->type.ref = type->ref;
  }
  else if (IS_ASM_SYM(s))
  {
    s->type.t = type->t | (s->type.t & VT_EXTERN);
    s->type.ref = type->ref;
    update_storage(s);
  }
  return s;
}

/* create an external reference with no specific type similar to asm labels.
   This avoids type conflicts if the symbol is used from C too */
ST_FUNC Sym *external_helper_sym(int v)
{
  CType ct = {VT_ASM_FUNC, NULL};
  return external_global_sym(v, &ct);
}

/* push a reference to an helper function (such as memmove) */
ST_FUNC void vpush_helper_func(int v)
{
  vpushsym(&func_old_type, external_helper_sym(v));
}

/* Merge symbol attributes.  */
static void merge_symattr(struct SymAttr *sa, struct SymAttr *sa1)
{
  if (sa1->aligned && !sa->aligned)
    sa->aligned = sa1->aligned;
  sa->packed |= sa1->packed;
  sa->weak |= sa1->weak;
  sa->nodebug |= sa1->nodebug;
  if (sa1->visibility != STV_DEFAULT)
  {
    int vis = sa->visibility;
    if (vis == STV_DEFAULT || vis > sa1->visibility)
      vis = sa1->visibility;
    sa->visibility = vis;
  }
  sa->dllexport |= sa1->dllexport;
  sa->nodecorate |= sa1->nodecorate;
  sa->dllimport |= sa1->dllimport;
  sa->naked |= sa1->naked;
}

/* Merge function attributes.  */
static void merge_funcattr(struct FuncAttr *fa, struct FuncAttr *fa1)
{
  if (fa1->func_call && !fa->func_call)
    fa->func_call = fa1->func_call;
  if (fa1->func_type && !fa->func_type)
    fa->func_type = fa1->func_type;
  if (fa1->func_args && !fa->func_args)
    fa->func_args = fa1->func_args;
  if (fa1->func_noreturn)
    fa->func_noreturn = 1;
  if (fa1->func_ctor)
    fa->func_ctor = 1;
  if (fa1->func_dtor)
    fa->func_dtor = 1;
  if (fa1->func_pure)
    fa->func_pure = 1;
  if (fa1->func_const)
    fa->func_const = 1;
}

/* Merge attributes.  */
static void merge_attr(AttributeDef *ad, AttributeDef *ad1)
{
  merge_symattr(&ad->a, &ad1->a);
  merge_funcattr(&ad->f, &ad1->f);

  if (ad1->section)
    ad->section = ad1->section;
  if (ad1->alias_target)
    ad->alias_target = ad1->alias_target;
  if (ad1->asm_label)
    ad->asm_label = ad1->asm_label;
  if (ad1->attr_mode)
    ad->attr_mode = ad1->attr_mode;
}

/* Merge some type attributes.  */
static void patch_type(Sym *sym, CType *type)
{
  if (!(type->t & VT_EXTERN) || IS_ENUM_VAL(sym->type.t))
  {
    if (!(sym->type.t & VT_EXTERN))
      tcc_error("redefinition of '%s'", get_tok_str(sym->v, NULL));
    sym->type.t &= ~VT_EXTERN;
  }

  if (IS_ASM_SYM(sym))
  {
    /* stay static if both are static */
    sym->type.t = type->t & (sym->type.t | ~VT_STATIC);
    sym->type.ref = type->ref;
    if ((type->t & VT_BTYPE) != VT_FUNC && !(type->t & VT_ARRAY))
      sym->r |= VT_LVAL;
  }

  if (!is_compatible_types(&sym->type, type))
  {
    tcc_error("incompatible types for redefinition of '%s'", get_tok_str(sym->v, NULL));
  }
  else if ((sym->type.t & VT_BTYPE) == VT_FUNC)
  {
    int static_proto = sym->type.t & VT_STATIC;
    /* warn if static follows non-static function declaration */
    if ((type->t & VT_STATIC) &&
        !static_proto
        /* XXX this test for inline shouldn't be here.  Until we
           implement gnu-inline mode again it silences a warning for
           mingw caused by our workarounds.  */
        && !((type->t | sym->type.t) & VT_INLINE))
      tcc_warning("static storage ignored for redefinition of '%s'", get_tok_str(sym->v, NULL));

    /* set 'inline' if both agree or if one has static */
    if ((type->t | sym->type.t) & VT_INLINE)
    {
      if (!((type->t ^ sym->type.t) & VT_INLINE) || ((type->t | sym->type.t) & VT_STATIC))
        static_proto |= VT_INLINE;
    }

    if (0 == (type->t & VT_EXTERN))
    {
      struct FuncAttr f = sym->type.ref->f;
      /* put complete type, use static from prototype */
      sym->type.t = (type->t & ~(VT_STATIC | VT_INLINE)) | static_proto;
      sym->type.ref = type->ref;
      merge_funcattr(&sym->type.ref->f, &f);
    }
    else
    {
      sym->type.t &= ~VT_INLINE | static_proto;
    }

    if (sym->type.ref->f.func_type == FUNC_OLD && type->ref->f.func_type != FUNC_OLD)
    {
      sym->type.ref = type->ref;
    }
  }
  else
  {
    if ((sym->type.t & VT_ARRAY) && type->ref->c >= 0)
    {
      /* set array size if it was omitted in extern declaration */
      sym->type.ref->c = type->ref->c;
    }
    if ((type->t ^ sym->type.t) & VT_STATIC)
      tcc_warning("storage mismatch for redefinition of '%s'", get_tok_str(sym->v, NULL));
  }
}

/* Merge some storage attributes.  */
static void patch_storage(Sym *sym, AttributeDef *ad, CType *type)
{
  if (type)
    patch_type(sym, type);

#ifdef TCC_TARGET_PE
  if (sym->a.dllimport != ad->a.dllimport)
    tcc_error("incompatible dll linkage for redefinition of '%s'", get_tok_str(sym->v, NULL));
#endif
  merge_symattr(&sym->a, &ad->a);
  /* Note: func_pure/func_const attributes are handled in external_sym
   * and in the function type symbol (type.ref->f), not in sym->f.
   * We don't merge ad->f into sym->f here to avoid corrupting function
   * type information (func_type, func_args). */
  if (ad->asm_label)
    sym->asm_label = ad->asm_label;
  update_storage(sym);
}

/* copy sym to other stack */
static Sym *sym_copy(Sym *s0, Sym **ps)
{
  Sym *s;
  s = sym_malloc(), *s = *s0;
  s->prev = *ps, *ps = s;
  if (s->v < SYM_FIRST_ANOM)
  {
    ps = &table_ident[s->v - TOK_IDENT]->sym_identifier;
    s->prev_tok = *ps, *ps = s;
  }
  return s;
}

/* copy s->type.ref to stack 'ps' for VT_FUNC and VT_PTR */
static void sym_copy_ref(Sym *s, Sym **ps)
{
  int bt = s->type.t & VT_BTYPE;
  if (bt == VT_FUNC || bt == VT_PTR || (bt == VT_STRUCT && s->sym_scope))
  {
    Sym **sp = &s->type.ref;
    for (s = *sp, *sp = NULL; s; s = s->next)
    {
      /* For struct types without local scope, don't copy - preserve type identity.
       * This fixes nested function struct return type mismatches where the struct
       * type would be copied, creating different ref pointers for the same type. */
      if ((s->type.t & VT_BTYPE) == VT_STRUCT && !s->sym_scope)
      {
        /* Keep the original global struct type, don't copy */
        *sp = s;
        sp = &s->next;
      }
      else
      {
        Sym *s2 = sym_copy(s, ps);
        sp = &(*sp = s2)->next;
        sym_copy_ref(s2, ps);
      }
    }
  }
}

/* define a new external reference to a symbol 'v' */
static Sym *external_sym(int v, CType *type, int r, AttributeDef *ad)
{
  Sym *s;

  /* look for global symbol */
  s = sym_find(v);
  while (s && s->sym_scope)
    s = s->prev_tok;

  if (!s)
  {
    /* push forward reference */
    s = global_identifier_push(v, type->t, 0);
    s->r |= r;
    s->a = ad->a;
    /* Merge function attributes (pure, const, etc.) without overwriting
     * func_type and func_args which are set from type.ref->f */
    if (ad->f.func_pure)
      s->f.func_pure = 1;
    if (ad->f.func_const)
      s->f.func_const = 1;
    s->asm_label = ad->asm_label;
    s->type.ref = type->ref;
    /* copy type to the global stack */
    if (local_stack)
      sym_copy_ref(s, &global_stack);
  }
  else
  {
    patch_storage(s, ad, type);
  }
  /* push variables on local_stack if any */
  if (local_stack && (s->type.t & VT_BTYPE) != VT_FUNC)
    s = sym_copy(s, &local_stack);
  return s;
}

/* Legacy register spilling helpers removed: IR owns spilling. */

/* IR-only: frontend never allocates physical registers. */

/* find a free temporary local variable (return the offset on stack) match
   size and align. If none, add new temporary stack variable.
   The temp local index is encoded in vr_out using VR_TEMP_LOCAL(). */
static int get_temp_local_var(int size, int align, int *vr_out)
{
  int i;
  struct temp_local_variable *temp_var;
  SValue *p;
  int r;
  unsigned used = 0;

  /* mark locations that are still in use */
  for (p = vstack; p <= vtop; p++)
  {
    r = p->r & VT_VALMASK;
    if (r == VT_LOCAL || r == VT_LLOCAL)
    {
      if (VR_IS_TEMP_LOCAL(p->vr))
        used |= 1 << VR_TEMP_LOCAL_IDX(p->vr);
    }
  }
  for (i = 0; i < nb_temp_local_vars; i++)
  {
    temp_var = &arr_temp_local_vars[i];
    if (!(used & 1 << i) && temp_var->size >= size && temp_var->align >= align)
    {
    ret_tmp:
      *vr_out = VR_TEMP_LOCAL(i);
      return temp_var->location;
    }
  }
  loc = (loc - size) & -align;
  if (nb_temp_local_vars < MAX_TEMP_LOCAL_VARIABLE_NUMBER)
  {
    temp_var = &arr_temp_local_vars[nb_temp_local_vars];
    temp_var->location = loc;
    temp_var->size = size;
    temp_var->align = align;
    nb_temp_local_vars++;
    goto ret_tmp;
  }
  *vr_out = -1; /* No temp local slot available */
  return loc;
}

/* move register 's' (of type 't') to 'r', and flush previous value of r to
   memory if needed */
static void move_reg(int r, int s, int t)
{
  (void)r;
  (void)s;
  (void)t;
  /* IR-only: physical register shuffling is handled after IR lowering. */
  return;
}

/* get address of vtop (vtop MUST BE an lvalue) */
ST_FUNC void gaddrof(void)
{
  vtop->r &= ~VT_LVAL;
  /* tricky: if saved lvalue, then we can go back to lvalue */
  if ((vtop->r & VT_VALMASK) == VT_LLOCAL)
  {
    /* VT_LLOCAL means the pointer is stored at the local/param location.
     * We need to load that pointer value into a temporary. */
    SValue ptr_location = *vtop; // Save the location where the pointer is stored

    // Convert VT_LLOCAL to VT_LOCAL so backend knows it's a stack/param location
    ptr_location.r = (ptr_location.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
    // ptr_location should have pointer type, not struct type
    // This tells the backend that loading from this location gives us a pointer value
    ptr_location.type = vtop->type; // Keep the pointer type from the original VT_LLOCAL parameter

    SValue loaded_ptr;
    memset(&loaded_ptr, 0, sizeof(loaded_ptr));
    loaded_ptr.type = *pointed_type(&vtop->type);                 // Type of what the pointer points to
    loaded_ptr.type.t = (loaded_ptr.type.t & ~VT_BTYPE) | VT_PTR; // Make it a pointer type
    loaded_ptr.type.t &= ~(VT_ARRAY | VT_VLA);
    loaded_ptr.type.ref = vtop->type.ref;
    loaded_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    // Generate LOAD operation: loaded_ptr <-- *ptr_location
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &ptr_location, NULL, &loaded_ptr);

    // Replace vtop with the loaded pointer
    *vtop = loaded_ptr;
    // The loaded pointer is the address value itself, NOT an lvalue.
    // We loaded the pointer from the stack slot; this pointer IS the base address.
    // Do NOT set VT_LVAL here - that would cause another dereference when we
    // want to do pointer arithmetic (e.g., adding field offset).
    vtop->r = 0;
  }
  else if ((vtop->r & VT_VALMASK) == VT_LOCAL && tcc_state->ir)
  {
    /* VT_LOCAL without VT_LVAL means "address of local variable".
     * In IR mode, emit explicit LEA to compute FP+offset into a vreg.
     * This avoids ambiguity where VT_LOCAL alone could be misinterpreted
     * as either "address value" or "spilled value to load".
     *
     * IMPORTANT: Do NOT set VT_LVAL here! LEA needs the raw VT_LOCAL
     * so that tcc_ir_materialize_addr() computes the stack address.
     * VT_LVAL would prevent address materialization.
     */
    SValue src = *vtop;
    /* Ensure VT_LOCAL is preserved and VT_LVAL is NOT set */
    src.r = (src.r & ~VT_LVAL) | VT_LOCAL;

    SValue dest;
    memset(&dest, 0, sizeof(dest));
    dest.type.t = VT_PTR;
    dest.type.ref = vtop->type.ref;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    tcc_ir_put(tcc_state->ir, TCCIR_OP_LEA, &src, NULL, &dest);

    vtop->vr = dest.vr;
    vtop->r = 0; /* Now it's a computed value in a vreg */
    vtop->c.i = 0;
  }
  else if ((vtop->r & VT_PARAM) && tcc_state->ir && (vtop->r & VT_VALMASK) < VT_CONST)
  {
    /* Register-passed parameter without VT_LOCAL: in IR mode, register
     * parameters are represented as VT_PARAM | VT_LVAL (val_kind = register
     * number) without VT_LOCAL. When address-of is applied, the parameter
     * must reside on the stack (addrtaken was already set on the vreg).
     * Emit a LEA referencing the vreg as VT_LOCAL so the register allocator
     * assigns a stack slot and the LEA computes its address.
     */
    SValue src = *vtop;
    /* Convert to VT_LOCAL (with VT_PARAM preserved) so svalue_to_iroperand
     * classifies it as IROP_TAG_STACKOFF with is_param=1. */
    src.r = VT_LOCAL | VT_PARAM;

    SValue dest;
    memset(&dest, 0, sizeof(dest));
    dest.type.t = VT_PTR;
    dest.type.ref = vtop->type.ref;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);

    tcc_ir_put(tcc_state->ir, TCCIR_OP_LEA, &src, NULL, &dest);

    vtop->vr = dest.vr;
    vtop->r = 0; /* Now it's a computed value in a vreg */
    vtop->c.i = 0;
  }
}

#ifdef CONFIG_TCC_BCHECK
/* generate a bounded pointer addition */
static void gen_bounded_ptr_add(void)
{
  int save = (vtop[-1].r & VT_VALMASK) == VT_LOCAL;
  if (save)
  {
    vpushv(&vtop[-1]);
    vrott(3);
  }
  vpush_helper_func(TOK___bound_ptr_add);
  vrott(3);
  // gfunc_call(2);
  tcc_error("1 implement me");
  vtop -= save;
  vpushi(0);
  /* returned pointer is in REG_IRET */
  vtop->r = REG_IRET | VT_BOUNDED;
  if (nocode_wanted)
    return;
  /* relocation offset of the bounding function call point */
  vtop->c.i = (cur_text_section->reloc->data_offset - sizeof(ElfW_Rel));
}

/* patch pointer addition in vtop so that pointer dereferencing is
   also tested */
static void gen_bounded_ptr_deref(void)
{
  addr_t func;
  int size, align;
  ElfW_Rel *rel;
  Sym *sym;

  if (nocode_wanted)
    return;

  size = type_size(&vtop->type, &align);
  switch (size)
  {
  case 1:
    func = TOK___bound_ptr_indir1;
    break;
  case 2:
    func = TOK___bound_ptr_indir2;
    break;
  case 4:
    func = TOK___bound_ptr_indir4;
    break;
  case 8:
    func = TOK___bound_ptr_indir8;
    break;
  case 12:
    func = TOK___bound_ptr_indir12;
    break;
  case 16:
    func = TOK___bound_ptr_indir16;
    break;
  default:
    /* may happen with struct member access */
    return;
  }
  sym = external_helper_sym(func);
  if (!sym->c)
    put_extern_sym(sym, NULL, 0, 0);
  /* patch relocation */
  /* XXX: find a better solution ? */
  rel = (ElfW_Rel *)(cur_text_section->reloc->data + vtop->c.i);
  rel->r_info = ELFW(R_INFO)(sym->c, ELFW(R_TYPE)(rel->r_info));
}

/* generate lvalue bound code */
static void gbound(void)
{
  CType type1;

  vtop->r &= ~VT_MUSTBOUND;
  /* if lvalue, then use checking code before dereferencing */
  if (vtop->r & VT_LVAL)
  {
    /* if not VT_BOUNDED value, then make one */
    if (!(vtop->r & VT_BOUNDED))
    {
      /* must save type because we must set it to int to get pointer */
      type1 = vtop->type;
      vtop->type.t = VT_PTR;
      gaddrof();
      vpushi(0);
      gen_bounded_ptr_add();
      vtop->r |= VT_LVAL;
      vtop->type = type1;
    }
    /* then check for dereferencing */
    gen_bounded_ptr_deref();
  }
}

/* Add bounds for local symbols from S to E (via ->prev) */
static void add_local_bounds(Sym *s, Sym *e)
{
  for (; s != e; s = s->prev)
  {
    if (!s->v || (s->r & VT_VALMASK) != VT_LOCAL)
      continue;
    /* Add arrays/structs/unions because we always take address */
    if ((s->type.t & VT_ARRAY) || (s->type.t & VT_BTYPE) == VT_STRUCT || s->a.addrtaken)
    {
      /* add local bound info */
      int align, size = type_size(&s->type, &align);
      addr_t *bounds_ptr = section_ptr_add(lbounds_section, 2 * sizeof(addr_t));
      bounds_ptr[0] = s->c;
      bounds_ptr[1] = size;
    }
  }
}
#endif

/* Wrapper around sym_pop, that potentially also registers local bounds.  */
static void pop_local_syms(Sym *b, int keep)
{
#ifdef CONFIG_TCC_BCHECK
  if (tcc_state->do_bounds_check && !keep && (local_scope || !func_var))
    add_local_bounds(local_stack, b);
#endif
  if (debug_modes)
    tcc_add_debug_info(tcc_state, !local_scope, local_stack, b);
  sym_pop(&local_stack, b, keep);
}

/* increment an lvalue pointer */
static void incr_offset(int offset)
{
  int t = vtop->type.t;
  gaddrof();                   /* remove VT_LVAL */
  vtop->type.t = VT_PTRDIFF_T; /* set scalar type */
  vpushs(offset);
  gen_op('+');
  vtop->r |= VT_LVAL;
  vtop->type.t = t;
}

static void incr_bf_adr(int o)
{
  vtop->type.t = VT_BYTE | VT_UNSIGNED;
  incr_offset(o);
}

/* single-byte load mode for packed or otherwise unaligned bitfields */
static void load_packed_bf(CType *type, int bit_pos, int bit_size)
{
  int n, o, bits;
  vpush64(type->t & VT_BTYPE, 0); // B X
  bits = 0, o = bit_pos >> 3, bit_pos &= 7;
  do
  {
    vswap(); // X B
    incr_bf_adr(o);
    vdup(); // X B B
    n = 8 - bit_pos;
    if (n > bit_size)
      n = bit_size;
    if (bit_pos)
      vpushi(bit_pos), gen_op(TOK_SHR), bit_pos = 0; // X B Y
    if (n < 8)
      vpushi((1 << n) - 1), gen_op('&');
    gen_cast(type);
    if (bits)
      vpushi(bits), gen_op(TOK_SHL);
    vrotb(3);    // B Y X
    gen_op('|'); // B X
    bits += n, bit_size -= n, o = 1;
  } while (bit_size);
  vswap(), vpop();
  if (!(type->t & VT_UNSIGNED))
  {
    n = ((type->t & VT_BTYPE) == VT_LLONG ? 64 : 32) - bits;
    vpushi(n), gen_op(TOK_SHL);
    vpushi(n), gen_op(TOK_SAR);
  }
}

/* single-byte store mode for packed or otherwise unaligned bitfields */
static void store_packed_bf(int bit_pos, int bit_size)
{
  int bits, n, o, m, c;
  c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  vswap(); // X B
  bits = 0, o = bit_pos >> 3, bit_pos &= 7;
  do
  {
    incr_bf_adr(o);        // X B
    vswap();               // B X
    c ? vdup() : gv_dup(); // B V X
    vrott(3);              // X B V
    if (bits)
      vpushi(bits), gen_op(TOK_SHR);
    if (bit_pos)
      vpushi(bit_pos), gen_op(TOK_SHL);
    n = 8 - bit_pos;
    if (n > bit_size)
      n = bit_size;
    if (n < 8)
    {
      m = ((1 << n) - 1) << bit_pos;
      vpushi(m), gen_op('&'); // X B V1
      vpushv(vtop - 1);       // X B V1 B
      vpushi(m & 0x80 ? ~m & 0x7f : ~m);
      gen_op('&'); // X B V1 B1
      gen_op('|'); // X B V2
    }
    vdup(), vtop[-1] = vtop[-2]; // X B B V2
    vstore(), vpop();            // X B
    bits += n, bit_size -= n, bit_pos = 0, o = 1;
  } while (bit_size);
  vpop(), vpop();
}

static int adjust_bf(SValue *sv, int bit_pos, int bit_size)
{
  int t;
  if (0 == sv->type.ref)
    return 0;
  t = sv->type.ref->auxtype;
  if (t != -1 && t != VT_STRUCT)
  {
    sv->type.t = (sv->type.t & ~(VT_BTYPE | VT_LONG)) | t;
    sv->r |= VT_LVAL;
  }
  return t;
}

/* store vtop a register belonging to class 'rc'. lvalues are
   converted to values. Cannot be used if cannot be converted to
   register value (such as structures). */
ST_FUNC int gv(int rc)
{
  int r, r_ok, r2_ok, rc2;
  int bit_pos, bit_size, size, align;
  int vreg = -1;

  /* For IR mode: if we already have a valid vreg computed, no need to do anything.
     Valid vregs have type 1, 2, or 3 in the upper 4 bits. Type 0 is invalid. */
  if (tcc_state->ir && TCCIR_DECODE_VREG_TYPE(vtop->vr) > 0 && !(vtop->r & VT_LVAL))
  {
    return vtop->r & VT_VALMASK;
  }

  /* NOTE: get_reg can modify vstack[] */
  if (vtop->type.t & VT_BITFIELD)
  {
    CType type;

    bit_pos = BIT_POS(vtop->type.t);
    bit_size = BIT_SIZE(vtop->type.t);
    /* remove bit field info to avoid loops */
    vtop->type.t &= ~VT_STRUCT_MASK;

    type.ref = NULL;
    type.t = vtop->type.t & VT_UNSIGNED;
    if ((vtop->type.t & VT_BTYPE) == VT_BOOL)
      type.t |= VT_UNSIGNED;

    r = adjust_bf(vtop, bit_pos, bit_size);

    if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
      type.t |= VT_LLONG;
    else
      type.t |= VT_INT;

    if (r == VT_STRUCT)
    {
      load_packed_bf(&type, bit_pos, bit_size);
    }
    else
    {
      int bits = (type.t & VT_BTYPE) == VT_LLONG ? 64 : 32;
      /* cast to int to propagate signedness in following ops */
      gen_cast(&type);
      /* generate shifts */
      vpushi(bits - (bit_pos + bit_size));
      gen_op(TOK_SHL);
      vpushi(bits - bit_size);
      /* NOTE: transformed to SHR if unsigned */
      gen_op(TOK_SAR);
      vreg = gv(rc);
    }
    r = gv(rc);
  }
  else
  {
    if (is_float(vtop->type.t) && (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
    {
      /* CPUs usually cannot use float constants, so we store them
         generically in data segment */
      init_params p = {rodata_section};
      unsigned long offset;
      size = type_size(&vtop->type, &align);
      if (NODATA_WANTED)
        size = 0, align = 1;
      offset = section_add(p.sec, size, align);
      vpush_ref(&vtop->type, p.sec, offset, size);
      vswap();
      init_putv(&p, &vtop->type, offset, -1);
      vtop->r |= VT_LVAL;
    }
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound();
#endif

    /* Arrays (including VLAs) are not values you can load from memory.
     * In most expressions they decay to a pointer to their first element.
     * If we treat them as an lvalue and "load" them, we end up
     * dereferencing the computed pointer and accidentally using a[0]
     * (or addr[0]) instead of the address itself.
     *
     * This is particularly visible in tests/tests2/79_vla_continue.c where
     * `addr[count] = a;` must store the pointer value of `a`.
     */
    if ((vtop->r & VT_LVAL) && (vtop->type.t & (VT_ARRAY | VT_VLA)))
    {
      gaddrof();
      vtop->type.t &= ~(VT_ARRAY | VT_VLA);
    }

    rc2 = RC_INT; // RC2_TYPE(bt, rc);

    /* need to reload if:
       - constant
       - lvalue (need to dereference pointer)
       - already a register, but not in the right class */
    r = vtop->r & VT_VALMASK;
    r_ok = !(vtop->r & VT_LVAL) && (r < VT_CONST) && (reg_classes[r] & rc);
    r2_ok = !rc2;

    if (tcc_state->ir == NULL)
    {
      if (!nocode_wanted)
        tcc_error("IR-only: gv() requires IR");
      return 0;
    }

    if (tcc_state->ir && rc2)
    {
      /* IR mode: treat 64-bit values as a single vreg, even on targets where
       * the legacy backend would split into two registers.
       *
       * Always materialize into a vreg if we don't already have one.
       */
      if (vtop->vr == -1 || (vtop->r & VT_LVAL) || (vtop->r & VT_VALMASK) >= VT_CONST)
      {
        int vreg = tcc_ir_get_vreg_temp(tcc_state->ir);
        if (is_float(vtop->type.t))
        {
          int is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
          tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
        }
        else if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
        {
          tcc_ir_set_llong_type(tcc_state->ir, vreg);
        }

        vset_VT_JMP();
        SValue dest;
        svalue_init(&dest);
        dest.type = vtop->type;
        dest.vr = vreg;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);

        vtop->vr = vreg;
        vtop->r = 0;
        vtop->c.i = 0;
        vtop->sym = NULL;
      }
      return 0;
    }

    if (!r_ok || !r2_ok)
    {
      /* IR-only: materialize into a vreg; no physical reg allocation. */
      if (rc2)
        tcc_error("IR-only: unexpected legacy 2-reg gv path");

      vreg = tcc_ir_get_vreg_temp(tcc_state->ir);
      if (is_float(vtop->type.t))
      {
        int is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
        tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
      }
      else if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vreg);
      }

      vset_VT_JMP();
      SValue dest;
      svalue_init(&dest);
      dest.type.t = vtop->type.t;
      dest.vr = vreg;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);

      vtop->vr = vreg;
      vtop->r = 0;
      vtop->c.i = 0;
      vtop->sym = NULL;
    }
    /* vtop->vr is set in the IR LOAD/ASSIGN paths when needed */
  }
  return 0;
}

/* generate vtop[-1] and vtop[0] in resp. classes rc1 and rc2 */
ST_FUNC void gv2(int rc1, int rc2)
{
  /* generate more generic register first. But VT_JMP or VT_CMP
     values must be generated first in all cases to avoid possible
     reload errors */
  if (vtop->r != VT_CMP && rc1 <= rc2)
  {
    vswap();
    gv(rc1);
    vswap();
    gv(rc2);
    /* test if reload is needed for first register */
    if ((vtop[-1].r & VT_VALMASK) >= VT_CONST)
    {
      vswap();
      gv(rc1);
      vswap();
    }
  }
  else
  {
    gv(rc2);
    vswap();
    gv(rc1);
    vswap();
    /* test if reload is needed for first register */
    if ((vtop[0].r & VT_VALMASK) >= VT_CONST)
    {
      gv(rc2);
    }
  }
}

#if PTR_SIZE == 4
/* expand 64bit on stack in two ints */
ST_FUNC void lexpand(void)
{
  int u, v;
  u = vtop->type.t & (VT_DEFSIGN | VT_UNSIGNED);
  v = vtop->r & (VT_VALMASK | VT_LVAL);
  if (v == VT_CONST)
  {
    vdup();
    vtop[0].c.i >>= 32;
  }
  else if (v == (VT_LVAL | VT_CONST) || v == (VT_LVAL | VT_LOCAL))
  {
    /* For IR mode, we need to generate explicit load operations */
    if (tcc_state->ir)
    {
      /* Load the full 64-bit value first, then split it */
      SValue full;
      SValue low32;
      SValue shifted64;
      SValue shift_amt;

      memset(&full, 0, sizeof(full));
      full.type.t = vtop->type.t;
      full.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      full.r = 0;
      if ((full.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, full.vr);

      /* Force load of the 64-bit value */
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &full);

      /* Create explicit low32 = (uint32_t)full. */
      memset(&low32, 0, sizeof(low32));
      low32.type.t = VT_INT | u;
      low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      low32.r = 0;
      int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &full, NULL, &low32);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;

      /* Bottom of stack becomes low32. */
      vtop->type.t = VT_INT | u;
      vtop->vr = low32.vr;
      vtop->r = 0;

      /* Duplicate and turn the new top into the high32 word. */
      vdup();
      vtop[0].type.t = VT_INT | u;
      vtop[0].vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      vtop[0].r = 0;

      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      /* shifted64 = full >> 32 (64-bit). */
      memset(&shifted64, 0, sizeof(shifted64));
      shifted64.type.t = VT_LLONG | u;
      shifted64.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      shifted64.r = 0;
      tcc_ir_set_llong_type(tcc_state->ir, shifted64.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHR, &full, &shift_amt, &shifted64);

      /* high32 = (uint32_t)shifted64 (i.e. original high word).
       * IMPORTANT: prevent coalescing here! The SHR must remain a 64-bit operation
       * to correctly extract the high word. If coalesced with this 32-bit ASSIGN,
       * the SHR's dest type would become 32-bit and codegen would emit a 32-bit shift
       * instead of a 64-bit shift, causing the high word to be lost. */
      old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &shifted64, NULL, &vtop[0]);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
    }
    else
    {
      vdup();
      vtop[0].c.i += 4;
    }
  }
  else
  {
    /* For IR mode: materialize the full 64-bit value into a temp vreg first,
     * then create two independent 32-bit values:
     * - low word: low32 = (uint32_t)full
     * - high word: high32 = (uint32_t)(full >> 32)
     *
     * IMPORTANT: do NOT reuse the 64-bit vreg as a 32-bit "view".
     * That causes later 64-bit ops (like lbuild's (high<<32)|low) to
     * accidentally see/propagate the full's high word via pr1.
     * Also, shifting by 32 must be done as a 64-bit shift; emitting a 32-bit
     * SHR #32 is not encodable on Thumb and leads to wrong codegen.
     */
    if (tcc_state->ir)
    {
      SValue full;
      SValue low32;
      SValue shifted64;
      SValue shift_amt;

      memset(&full, 0, sizeof(full));
      full.type.t = vtop->type.t;
      full.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      full.r = 0;
      if ((full.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, full.vr);
      /* Force a value-producing vreg (loads from lvalues if needed). */
      int assign_pos = tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &full);

      /* If coalescing happened, update full.vr to match the coalesced instruction's dest */
      if (assign_pos < tcc_state->ir->next_instruction_index)
      {
        IROperand dest = tcc_ir_get_dest(tcc_state->ir, assign_pos);
        full.vr = irop_get_vreg(dest);
        /* Also update full.type to match the coalesced instruction's dest type! */
        full.type.t = irop_btype_to_vt_btype(irop_get_btype(dest));
        if (dest.is_unsigned)
          full.type.t |= VT_UNSIGNED;
      }

      /* Create explicit low32 = (uint32_t)full. */
      memset(&low32, 0, sizeof(low32));
      low32.type.t = VT_INT | u;
      low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      low32.r = 0;
      int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      int low_assign_pos = tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &full, NULL, &low32);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;

      /* IMPORTANT (IR mode): prevent ASSIGN coalescing here.
       *
       * lexpand splits a 64-bit value `full` into low/high 32-bit words.
       * We still need `full` for the subsequent (full >> 32) extraction.
       *
       * The IR layer has an ASSIGN coalescing peephole that can rewrite the
       * previous instruction's destination to our `low32` and drop this ASSIGN
       * when the source is a TEMP produced by the previous instruction.
       *
       * That optimization is invalid for lexpand: it would make the original
       * `full` vreg undefined for the later shift, causing codegen to read from
       * uninitialized registers (observed as stray use of r9 in mul_s).
       */
      (void)low_assign_pos;

      /* NOTE: do not update full.vr based on this ASSIGN.
       * This instruction produces a 32-bit low word; if we overwrite full.vr
       * here, the later (full >> 32) would accidentally shift the low word,
       * yielding a zero high word and breaking 64-bit math.
       * (low_assign_pos is kept for debugging / symmetry with the earlier ASSIGN.) */
      /* low_assign_pos is kept only for debugging/symmetry. */

      /* Bottom of stack becomes low32. */
      vtop->type.t = VT_INT | u;
      vtop->vr = low32.vr;
      vtop->r = 0;

      /* Duplicate and turn the new top into the high32 word. */
      vdup();
      vtop[0].type.t = VT_INT | u;
      vtop[0].vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      vtop[0].r = 0;

      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      /* shifted64 = full >> 32 (64-bit). */
      memset(&shifted64, 0, sizeof(shifted64));
      shifted64.type.t = VT_LLONG | u;
      shifted64.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      shifted64.r = 0;
      tcc_ir_set_llong_type(tcc_state->ir, shifted64.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHR, &full, &shift_amt, &shifted64);

      /* high32 = (uint32_t)shifted64 (i.e. original high word).
       * IMPORTANT: prevent coalescing here! The SHR must remain a 64-bit operation
       * to correctly extract the high word. If coalesced with this 32-bit ASSIGN,
       * the SHR's dest type would become 32-bit and codegen would emit a 32-bit shift
       * instead of a 64-bit shift, causing the high word to be lost. */
      old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &shifted64, NULL, &vtop[0]);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
    }
  }
  vtop[0].type.t = vtop[-1].type.t = VT_INT | u;
}
#endif

#if PTR_SIZE == 4
/* build a long long from two ints */
static void lbuild(int t)
{
  /* For IR mode: combine low and high vregs into a single 64-bit vreg.
   * Generate an OR operation: (high << 32) | low
   *
   * Handle cases where one or both operands are constants (vr == -1).
   * Constants are encoded with VT_CONST in .r and the value in .c.i.
   */
  if (tcc_state->ir)
  {
    SValue low = vtop[-1];
    SValue high = vtop[0];
    /* Check if we have valid operands (either vreg or constant) */
    int low_is_const = (low.vr < 0) && ((low.r & VT_VALMASK) == VT_CONST);
    int high_is_const = (high.vr < 0) && ((high.r & VT_VALMASK) == VT_CONST);
    int low_is_vreg = (low.vr >= 0);
    int high_is_vreg = (high.vr >= 0);

    /* Only proceed if both operands are valid (vreg or constant) */
    if ((low_is_vreg || low_is_const) && (high_is_vreg || high_is_const))
    {
      /* Special case: both are constants - compute result directly */
      if (low_is_const && high_is_const)
      {
        uint64_t result_val = ((uint64_t)(uint32_t)high.c.i << 32) | (uint32_t)low.c.i;
        vtop[-1].c.i = (long long)result_val;
        vtop[-1].type.t = t;
        vtop[-1].r = VT_CONST;
        vtop[-1].vr = -1;
        vpop();
        return;
      }

      /* In IR mode, vtop entries may still carry address-like VT_LOCAL
       * flags. lbuild must operate on the VALUES, not addresses.
       * Force both operands to be treated as rvalues when emitting IR. */
      {
        const int low_kind = low.r & VT_VALMASK;
        if ((low_kind == VT_LOCAL || low_kind == VT_LLOCAL) && !(low.r & VT_LVAL))
          low.r |= VT_LVAL;
        const int high_kind = high.r & VT_VALMASK;
        if ((high_kind == VT_LOCAL || high_kind == VT_LLOCAL) && !(high.r & VT_LVAL))
          high.r |= VT_LVAL;
      }

      /* Create new 64-bit temp vreg for result */
      int result_vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      if ((t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, result_vr);
      /* Special case: high word is constant 0 - just assign/extend low to 64-bit */
      if (high_is_const && high.c.i == 0)
      {
        /* Result is just the low word zero-extended to 64-bit.
         * Generate: result = low | 0 (or just assign if low is already correct) */
        SValue result;
        memset(&result, 0, sizeof(result));
        result.type.t = t;
        result.vr = result_vr;
        result.r = 0;
        if ((result.type.t & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, result.vr);

        /* For zero-extension, we can use ASSIGN with proper type or OR with 0 */
        SValue zero;
        memset(&zero, 0, sizeof(zero));
        zero.type.t = VT_LLONG;
        zero.r = VT_CONST;
        zero.c.i = 0;
        zero.vr = -1;

        tcc_ir_put(tcc_state->ir, TCCIR_OP_OR, &low, &zero, &result);

        vtop[-1].vr = result_vr;
        vtop[-1].type.t = t;
        vtop[-1].r = 0;
        vpop();
        return;
      }

      /* First shift high word left by 32: high_shifted = high << 32 */
      SValue shift_amt;
      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      SValue high_shifted;
      memset(&high_shifted, 0, sizeof(high_shifted));
      high_shifted.type.t = VT_LLONG;
      high_shifted.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_set_llong_type(tcc_state->ir, high_shifted.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHL, &high, &shift_amt, &high_shifted);

      /* Then OR with low word: result = high_shifted | low */
      SValue result;
      memset(&result, 0, sizeof(result));
      result.type.t = t;
      result.vr = result_vr;
      if ((result.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, result.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_OR, &high_shifted, &low, &result);

      vtop[-1].vr = result_vr;
      vtop[-1].type.t = t;
      vtop[-1].r = 0;
      vpop();
      return;
    }
  }
}
#endif

/* convert stack entry to register and duplicate its value in another
   register */
static void gv_dup(void)
{
  int t;
  SValue sv;

  t = vtop->type.t;
#if PTR_SIZE == 4
  if ((t & VT_BTYPE) == VT_LLONG)
  {
    if (t & VT_BITFIELD)
    {
      gv(RC_INT);
      t = vtop->type.t;
    }
    lexpand();
    gv_dup();
    vswap();
    vrotb(3);
    gv_dup();
    vrotb(4);
    /* stack: H L L1 H1 */
    lbuild(t);
    vrotb(3);
    vrotb(3);
    vswap();
    lbuild(t);
    vswap();
    return;
  }
#endif
  sv.type.t = VT_INT;
  sv.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  sv.r = 0;
  sv.c.i = 0;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &sv);
  vtop->vr = sv.vr;
  vtop->r = 0;
  vtop->c.i = 0; /* Clear c.i to avoid corrupting later operations */
  vdup();
}

#if PTR_SIZE == 4
/* generate CPU independent (unsigned) long long operations */
static void gen_opl(int op)
{
  int t, op1, c, i;
  int func;
  unsigned short reg_iret = REG_IRET;
  SValue tmp;

  switch (op)
  {
  case '/':
  case TOK_PDIV:
    func = TOK___divdi3;
    goto gen_func;
  case TOK_UDIV:
    func = TOK___udivdi3;
    goto gen_func;
  case '%':
    func = TOK___moddi3;
    goto gen_mod_func;
  case TOK_UMOD:
    func = TOK___umoddi3;
  gen_mod_func:
#ifdef TCC_ARM_EABI
    reg_iret = TREG_R2;
#endif
  gen_func:
    /* call generic long long function */
    vpush_helper_func(func);
    vrott(3);
    /* Stack after vrott(3): func, arg1, arg2 (arg2 is at vtop) */
    {
      SValue param_num;
      SValue dest;
      const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
      svalue_init(&param_num);
      param_num.vr = -1;
      param_num.r = VT_CONST;
      /* Generate FUNCPARAMVAL for arg1 (param 0) */
      param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
      TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=llong_helper call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                   call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-1].r, vtop[-1].vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);
      /* Generate FUNCPARAMVAL for arg2 (param 1) */
      param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
      TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=llong_helper call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                   call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[0].r, vtop[0].vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);
      /* Generate FUNCCALLVAL for the function call (returns long long) */
      svalue_init(&dest);
      dest.type.t = VT_LLONG;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 2);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-2], &call_id_sv, &dest);
      /* Pop all 3 values (arg1, arg2, func) and push result */
      vtop -= 3;
      vpushi(0);
      vtop->type.t = VT_LLONG;
      vtop->vr = dest.vr;
      vtop->r = reg_iret;
    }
    break;
  case '^':
  case '&':
  case '|':
  case '+':
  case '-':
    /* For IR mode: generate 64-bit operations directly without lexpand/lbuild */
    if (tcc_state->ir)
    {
      t = vtop->type.t;
      int dest_type = VT_LLONG | (t & VT_UNSIGNED);
      if (op == '+' || op == '-')
      {
        /* 64-bit add/sub - generate single IR operation */
        SValue dest;
        svalue_init(&dest);
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.type.t = dest_type;
        dest.r = 0;
        if ((dest_type & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, dest.vr);
        TccIrOp ir_op = (op == '+') ? TCCIR_OP_ADD : TCCIR_OP_SUB;
        tcc_ir_put(tcc_state->ir, ir_op, &vtop[-1], &vtop[0], &dest);
        vtop--;
        vtop->vr = dest.vr;
        vtop->type.t = dest_type;
        vtop->r = 0;
      }
      else
      {
        /* 64-bit bitwise ops (^, &, |) - generate single IR operation */
        SValue dest;
        svalue_init(&dest);
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.type.t = dest_type;
        dest.r = 0;
        if ((dest_type & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, dest.vr);
        TccIrOp ir_op;
        switch (op)
        {
        case '^':
          ir_op = TCCIR_OP_XOR;
          break;
        case '&':
          ir_op = TCCIR_OP_AND;
          break;
        case '|':
          ir_op = TCCIR_OP_OR;
          break;
        }
        tcc_ir_put(tcc_state->ir, ir_op, &vtop[-1], &vtop[0], &dest);
        vtop--;
        vtop->vr = dest.vr;
        vtop->type.t = dest_type;
        vtop->r = 0;
      }
      break;
    }
    /* Fall through for non-IR mode */
    /* FALLTHROUGH */
  case '*':
    t = vtop->type.t; /* Save type for lbuild at end */
    vswap();
    lexpand();
    vrotb(3);
    lexpand();
    /* stack: L1 H1 L2 H2 */
    tmp = vtop[0];
    vtop[0] = vtop[-3];
    vtop[-3] = tmp;
    tmp = vtop[-2];
    vtop[-2] = vtop[-3];
    vtop[-3] = tmp;
    vswap();
    /* stack: H1 H2 L1 L2 */
    // pv("gen_opl B", 0, 4);
    if (op == '*')
    {
      vpushv(vtop - 1);
      vpushv(vtop - 1);
      gen_op(TOK_UMULL);
      lexpand();
      /* stack: H1 H2 L1 L2 ML MH */
      for (i = 0; i < 4; i++)
        vrotb(6);
      /* stack: ML MH H1 H2 L1 L2 */
      tmp = vtop[0];
      vtop[0] = vtop[-2];
      vtop[-2] = tmp;
      /* stack: ML MH H1 L2 H2 L1 */
      gen_op('*');
      vrotb(3);
      vrotb(3);
      gen_op('*');
      /* stack: ML MH M1 M2 */
      gen_op('+');
      gen_op('+');
    }
    else if (op == '+' || op == '-')
    {
      /* XXX: add non carry method too (for MIPS or alpha) */
      if (op == '+')
        op1 = TOK_ADDC1;
      else
        op1 = TOK_SUBC1;
      gen_op(op1);
      /* stack: H1 H2 (L1 op L2) */
      vrotb(3);
      vrotb(3);
      gen_op(op1 + 1); /* TOK_xxxC2 */
    }
    else
    {
      gen_op(op);
      /* stack: H1 H2 (L1 op L2) */
      vrotb(3);
      vrotb(3);
      /* stack: (L1 op L2) H1 H2 */
      gen_op(op);
      /* stack: (L1 op L2) (H1 op H2) */
    }
    /* stack: L H */
    lbuild(t);
    break;
  case TOK_SAR:
  case TOK_SHR:
  case TOK_SHL:
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      t = vtop[-1].type.t;
      vswap();
      lexpand();
      vrotb(3);
      /* stack: L H shift */
      c = (int)vtop->c.i;
      /* constant: simpler */
      /* NOTE: all comments are for SHL. the other cases are
         done by swapping words */
      vpop();
      if (op != TOK_SHL)
        vswap();
      if (c >= 32)
      {
        /* stack: L H */
        vpop();
        if (c > 32)
        {
          vpushi(c - 32);
          gen_op(op);
        }
        if (op != TOK_SAR)
        {
          vpushi(0);
        }
        else
        {
          gv_dup();
          vpushi(31);
          gen_op(TOK_SAR);
        }
        vswap();
      }
      else
      {
        vswap();
        gv_dup();
        /* stack: H L L */
        vpushi(c);
        gen_op(op);
        vswap();
        vpushi(32 - c);
        if (op == TOK_SHL)
          gen_op(TOK_SHR);
        else
          gen_op(TOK_SHL);
        vrotb(3);
        /* stack: L L H */
        vpushi(c);
        if (op == TOK_SHL)
          gen_op(TOK_SHL);
        else
          gen_op(TOK_SHR);
        gen_op('|');
      }
      if (op != TOK_SHL)
        vswap();
      lbuild(t);
    }
    else
    {
      /* XXX: should provide a faster fallback on x86 ? */
      switch (op)
      {
      case TOK_SAR:
        func = TOK___ashrdi3;
        goto gen_func;
      case TOK_SHR:
        func = TOK___lshrdi3;
        goto gen_func;
      case TOK_SHL:
        func = TOK___ashldi3;
        goto gen_func;
      }
    }
    break;
  default:
    /* compare operations - use __aeabi_lcmp/__aeabi_ulcmp for ARM EABI */
    t = vtop->type.t;
    {
      int is_unsigned = (op == TOK_ULT || op == TOK_ULE || op == TOK_UGT || op == TOK_UGE);
      func = is_unsigned ? TOK___aeabi_ulcmp : TOK___aeabi_lcmp;

      /* Call the comparison helper function */
      vpush_helper_func(func);
      vrott(3);
      /* Stack after vrott(3): func, arg1, arg2 (arg2 is at vtop) */
      {
        SValue param_num;
        SValue dest;
        const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
        svalue_init(&param_num);
        param_num.vr = -1;
        /* Generate FUNCPARAMVAL for arg1 (param 0) */
        param_num.r = VT_CONST;
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
        TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=aeabi_lcmp call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                     call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-1].r, vtop[-1].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);
        /* Generate FUNCPARAMVAL for arg2 (param 1) */
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
        TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=aeabi_lcmp call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                     call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[0].r, vtop[0].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);
        /* Generate FUNCCALLVAL for the function call (returns int: -1, 0, or 1) */
        svalue_init(&dest);
        dest.type.t = VT_INT;
        dest.r = 0;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 2);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-2], &call_id_sv, &dest);
        /* Pop all 3 values (arg1, arg2, func) and push result */
        vtop -= 3;
        vpushi(0);
        vtop->type.t = VT_INT;
        vtop->vr = dest.vr;
        vtop->r = REG_IRET;
      }

      /* Now compare the result (in r0) against 0 using the appropriate comparison */
      /* __aeabi_lcmp returns: <0 if a<b, 0 if a==b, >0 if a>b */
      vpushi(0);
      switch (op)
      {
      case TOK_LT:
      case TOK_ULT:
        /* result < 0 means a < b */
        gen_op(TOK_LT);
        break;
      case TOK_LE:
      case TOK_ULE:
        /* result <= 0 means a <= b */
        gen_op(TOK_LE);
        break;
      case TOK_GT:
      case TOK_UGT:
        /* result > 0 means a > b */
        gen_op(TOK_GT);
        break;
      case TOK_GE:
      case TOK_UGE:
        /* result >= 0 means a >= b */
        gen_op(TOK_GE);
        break;
      case TOK_EQ:
        /* result == 0 means a == b */
        gen_op(TOK_EQ);
        break;
      case TOK_NE:
        /* result != 0 means a != b */
        gen_op(TOK_NE);
        break;
      }
    }
    break;
  }
}
#endif

/* normalize values */
static uint64_t value64(uint64_t l1, int t)
{
  uint64_t result;
  if ((t & VT_BTYPE) == VT_LLONG || (PTR_SIZE == 8 && (t & VT_BTYPE) == VT_PTR))
    result = l1;
  else if (t & VT_UNSIGNED)
    result = (uint32_t)l1;
  else
    result = (uint32_t)l1 | -(l1 & 0x80000000);
  return result;
}

static uint64_t gen_opic_sdiv(uint64_t a, uint64_t b)
{
  uint64_t x = (a >> 63 ? -a : a) / (b >> 63 ? -b : b);
  return (a ^ b) >> 63 ? -x : x;
}

static int gen_opic_lt(uint64_t a, uint64_t b)
{
  return (a ^ (uint64_t)1 << 63) < (b ^ (uint64_t)1 << 63);
}

/* handle integer constant optimizations and various machine
   independent opt */
static void gen_opic(int op)
{
  SValue *v1 = vtop - 1;
  SValue *v2 = vtop;
  int t1 = v1->type.t & VT_BTYPE;
  int t2 = v2->type.t & VT_BTYPE;
  int c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  uint64_t l1 = c1 ? value64(v1->c.i, v1->type.t) : 0;
  uint64_t l2 = c2 ? value64(v2->c.i, v2->type.t) : 0;
  int shm = (t1 == VT_LLONG) ? 63 : 31;
  int r;

  if (c1 && c2)
  {
    switch (op)
    {
    case '+':
      l1 += l2;
      break;
    case '-':
      l1 -= l2;
      break;
    case '&':
      l1 &= l2;
      break;
    case '^':
      l1 ^= l2;
      break;
    case '|':
      l1 |= l2;
      break;
    case '*':
      l1 *= l2;
      break;

    case TOK_PDIV:
    case '/':
    case '%':
    case TOK_UDIV:
    case TOK_UMOD:
      /* if division by zero, generate explicit division */
      if (l2 == 0)
      {
        if (CONST_WANTED && !NOEVAL_WANTED)
          tcc_error("division by zero in constant");
        goto general_case;
      }
      switch (op)
      {
      default:
        l1 = gen_opic_sdiv(l1, l2);
        break;
      case '%':
        l1 = l1 - l2 * gen_opic_sdiv(l1, l2);
        break;
      case TOK_UDIV:
        l1 = l1 / l2;
        break;
      case TOK_UMOD:
        l1 = l1 % l2;
        break;
      }
      break;
    case TOK_SHL:
      l1 <<= (l2 & shm);
      break;
    case TOK_SHR:
      l1 >>= (l2 & shm);
      break;
    case TOK_SAR:
      l1 = (l1 >> 63) ? ~(~l1 >> (l2 & shm)) : l1 >> (l2 & shm);
      break;
      /* tests */
    case TOK_ULT:
      l1 = l1 < l2;
      break;
    case TOK_UGE:
      l1 = l1 >= l2;
      break;
    case TOK_EQ:
      l1 = l1 == l2;
      break;
    case TOK_NE:
      l1 = l1 != l2;
      break;
    case TOK_ULE:
      l1 = l1 <= l2;
      break;
    case TOK_UGT:
      l1 = l1 > l2;
      break;
    case TOK_LT:
      l1 = gen_opic_lt(l1, l2);
      break;
    case TOK_GE:
      l1 = !gen_opic_lt(l1, l2);
      break;
    case TOK_LE:
      l1 = !gen_opic_lt(l2, l1);
      break;
    case TOK_GT:
      l1 = gen_opic_lt(l2, l1);
      break;
      /* logical */
    case TOK_LAND:
      l1 = l1 && l2;
      break;
    case TOK_LOR:
      l1 = l1 || l2;
      break;
    default:
      goto general_case;
    }
    v1->c.i = value64(l1, v1->type.t);
    v1->r |= v2->r & VT_NONCONST;
    vtop--;
    print_vstack("gen_opic(0)");
  }
  else
  {
    /* if commutative ops, put c2 as constant */
    if (c1 && (op == '+' || op == '&' || op == '^' || op == '|' || op == '*' || op == TOK_EQ || op == TOK_NE))
    {
      vswap();
      c2 = c1; // c = c1, c1 = c2, c2 = c;
      l2 = l1; // l = l1, l1 = l2, l2 = l;
    }
    if (c1 && ((l1 == 0 && (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)) || (l1 == -1 && op == TOK_SAR)))
    {
      /* treat (0 << x), (0 >> x) and (-1 >> x) as constant */
      vpop();
    }
    else if (c2 && ((l2 == 0 && (op == '&' || op == '*')) ||
                    (op == '|' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG))) ||
                    (l2 == 1 && (op == '%' || op == TOK_UMOD))))
    {
      /* treat (x & 0), (x * 0), (x | -1) and (x % 1) as constant */
      if (l2 == 1)
        vtop->c.i = 0;
      vswap();
      vtop--;
      print_vstack("gen_opic(1)");
    }
    else if (c2 &&
             (((op == '*' || op == '/' || op == TOK_UDIV || op == TOK_PDIV) && l2 == 1) ||
              ((op == '+' || op == '-' || op == '|' || op == '^' || op == TOK_SHL || op == TOK_SHR || op == TOK_SAR) &&
               l2 == 0) ||
              (op == '&' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG)))))
    {
      /* filter out NOP operations like x*1, x-0, x&-1... */
      vtop--;
      print_vstack("gen_opic(2)");
    }
    else if (c2 && (op == '*' || op == TOK_PDIV || op == TOK_UDIV))
    {
      /* try to use shifts instead of muls or divs */
      if (l2 > 0 && (l2 & (l2 - 1)) == 0)
      {
        int n = -1;
        while (l2)
        {
          l2 >>= 1;
          n++;
        }
        vtop->c.i = n;
        if (op == '*')
          op = TOK_SHL;
        else if (op == TOK_PDIV)
          op = TOK_SAR;
        else
          op = TOK_SHR;
      }
      goto general_case;
    }
    else if (c2 && (op == '+' || op == '-') &&
             (r = vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM), r == (VT_CONST | VT_SYM) || r == VT_LOCAL))
    {
      /* symbol + constant case */
      if (op == '-')
        l2 = -l2;
      l2 += vtop[-1].c.i;
      /* The backends can't always deal with addends to symbols
         larger than +-1<<31.  Don't construct such.  */
      if ((int)l2 != l2)
        goto general_case;
      vtop--;
      print_vstack("gen_opic(3)");
      vtop->c.i = l2;
    }
    else
    {
    general_case:
      /* call low level op generator */
      if (t1 == VT_LLONG || t2 == VT_LLONG || (PTR_SIZE == 8 && (t1 == VT_PTR || t2 == VT_PTR)))
        gen_opl(op);
      else
      {
        // gen_opi(op);
        tcc_ir_gen_i(tcc_state->ir, op);
      }
    }
    if (vtop->r == VT_CONST)
      vtop->r |= VT_NONCONST; /* is const, but only by optimization */
  }
}

#if defined TCC_TARGET_X86_64 || defined TCC_TARGET_I386
#define gen_negf gen_opf
#elif defined TCC_TARGET_ARM
void gen_negf(int op)
{
  /* arm will detect 0-x and replace by vneg */
  vpushi(0), vswap(), gen_op('-');
}
#else
/* XXX: implement in gen_opf() for other backends too */
void gen_negf(int op)
{
  /* In IEEE negate(x) isn't subtract(0,x).  Without NaNs it's
     subtract(-0, x), but with them it's really a sign flip
     operation.  We implement this with bit manipulation and have
     to do some type reinterpretation for this, which TCC can do
     only via memory.  */

  int align, size, bt;

  size = type_size(&vtop->type, &align);
  bt = vtop->type.t & VT_BTYPE;
  gv(RC_TYPE(bt));
  vdup();
  incr_bf_adr(size - 1);
  vdup();
  vpushi(0x80); /* flip sign */
  gen_op('^');
  vstore();
  vpop();
}
#endif

/* generate a floating point operation with constant propagation */
static void gen_opif(int op)
{
  int c1, c2, i, bt;
  SValue *v1, *v2;
#if defined _MSC_VER && defined __x86_64__
  /* avoid bad optimization with f1 -= f2 for f1:-0.0, f2:0.0 */
  volatile
#endif
      long double f1,
      f2;

  v1 = vtop - 1;
  v2 = vtop;
  if (op == TOK_NEG)
    v1 = v2;
  bt = v1->type.t & VT_BTYPE;

  /* currently, we cannot do computations with forward symbols */
  c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  if (c1 && c2)
  {
    if (bt == VT_FLOAT)
    {
      f1 = v1->c.f;
      f2 = v2->c.f;
    }
    else if (bt == VT_DOUBLE)
    {
      f1 = v1->c.d;
      f2 = v2->c.d;
    }
    else
    {
      f1 = v1->c.ld;
      f2 = v2->c.ld;
    }
    /* NOTE: we only do constant propagation if finite number (not
       NaN or infinity) (ANSI spec) */
    if (!(ieee_finite(f1) || !ieee_finite(f2)) && !CONST_WANTED)
      goto general_case;
    switch (op)
    {
    case '+':
      f1 += f2;
      break;
    case '-':
      f1 -= f2;
      break;
    case '*':
      f1 *= f2;
      break;
    case '/':
      if (f2 == 0.0)
      {
        union
        {
          float f;
          unsigned u;
        } x1, x2, y;
        /* If not in initializer we need to potentially generate
           FP exceptions at runtime, otherwise we want to fold.  */
        if (!CONST_WANTED)
          goto general_case;
        /* the run-time result of 0.0/0.0 on x87, also of other compilers
           when used to compile the f1 /= f2 below, would be -nan */
        x1.f = f1, x2.f = f2;
        if (f1 == 0.0)
          y.u = 0x7fc00000; /* nan */
        else
          y.u = 0x7f800000;                /* infinity */
        y.u |= (x1.u ^ x2.u) & 0x80000000; /* set sign */
        f1 = y.f;
        break;
      }
      f1 /= f2;
      break;
    case TOK_NEG:
      f1 = -f1;
      goto unary_result;
    case TOK_EQ:
      i = f1 == f2;
    make_int:
      vtop -= 2;
      print_vstack("gen_opif(0)");
      vpushi(i);
      return;
    case TOK_NE:
      i = f1 != f2;
      goto make_int;
    case TOK_LT:
      i = f1 < f2;
      goto make_int;
    case TOK_GE:
      i = f1 >= f2;
      goto make_int;
    case TOK_LE:
      i = f1 <= f2;
      goto make_int;
    case TOK_GT:
      i = f1 > f2;
      goto make_int;
    default:
      goto general_case;
    }
    vtop--;
    print_vstack("gen_opif(1)");
  unary_result:
    /* XXX: overflow test ? */
    if (bt == VT_FLOAT)
    {
      v1->c.f = f1;
    }
    else if (bt == VT_DOUBLE)
    {
      v1->c.d = f1;
    }
    else
    {
      v1->c.ld = f1;
    }
  }
  else
  {
  general_case:
    if (op == TOK_NEG)
    {
      gen_negf(op);
    }
    else
    {
      // gen_opf(op);
      tcc_ir_gen_f(tcc_state->ir, op);
    }
  }
}

/* print a type. If 'varstr' is not NULL, then the variable is also
   printed in the type */
/* XXX: union */
/* XXX: add array and function pointers */
static void type_to_str(char *buf, int buf_size, CType *type, const char *varstr)
{
  int bt, v, t;
  Sym *s, *sa;
  char buf1[256];
  const char *tstr;

  t = type->t;
  bt = t & VT_BTYPE;
  buf[0] = '\0';

  if (t & VT_EXTERN)
    pstrcat(buf, buf_size, "extern ");
  if (t & VT_STATIC)
    pstrcat(buf, buf_size, "static ");
  if (t & VT_TYPEDEF)
    pstrcat(buf, buf_size, "typedef ");
  if (t & VT_INLINE)
    pstrcat(buf, buf_size, "inline ");
  if (bt != VT_PTR)
  {
    if (t & VT_VOLATILE)
      pstrcat(buf, buf_size, "volatile ");
    if (t & VT_CONSTANT)
      pstrcat(buf, buf_size, "const ");
  }
  if (((t & VT_DEFSIGN) && bt == VT_BYTE) ||
      ((t & VT_UNSIGNED) && (bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG) && !IS_ENUM(t)))
    pstrcat(buf, buf_size, (t & VT_UNSIGNED) ? "unsigned " : "signed ");

  buf_size -= strlen(buf);
  buf += strlen(buf);

  /* DONE: Phase 1 - Handle complex types in type_to_str() */
  if (t & VT_COMPLEX)
  {
    if (bt == VT_FLOAT)
      pstrcat(buf, buf_size, "float _Complex");
    else if (bt == VT_DOUBLE)
      pstrcat(buf, buf_size, "double _Complex");
    else if (bt == VT_LDOUBLE)
      pstrcat(buf, buf_size, "long double _Complex");
    else
      pstrcat(buf, buf_size, "_Complex");
  }
  else
    switch (bt)
    {
    case VT_VOID:
      tstr = "void";
      goto add_tstr;
    case VT_BOOL:
      tstr = "_Bool";
      goto add_tstr;
    case VT_BYTE:
      tstr = "char";
      goto add_tstr;
    case VT_SHORT:
      tstr = "short";
      goto add_tstr;
    case VT_INT:
      tstr = "int";
      goto maybe_long;
    case VT_LLONG:
      tstr = "long long";
    maybe_long:
      if (t & VT_LONG)
        tstr = "long";
      if (!IS_ENUM(t))
        goto add_tstr;
      tstr = "enum ";
      goto tstruct;
    case VT_FLOAT:
      tstr = "float";
      goto add_tstr;
    case VT_DOUBLE:
      tstr = "double";
      if (!(t & VT_LONG))
        goto add_tstr;
    case VT_LDOUBLE:
      tstr = "long double";
    add_tstr:
      pstrcat(buf, buf_size, tstr);
      break;
    case VT_STRUCT:
      tstr = "struct ";
      if (IS_UNION(t))
        tstr = "union ";
    tstruct:
      pstrcat(buf, buf_size, tstr);
      v = type->ref->v & ~SYM_STRUCT;
      if (v >= SYM_FIRST_ANOM)
        pstrcat(buf, buf_size, "<anonymous>");
      else
        pstrcat(buf, buf_size, get_tok_str(v, NULL));
      break;
    case VT_FUNC:
      s = type->ref;
      buf1[0] = 0;
      if (varstr && '*' == *varstr)
      {
        pstrcat(buf1, sizeof(buf1), "(");
        pstrcat(buf1, sizeof(buf1), varstr);
        pstrcat(buf1, sizeof(buf1), ")");
      }
      pstrcat(buf1, buf_size, "(");
      sa = s->next;
      while (sa != NULL)
      {
        char buf2[256];
        type_to_str(buf2, sizeof(buf2), &sa->type, NULL);
        pstrcat(buf1, sizeof(buf1), buf2);
        sa = sa->next;
        if (sa)
          pstrcat(buf1, sizeof(buf1), ", ");
      }
      if (s->f.func_type == FUNC_ELLIPSIS)
        pstrcat(buf1, sizeof(buf1), ", ...");
      pstrcat(buf1, sizeof(buf1), ")");
      type_to_str(buf, buf_size, &s->type, buf1);
      goto no_var;
    case VT_PTR:
      s = type->ref;
      if (t & (VT_ARRAY | VT_VLA))
      {
        if (varstr && '*' == *varstr)
          snprintf(buf1, sizeof(buf1), "(%s)[%d]", varstr, s->c);
        else
          snprintf(buf1, sizeof(buf1), "%s[%d]", varstr ? varstr : "", s->c);
        type_to_str(buf, buf_size, &s->type, buf1);
        goto no_var;
      }
      pstrcpy(buf1, sizeof(buf1), "*");
      if (t & VT_CONSTANT)
        pstrcat(buf1, buf_size, "const ");
      if (t & VT_VOLATILE)
        pstrcat(buf1, buf_size, "volatile ");
      if (varstr)
        pstrcat(buf1, sizeof(buf1), varstr);
      type_to_str(buf, buf_size, &s->type, buf1);
      goto no_var;
    }
  if (varstr)
  {
    pstrcat(buf, buf_size, " ");
    pstrcat(buf, buf_size, varstr);
  }
no_var:;
}

static void type_incompatibility_error(CType *st, CType *dt, const char *fmt)
{
  char buf1[256], buf2[256];
  type_to_str(buf1, sizeof(buf1), st, NULL);
  type_to_str(buf2, sizeof(buf2), dt, NULL);
  tcc_error(fmt, buf1, buf2);
}

static void type_incompatibility_warning(CType *st, CType *dt, const char *fmt)
{
  char buf1[256], buf2[256];
  type_to_str(buf1, sizeof(buf1), st, NULL);
  type_to_str(buf2, sizeof(buf2), dt, NULL);
  tcc_warning(fmt, buf1, buf2);
}

static int pointed_size(CType *type)
{
  int align;
  return type_size(pointed_type(type), &align);
}

static inline int is_null_pointer(SValue *p)
{
  if ((p->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
    return 0;
  return ((p->type.t & VT_BTYPE) == VT_INT && (uint32_t)p->c.i == 0) ||
         ((p->type.t & VT_BTYPE) == VT_LLONG && p->c.i == 0) ||
         ((p->type.t & VT_BTYPE) == VT_PTR && (PTR_SIZE == 4 ? (uint32_t)p->c.i == 0 : p->c.i == 0) &&
          ((pointed_type(&p->type)->t & VT_BTYPE) == VT_VOID) &&
          0 == (pointed_type(&p->type)->t & (VT_CONSTANT | VT_VOLATILE)));
}

/* compare function types. OLD functions match any new functions */
static int is_compatible_func(CType *type1, CType *type2)
{
  Sym *s1, *s2;

  s1 = type1->ref;
  s2 = type2->ref;
  if (s1->f.func_call != s2->f.func_call)
    return 0;
  if (s1->f.func_type != s2->f.func_type && s1->f.func_type != FUNC_OLD && s2->f.func_type != FUNC_OLD)
    return 0;
  for (;;)
  {
    if (!is_compatible_unqualified_types(&s1->type, &s2->type))
      return 0;
    if (s1->f.func_type == FUNC_OLD || s2->f.func_type == FUNC_OLD)
      return 1;
    s1 = s1->next;
    s2 = s2->next;
    if (!s1)
      return !s2;
    if (!s2)
      return 0;
  }
  return 0; /* unreachable */
}

/* return true if type1 and type2 are the same.  If unqualified is
   true, qualifiers on the types are ignored.
 */
static int compare_types(CType *type1, CType *type2, int unqualified)
{
  int bt1, t1, t2;

  if (IS_ENUM(type1->t))
  {
    if (IS_ENUM(type2->t))
      return type1->ref == type2->ref;
    type1 = &type1->ref->type;
  }
  else if (IS_ENUM(type2->t))
    type2 = &type2->ref->type;

  t1 = type1->t & VT_TYPE;
  t2 = type2->t & VT_TYPE;
  if (unqualified)
  {
    /* strip qualifiers before comparing */
    t1 &= ~(VT_CONSTANT | VT_VOLATILE);
    t2 &= ~(VT_CONSTANT | VT_VOLATILE);
  }

  /* Default Vs explicit signedness only matters for char */
  if ((t1 & VT_BTYPE) != VT_BYTE)
  {
    t1 &= ~VT_DEFSIGN;
    t2 &= ~VT_DEFSIGN;
  }
  /* XXX: bitfields ? */
  if (t1 != t2)
    return 0;

  if ((t1 & VT_ARRAY) && !(type1->ref->c < 0 || type2->ref->c < 0 || type1->ref->c == type2->ref->c))
    return 0;

  /* test more complicated cases */
  bt1 = t1 & VT_BTYPE;
  if (bt1 == VT_PTR)
  {
    type1 = pointed_type(type1);
    type2 = pointed_type(type2);
    return is_compatible_types(type1, type2);
  }
  else if (bt1 == VT_STRUCT)
  {
    return (type1->ref == type2->ref);
  }
  else if (bt1 == VT_FUNC)
  {
    return is_compatible_func(type1, type2);
  }
  else
  {
    return 1;
  }
  return 0; /* unreachable */
}

#define CMP_OP 'C'
#define SHIFT_OP 'S'

static int get_int_type_bits(void)
{
  CType it;
  int align;
  it.t = VT_INT;
  it.ref = NULL;
  return type_size(&it, &align) * 8;
}

static int promote_bitfield_expr_type(int t)
{
  /* Apply integer promotions for bit-field expressions.
     - For bit-fields based on long long/unsigned long long: keep that type.
     - For bit-fields based on <= int rank: promote to int, except an
       unsigned bit-field of full int width promotes to unsigned int.

     This matters because combine_types() runs before gv() has extracted the
     bit-field and removed VT_BITFIELD, so we must reason about promotions
     using BIT_SIZE(). */
  int bt = t & VT_BTYPE;
  int is_unsigned = t & VT_UNSIGNED;
  int bf_size = BIT_SIZE(t);

  t &= ~VT_STRUCT_MASK;

  if (bt == VT_LLONG)
  {
    /* Keep (un)signed long long. */
    return t;
  }

  /* Promote to int, potentially unsigned int. */
  t = (t & ~(VT_BTYPE | VT_UNSIGNED | VT_LONG)) | VT_INT;
  if (is_unsigned && bf_size == get_int_type_bits())
    t |= VT_UNSIGNED;
  return t;
}

/* Check if OP1 and OP2 can be "combined" with operation OP, the combined
   type is stored in DEST if non-null (except for pointer plus/minus) . */
static int combine_types(CType *dest, SValue *op1, SValue *op2, int op)
{
  CType *type1, *type2, type;
  int t1, t2, bt1, bt2;
  int ret = 1;

  /* for shifts, 'combine' only left operand */
  if (op == SHIFT_OP)
    op2 = op1;

  type1 = &op1->type, type2 = &op2->type;
  t1 = type1->t, t2 = type2->t;

  if (t1 & VT_BITFIELD)
    t1 = promote_bitfield_expr_type(t1);
  if (t2 & VT_BITFIELD)
    t2 = promote_bitfield_expr_type(t2);

  bt1 = t1 & VT_BTYPE, bt2 = t2 & VT_BTYPE;

  type.t = VT_VOID;
  type.ref = NULL;

  if (bt1 == VT_VOID || bt2 == VT_VOID)
  {
    ret = op == '?' ? 1 : 0;
    /* NOTE: as an extension, we accept void on only one side */
    type.t = VT_VOID;
  }
  else if (bt1 == VT_PTR || bt2 == VT_PTR)
  {
    if (op == '+')
    {
      if (!is_integer_btype(bt1 == VT_PTR ? bt2 : bt1))
        ret = 0;
    }
    /* http://port70.net/~nsz/c/c99/n1256.html#6.5.15p6 */
    /* If one is a null ptr constant the result type is the other.  */
    else if (is_null_pointer(op2))
      type = *type1;
    else if (is_null_pointer(op1))
      type = *type2;
    else if (bt1 != bt2)
    {
      /* accept comparison or cond-expr between pointer and integer
         with a warning */
      if ((op == '?' || op == CMP_OP) && (is_integer_btype(bt1) || is_integer_btype(bt2)))
        tcc_warning("pointer/integer mismatch in %s", op == '?' ? "conditional expression" : "comparison");
      else if (op != '-' || !is_integer_btype(bt2))
        ret = 0;
      type = *(bt1 == VT_PTR ? type1 : type2);
    }
    else
    {
      CType *pt1 = pointed_type(type1);
      CType *pt2 = pointed_type(type2);
      int pbt1 = pt1->t & VT_BTYPE;
      int pbt2 = pt2->t & VT_BTYPE;
      int newquals, copied = 0;
      if (pbt1 != VT_VOID && pbt2 != VT_VOID && !compare_types(pt1, pt2, 1 /*unqualif*/))
      {
        if (op != '?' && op != CMP_OP)
          ret = 0;
        else
          type_incompatibility_warning(type1, type2,
                                       op == '?' ? "pointer type mismatch in conditional expression "
                                                   "('%s' and '%s')"
                                                 : "pointer type mismatch in comparison('%s' and '%s')");
      }
      if (op == '?')
      {
        /* pointers to void get preferred, otherwise the
           pointed to types minus qualifs should be compatible */
        type = *((pbt1 == VT_VOID) ? type1 : type2);
        /* combine qualifs */
        newquals = ((pt1->t | pt2->t) & (VT_CONSTANT | VT_VOLATILE));
        if ((~pointed_type(&type)->t & (VT_CONSTANT | VT_VOLATILE)) & newquals)
        {
          /* copy the pointer target symbol */
          type.ref = sym_push(SYM_FIELD, &type.ref->type, 0, type.ref->c);
          copied = 1;
          pointed_type(&type)->t |= newquals;
        }
        /* pointers to incomplete arrays get converted to
           pointers to completed ones if possible */
        if (pt1->t & VT_ARRAY && pt2->t & VT_ARRAY && pointed_type(&type)->ref->c < 0 &&
            (pt1->ref->c > 0 || pt2->ref->c > 0))
        {
          if (!copied)
            type.ref = sym_push(SYM_FIELD, &type.ref->type, 0, type.ref->c);
          pointed_type(&type)->ref =
              sym_push(SYM_FIELD, &pointed_type(&type)->ref->type, 0, pointed_type(&type)->ref->c);
          pointed_type(&type)->ref->c = 0 < pt1->ref->c ? pt1->ref->c : pt2->ref->c;
        }
      }
    }
    if (op == CMP_OP)
      type.t = VT_SIZE_T;
  }
  else if (bt1 == VT_STRUCT || bt2 == VT_STRUCT)
  {
    if (op != '?' || !compare_types(type1, type2, 1))
      ret = 0;
    type = *type1;
  }
  else if (is_float(bt1) || is_float(bt2))
  {
    if (bt1 == VT_LDOUBLE || bt2 == VT_LDOUBLE)
    {
      type.t = VT_LDOUBLE;
    }
    else if (bt1 == VT_DOUBLE || bt2 == VT_DOUBLE)
    {
      type.t = VT_DOUBLE;
    }
    else
    {
      type.t = VT_FLOAT;
    }
    /* Phase 3: Propagate VT_COMPLEX flag if either operand is complex.
     * Complex arithmetic follows usual arithmetic conversions:
     * - If either operand is complex, the result is complex
     * - For mixed real/complex: real is converted to complex then operation
     */
    if ((t1 & VT_COMPLEX) || (t2 & VT_COMPLEX))
      type.t |= VT_COMPLEX;
  }
  else if (bt1 == VT_LLONG || bt2 == VT_LLONG)
  {
    /* cast to biggest op */
    type.t = VT_LLONG | VT_LONG;
    if (bt1 == VT_LLONG)
      type.t &= t1;
    if (bt2 == VT_LLONG)
      type.t &= t2;
    /* convert to unsigned if it does not fit in a long long */
    if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED) ||
        (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
      type.t |= VT_UNSIGNED;
  }
  else
  {
    /* integer operations */
    type.t = VT_INT | (VT_LONG & (t1 | t2));
    /* convert to unsigned if it does not fit in an integer */
    if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) ||
        (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED))
      type.t |= VT_UNSIGNED;
  }
  if (dest)
    *dest = type;
  return ret;
}

/* generic gen_op: handles types problems */
ST_FUNC void gen_op(int op)
{
  int t1, t2, bt1, bt2, t;
  CType type1, combtype;
  int op_class = op;

  if (op == TOK_SHR || op == TOK_SAR || op == TOK_SHL)
    op_class = SHIFT_OP;
  else if (TOK_ISCOND(op)) /* == != > ... */
    op_class = CMP_OP;

redo:
  t1 = vtop[-1].type.t;
  t2 = vtop[0].type.t;
  bt1 = t1 & VT_BTYPE;
  bt2 = t2 & VT_BTYPE;

  if (bt1 == VT_FUNC || bt2 == VT_FUNC)
  {
    if (bt2 == VT_FUNC)
    {
      mk_pointer(&vtop->type);
      gaddrof();
    }
    if (bt1 == VT_FUNC)
    {
      vswap();
      mk_pointer(&vtop->type);
      gaddrof();
      vswap();
    }
    goto redo;
  }
  else if (!combine_types(&combtype, vtop - 1, vtop, op_class))
  {
  op_err:
    tcc_error("invalid operand types for binary operation");
  }
  else if (bt1 == VT_PTR || bt2 == VT_PTR)
  {
    /* at least one operand is a pointer */
    /* relational op: must be both pointers */
    int align;
    if (op_class == CMP_OP)
      goto std_op;
    /* if both pointers, then it must be the '-' op */
    if (bt1 == VT_PTR && bt2 == VT_PTR)
    {
      if (op != '-')
        goto op_err;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      vtop->type.t &= ~VT_UNSIGNED;
      vrott(3);
      gen_opic(op);
      vtop->type.t = VT_PTRDIFF_T;
      vswap();
      gen_op(TOK_PDIV);
    }
    else
    {
      /* exactly one pointer : must be '+' or '-'. */
      if (op != '-' && op != '+')
        goto op_err;
      /* Put pointer as first operand */
      if (bt2 == VT_PTR)
      {
        vswap();
        t = t1, t1 = t2, t2 = t;
        bt2 = bt1;
      }
#if PTR_SIZE == 4
      if (bt2 == VT_LLONG)
        /* XXX: truncate here because gen_opl can't handle ptr + long long */
        gen_cast_s(VT_INT);
#endif
      type1 = vtop[-1].type;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      gen_op('*');
#ifdef CONFIG_TCC_BCHECK
      if (tcc_state->do_bounds_check && !CONST_WANTED)
      {
        /* if bounded pointers, we generate a special code to
           test bounds */
        if (op == '-')
        {
          vpushi(0);
          vswap();
          gen_op('-');
        }
        gen_bounded_ptr_add();
      }
      else
#endif
      {
        gen_opic(op);
      }
      type1.t &= ~(VT_ARRAY | VT_VLA);
      /* put again type if gen_opic() swaped operands */
      vtop->type = type1;
    }
  }
  else
  {
    /* floats can only be used for a few operations */
    if (is_float(combtype.t) && op != '+' && op != '-' && op != '*' && op != '/' && op_class != CMP_OP)
    {
      goto op_err;
    }
  std_op:
    t = t2 = combtype.t;
    /* special case for shifts and long long: we keep the shift as
       an integer */
    if (op_class == SHIFT_OP)
      t2 = VT_INT;
    /* XXX: currently, some unsigned operations are explicit, so
       we modify them here */
    if (t & VT_UNSIGNED)
    {
      if (op == TOK_SAR)
        op = TOK_SHR;
      else if (op == '/')
        op = TOK_UDIV;
      else if (op == '%')
        op = TOK_UMOD;
      else if (op == TOK_LT)
        op = TOK_ULT;
      else if (op == TOK_GT)
        op = TOK_UGT;
      else if (op == TOK_LE)
        op = TOK_ULE;
      else if (op == TOK_GE)
        op = TOK_UGE;
    }
    vswap();
    gen_cast_s(t);
    vswap();
    gen_cast_s(t2);
    if (is_float(t))
      gen_opif(op);
    else
      gen_opic(op);
    if (op_class == CMP_OP)
    {
      /* relational op: the result is an int */
      vtop->type.t = VT_INT;
    }
    else if (op == TOK_UMULL)
    {
      /* UMULL produces 64-bit result from 32-bit inputs - preserve the type set by tcc_ir_gen_opi */
    }
    else
    {
      vtop->type.t = t;
    }
  }
  // Make sure that we have converted to an rvalue:
  // if (vtop->r & VT_LVAL)
  //   gv(is_float(vtop->type.t & VT_BTYPE) ? RC_FLOAT : RC_INT);
}

#if defined TCC_TARGET_ARM64 || defined TCC_TARGET_RISCV64 || defined TCC_TARGET_ARM
#define gen_cvt_itof1 gen_cvt_itof
#else
/* generic itof for unsigned long long case */
static void gen_cvt_itof1(int t)
{
  if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
  {

    if (t == VT_FLOAT)
      vpush_helper_func(TOK___floatundisf);
#if LDOUBLE_SIZE != 8
    else if (t == VT_LDOUBLE)
      vpush_helper_func(TOK___floatundixf);
#endif
    else
      vpush_helper_func(TOK___floatundidf);
    vrott(2);
    // gfunc_call(1);
    tcc_error("3 implement me");
    vpushi(0);
    PUT_R_RET(vtop, t);
  }
  else
  {
    gen_cvt_itof(t);
  }
}
#endif

/* special delayed cast for char/short */
static void force_charshort_cast(void)
{
  int sbt = BFGET(vtop->r, VT_MUSTCAST) == 2 ? VT_LLONG : VT_INT;
  int dbt = vtop->type.t;
  vtop->r &= ~VT_MUSTCAST;
  vtop->type.t = sbt;
  gen_cast_s(dbt == VT_BOOL ? VT_BYTE | VT_UNSIGNED : dbt);
  vtop->type.t = dbt;
}

static void gen_cast_s(int t)
{
  CType type;
  type.t = t;
  type.ref = NULL;
  gen_cast(&type);
}

/* cast 'vtop' to 'type'. Casting to bitfields is forbidden. */
static void gen_cast(CType *type)
{
  int sbt, dbt, sf, df, c;
  int dbt_bt, sbt_bt, ds, ss, bits, trunc;

  /* special delayed cast for char/short */
  if (vtop->r & VT_MUSTCAST)
    force_charshort_cast();

  /* bitfields first get cast to ints */
  if (vtop->type.t & VT_BITFIELD)
    gv(RC_INT);

  if (IS_ENUM(type->t) && type->ref->c < 0)
    tcc_error("cast to incomplete type");

  dbt = type->t & (VT_BTYPE | VT_UNSIGNED);
  sbt = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
  if (sbt == VT_FUNC)
    sbt = VT_PTR;

again:
  if (sbt != dbt)
  {
    sf = is_float(sbt);
    df = is_float(dbt);
    dbt_bt = dbt & VT_BTYPE;
    sbt_bt = sbt & VT_BTYPE;
    if (dbt_bt == VT_VOID)
      goto done;
    if (sbt_bt == VT_VOID)
    {
    error:
      cast_error(&vtop->type, type);
    }

    c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
#if !defined TCC_IS_NATIVE && !defined TCC_IS_NATIVE_387
    /* don't try to convert to ldouble when cross-compiling
       (except when it's '0' which is needed for arm:gen_negf()) */
    if (dbt_bt == VT_LDOUBLE && !nocode_wanted && (sf || vtop->c.i != 0))
      c = 0;
#endif
    if (c)
    {
      /* constant case: we can do it now */
      /* XXX: in ISOC, cannot do it if error in convert */
      if (sbt == VT_FLOAT)
        vtop->c.ld = vtop->c.f;
      else if (sbt == VT_DOUBLE)
        vtop->c.ld = vtop->c.d;

      if (df)
      {
        if (sbt_bt == VT_LLONG)
        {
          if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 63))
            vtop->c.ld = vtop->c.i;
          else
            vtop->c.ld = -(long double)-vtop->c.i;
        }
        else if (!sf)
        {
          if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 31))
            vtop->c.ld = (uint32_t)vtop->c.i;
          else
            vtop->c.ld = -(long double)-(uint32_t)vtop->c.i;
        }

        if (dbt == VT_FLOAT)
          vtop->c.f = (float)vtop->c.ld;
        else if (dbt == VT_DOUBLE)
          vtop->c.d = (double)vtop->c.ld;
      }
      else if (sf && dbt == VT_BOOL)
      {
        vtop->c.i = (vtop->c.ld != 0);
      }
      else
      {
        if (sf)
        {
          if (dbt & VT_UNSIGNED)
            vtop->c.i = (uint64_t)vtop->c.ld;
          else
            vtop->c.i = (int64_t)vtop->c.ld;
        }
        else if (sbt_bt == VT_LLONG || (PTR_SIZE == 8 && sbt == VT_PTR))
          ;
        else if (sbt & VT_UNSIGNED)
          vtop->c.i = (uint32_t)vtop->c.i;
        else
          vtop->c.i = ((uint32_t)vtop->c.i | -(vtop->c.i & 0x80000000));

        if (dbt_bt == VT_LLONG || (PTR_SIZE == 8 && dbt == VT_PTR))
          ;
        else if (dbt == VT_BOOL)
          vtop->c.i = (vtop->c.i != 0);
        else
        {
          uint32_t m = dbt_bt == VT_BYTE ? 0xff : dbt_bt == VT_SHORT ? 0xffff : 0xffffffff;
          vtop->c.i &= m;
          if (!(dbt & VT_UNSIGNED))
            vtop->c.i |= -(vtop->c.i & ((m >> 1) + 1));
        }
      }
      goto done;
    }
    else if (dbt == VT_BOOL && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM))
    {
      /* addresses are considered non-zero (see tcctest.c:sinit23) */
      vtop->r = VT_CONST;
      vtop->c.i = 1;
      goto done;
    }

    /* cannot generate code for global or static initializers */
    if (nocode_wanted & DATA_ONLY_WANTED)
      goto done;

    /* non constant case: generate code */
    if (dbt == VT_BOOL)
    {
      gen_test_zero(TOK_NE);
      goto done;
    }

    if (sf || df)
    {
      if (sf && df)
      {
        /* convert from fp to fp - emit IR operation */
        SValue dest;
        int dst_is_double = (dbt == VT_DOUBLE || dbt == VT_LDOUBLE);
        dest.type.t = dbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.r = 0;
        dest.c.i = 0;
        /* Mark the temp vreg as float/double for register allocation */
        tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, dst_is_double);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_FTOF, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
      }
      else if (df)
      {
        /* convert int to fp - emit IR operation */
        SValue dest;
        int dst_is_double = (dbt == VT_DOUBLE || dbt == VT_LDOUBLE);
        dest.type.t = dbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        /* Mark the temp vreg as float/double for register allocation */
        tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, dst_is_double);
        dest.r = 0;
        dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_ITOF, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
      }
      else
      {
        /* convert fp to int - emit IR operation */
        SValue dest;
        sbt = dbt;
        if (dbt_bt != VT_LLONG && dbt_bt != VT_INT)
          sbt = VT_INT;
        dest.type.t = sbt;
        dest.type.ref = NULL;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.r = 0;
        dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CVT_FTOI, vtop, NULL, &dest);
        vtop->vr = dest.vr;
        vtop->r = 0;
        goto again; /* may need char/short cast */
      }
      goto done;
    }

    ds = btype_size(dbt_bt);
    ss = btype_size(sbt_bt);
    if (ds == 0 || ss == 0)
      goto error;

    /* same size and no sign conversion needed */
    if (ds == ss && ds >= 4)
      goto done;
    if (dbt_bt == VT_PTR || sbt_bt == VT_PTR)
    {
      tcc_warning("cast between pointer and integer of different size");
      if (sbt_bt == VT_PTR)
      {
        /* put integer type to allow logical operations below */
        vtop->type.t = (PTR_SIZE == 8 ? VT_LLONG : VT_INT);
      }
    }

/* processor allows { int a = 0, b = *(char*)&a; }
   That means that if we cast to less width, we can just
   change the type and read it still later. */
#define ALLOW_SUBTYPE_ACCESS 1

    if (ALLOW_SUBTYPE_ACCESS && (vtop->r & VT_LVAL) && !tcc_state->ir)
    {
      /* value still in memory.
       * NOTE: This optimization is disabled in IR mode because the IR
       * backend may promote stack lvalues to registers during register
       * allocation.  When that happens the byte/halfword memory load
       * that would have done the extension is replaced by a plain
       * register-to-register move, silently dropping the extension.
       * Falling through to the SHL+SAR path below generates explicit
       * IR instructions for the extension which survive regalloc. */
      if (ds <= ss)
      {
        /* For IR mode: when casting from long long to smaller type,
         * we need to generate a proper load of just the low word,
         * not rely on implicit truncation */
        if (ss == 8 && ds <= 4 && vtop->vr < 0)
        {
          /* Generate LOAD IR for the low word only by changing type first */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE) | dbt_bt;
        }
        goto done;
      }
      /* ss <= 4 here */
      if (ds <= 4 && !(dbt == (VT_SHORT | VT_UNSIGNED) && sbt == VT_BYTE))
      {
        gv(RC_INT);
        goto done; /* no 64bit envolved */
      }
    }
    gv(RC_INT);

    trunc = 0;
#if PTR_SIZE == 4
    if (ds == 8)
    {
      /* generate high word */
      if (sbt & VT_UNSIGNED)
      {
        vpushi(0);
        gv(RC_INT);
      }
      else
      {
        gv_dup();
        vpushi(31);
        gen_op(TOK_SAR);
      }
      lbuild(dbt);
    }
    else if (ss == 8)
    {
      /* from long long: take low order word
       * IMPORTANT (IR mode): do NOT retag the existing 64-bit vreg as 32-bit.
       * That would break subsequent uses that still need the full 64-bit value
       * (e.g. high-word extraction via SHR #32), causing 32-bit shifts and
       * lost high words. Instead, materialize a new 32-bit temp. */
      if (tcc_state->ir && TCCIR_DECODE_VREG_TYPE(vtop->vr) > 0)
      {
        SValue low32;
        memset(&low32, 0, sizeof(low32));
        low32.type.t = VT_INT | (vtop->type.t & VT_UNSIGNED);
        low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        low32.r = 0;
        int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
        tcc_state->ir->prevent_coalescing = 1;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &low32);
        tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
        vtop->type.t = low32.type.t;
        vtop->vr = low32.vr;
        vtop->r = 0;
      }
      else
      {
        lexpand();
        vpop();
      }
    }
    ss = 4;

#elif PTR_SIZE == 8
    if (ds == 8)
    {
      /* need to convert from 32bit to 64bit */
      if (sbt & VT_UNSIGNED)
      {
#if defined(TCC_TARGET_RISCV64)
        /* RISC-V keeps 32bit vals in registers sign-extended.
           So here we need a zero-extension.  */
        trunc = 32;
#else
        goto done;
#endif
      }
      else
      {
        gen_cvt_sxtw();
        goto done;
      }
      ss = ds, ds = 4, dbt = sbt;
    }
    else if (ss == 8)
    {
      /* RISC-V keeps 32bit vals in registers sign-extended.
         So here we need a sign-extension for signed types and
         zero-extension. for unsigned types. */
#if !defined(TCC_TARGET_RISCV64)
      trunc = 32; /* zero upper 32 bits for non RISC-V targets */
#endif
    }
    else
    {
      ss = 4;
    }
#endif

    if (ds >= ss)
      goto done;
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM64
    if (ss == 4)
    {
      gen_cvt_csti(dbt);
      goto done;
    }
#endif
    bits = (ss - ds) * 8;
    /* for unsigned, gen_op will convert SAR to SHR */
    vtop->type.t = (ss == 8 ? VT_LLONG : VT_INT) | (dbt & VT_UNSIGNED);
    vpushi(bits);
    gen_op(TOK_SHL);
    vpushi(bits - trunc);
    gen_op(TOK_SAR);
    vpushi(trunc);
    gen_op(TOK_SHR);
  }
done:
  vtop->type = *type;
  vtop->type.t &= ~(VT_CONSTANT | VT_VOLATILE | VT_ARRAY);
}

/* return type size as known at compile time. Put alignment at 'a' */
ST_FUNC int type_size(const CType *type, int *a)
{
  Sym *s;
  int bt;

  bt = type->t & VT_BTYPE;

  /* DONE: Phase 1 - Handle complex types in type_size() */
  if (type->t & VT_COMPLEX)
  {
    if (bt == VT_FLOAT)
    {
      *a = 4;   /* Alignment of float */
      return 8; /* 2 x 4 bytes */
    }
    else if (bt == VT_DOUBLE || bt == VT_LDOUBLE)
    {
      *a = 8;    /* Alignment of double */
      return 16; /* 2 x 8 bytes */
    }
  }

  if (bt == VT_STRUCT)
  {
    /* struct/union */
    s = type->ref;
    *a = s->r;
    return s->c;
  }
  else if (bt == VT_PTR)
  {
    if (type->t & VT_ARRAY)
    {
      int ts;
      s = type->ref;
      ts = type_size(&s->type, a);
      if (ts < 0 && s->c < 0)
        ts = -ts;
      return ts * s->c;
    }
    else
    {
      *a = PTR_SIZE;
      return PTR_SIZE;
    }
  }
  else if (IS_ENUM(type->t) && type->ref->c < 0)
  {
    *a = 0;
    return -1; /* incomplete enum */
  }
  else if (bt == VT_LDOUBLE)
  {
    *a = LDOUBLE_ALIGN;
    return LDOUBLE_SIZE;
  }
  else if (bt == VT_DOUBLE || bt == VT_LLONG)
  {
#if (defined TCC_TARGET_I386 && !defined TCC_TARGET_PE) || (defined TCC_TARGET_ARM && !defined TCC_ARM_EABI)
    *a = 4;
#else
    *a = 8;
#endif
    return 8;
  }
  else if (bt == VT_INT || bt == VT_FLOAT)
  {
    *a = 4;
    return 4;
  }
  else if (bt == VT_SHORT)
  {
    *a = 2;
    return 2;
  }
  else if (bt == VT_QLONG || bt == VT_QFLOAT)
  {
    *a = 8;
    return 16;
  }
  else
  {
    /* char, void, function, _Bool */
    *a = 1;
    return 1;
  }
  /* unreachable - all branches above return, but TCC's flow analysis
     needs an explicit return to avoid 'function might return no value' */
  return 0;
}

/* Return 1 if a struct/union type has any VLA (variable-length array)
   member field that requires dynamic stack allocation. */
static int struct_has_vla_member(const CType *type)
{
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT)
    return 0;
  for (f = type->ref->next; f; f = f->next)
    if (f->type.t & VT_VLA)
      return 1;
  return 0;
}

/* push type size as known at runtime time on top of value stack. Put
   alignment at 'a' */
static void vpush_type_size(CType *type, int *a)
{
  if (type->t & VT_VLA)
  {
    type_size(&type->ref->type, a);
    vset(&int_type, VT_LOCAL | VT_LVAL, type->ref->c);
  }
  else
  {
    int size = type_size(type, a);
    if (size < 0)
      tcc_error("unknown type size");
    vpushs(size);
  }
}

/* return the pointed type of t */
static inline CType *pointed_type(CType *type)
{
  return &type->ref->type;
}

/* modify type so that its it is a pointer to type. */
ST_FUNC void mk_pointer(CType *type)
{
  Sym *s;
  s = sym_push(SYM_FIELD, type, 0, -1);
  type->t = VT_PTR | (type->t & VT_STORAGE);
  type->ref = s;
}

/* return true if type1 and type2 are exactly the same (including
   qualifiers).
*/
static int is_compatible_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 0);
}

/* return true if type1 and type2 are the same (ignoring qualifiers).
 */
static int is_compatible_unqualified_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 1);
}

static void cast_error(CType *st, CType *dt)
{
  type_incompatibility_error(st, dt, "cannot convert '%s' to '%s'");
}

/* verify type compatibility to store vtop in 'dt' type */
static void verify_assign_cast(CType *dt)
{
  CType *st, *type1, *type2;
  int dbt, sbt, qualwarn, lvl;

  st = &vtop->type; /* source type */
  dbt = dt->t & VT_BTYPE;
  sbt = st->t & VT_BTYPE;
  if (dt->t & VT_CONSTANT)
    tcc_warning("assignment of read-only location");
  switch (dbt)
  {
  case VT_VOID:
    if (sbt != dbt)
      tcc_error("assignment to void expression");
    break;
  case VT_PTR:
    /* special cases for pointers */
    /* '0' can also be a pointer */
    if (is_null_pointer(vtop))
      break;
    /* accept implicit pointer to integer cast with warning */
    if (is_integer_btype(sbt))
    {
      tcc_warning("assignment makes pointer from integer without a cast");
      break;
    }
    type1 = pointed_type(dt);
    if (sbt == VT_PTR)
      type2 = pointed_type(st);
    else if (sbt == VT_FUNC)
      type2 = st; /* a function is implicitly a function pointer */
    else
      goto error;
    if (is_compatible_types(type1, type2))
      break;
    for (qualwarn = lvl = 0;; ++lvl)
    {
      if (((type2->t & VT_CONSTANT) && !(type1->t & VT_CONSTANT)) ||
          ((type2->t & VT_VOLATILE) && !(type1->t & VT_VOLATILE)))
        qualwarn = 1;
      dbt = type1->t & (VT_BTYPE | VT_LONG);
      sbt = type2->t & (VT_BTYPE | VT_LONG);
      if (dbt != VT_PTR || sbt != VT_PTR)
        break;
      type1 = pointed_type(type1);
      type2 = pointed_type(type2);
    }
    if (!is_compatible_unqualified_types(type1, type2))
    {
      if ((dbt == VT_VOID || sbt == VT_VOID) && lvl == 0)
      {
        /* void * can match anything */
      }
      else if (dbt == sbt && is_integer_btype(sbt & VT_BTYPE) &&
               IS_ENUM(type1->t) + IS_ENUM(type2->t) + !!((type1->t ^ type2->t) & VT_UNSIGNED) < 2)
      {
        /* Like GCC don't warn by default for merely changes
           in pointer target signedness.  Do warn for different
           base types, though, in particular for unsigned enums
           and signed int targets.  */
      }
      else
      {
        tcc_warning("assignment from incompatible pointer type");
        break;
      }
    }
    if (qualwarn)
      tcc_warning_c(warn_discarded_qualifiers)("assignment discards qualifiers from pointer target type");
    break;
  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
    if (sbt == VT_PTR || sbt == VT_FUNC)
    {
      tcc_warning("assignment makes integer from pointer without a cast");
    }
    else if (sbt == VT_STRUCT)
    {
      goto case_VT_STRUCT;
    }
    /* XXX: more tests */
    break;
  case VT_STRUCT:
  case_VT_STRUCT:
    if (!is_compatible_unqualified_types(dt, st))
    {
    error:
      cast_error(st, dt);
    }
    break;
  }
}

static void gen_assign_cast(CType *dt)
{
  verify_assign_cast(dt);
  gen_cast(dt);
}

/* store vtop in lvalue pushed on stack */
ST_FUNC void vstore(void)
{
  int sbt, dbt, ft, r, size, align, bit_size, bit_pos, delayed_cast;

  ft = vtop[-1].type.t;
  sbt = vtop->type.t & VT_BTYPE;
  dbt = ft & VT_BTYPE;

  verify_assign_cast(&vtop[-1].type);

  if (sbt == VT_STRUCT)
  {
    /* if structure, only generate pointer */
    /* structure assignment : generate memcpy */
    size = type_size(&vtop->type, &align);
    /* destination, keep on stack() as result */
    vpushv(vtop - 1);
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound(); /* check would be wrong after gaddrof() */
#endif
    vtop->type.t = VT_PTR;
    gaddrof();
    /* source */
    vswap();
#ifdef CONFIG_TCC_BCHECK
    if (vtop->r & VT_MUSTBOUND)
      gbound();
#endif
    vtop->type.t = VT_PTR;
    gaddrof();

#ifdef TCC_TARGET_NATIVE_STRUCT_COPY
    if (1
#ifdef CONFIG_TCC_BCHECK
        && !tcc_state->do_bounds_check
#endif
    )
    {
      gen_struct_copy(size);
    }
    else
#endif
    {
      /* type size */
      vpushi(size);
      /* Use memmove, rather than memcpy, as dest and src may be same: */
#ifdef TCC_ARM_EABI
      if (!(align & 7))
        vpush_helper_func(TOK_memmove8);
      else if (!(align & 3))
        vpush_helper_func(TOK_memmove4);
      else
#endif
        vpush_helper_func(TOK_memmove);
      {
        /* Stack is now: dest_lval, dest_ptr, src_ptr, size, func
         * IR uses 0-based parameter indices. */
        SValue param_num;
        const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
        svalue_init(&param_num);
        param_num.vr = -1;

        param_num.r = VT_CONST;
        /* memmove(dest, src, size) */
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
        TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                     call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-3].r, vtop[-3].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-3], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
        TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                     call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-2].r, vtop[-2].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
        TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=memmove call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                     call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-1].r, vtop[-1].vr);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);

        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
        /* Pop func + 3 args; keep the saved destination lvalue as result */
        vtop -= 4;
      }
    }
  }
  else if (ft & VT_BITFIELD)
  {
    /* bitfield store handling */

    /* save lvalue as expression result (example: s.b = s.a = n;) */
    vdup(), vtop[-1] = vtop[-2];

    bit_pos = BIT_POS(ft);
    bit_size = BIT_SIZE(ft);
    /* remove bit field info to avoid loops */
    vtop[-1].type.t = ft & ~VT_STRUCT_MASK;

    if (dbt == VT_BOOL)
    {
      gen_cast(&vtop[-1].type);
      vtop[-1].type.t = (vtop[-1].type.t & ~VT_BTYPE) | (VT_BYTE | VT_UNSIGNED);
    }
    r = adjust_bf(vtop - 1, bit_pos, bit_size);
    if (dbt != VT_BOOL)
    {
      gen_cast(&vtop[-1].type);
      dbt = vtop[-1].type.t & VT_BTYPE;
    }
    if (r == VT_STRUCT)
    {
      store_packed_bf(bit_pos, bit_size);
    }
    else
    {
      unsigned long long mask = (1ULL << bit_size) - 1;
      if (dbt != VT_BOOL)
      {
        /* mask source */
        if (dbt == VT_LLONG)
          vpushll(mask);
        else
          vpushi((unsigned)mask);
        gen_op('&');
      }
      /* shift source */
      vpushi(bit_pos);
      gen_op(TOK_SHL);
      vswap();
      /* duplicate destination */
      vdup();
      vrott(3);
      /* load destination, mask and or with source */
      if (dbt == VT_LLONG)
        vpushll(~(mask << bit_pos));
      else
        vpushi(~((unsigned)mask << bit_pos));
      gen_op('&');
      gen_op('|');
      /* store result */
      vstore();
      /* ... and discard */
      vpop();
    }
  }
  else if (dbt == VT_VOID)
  {
    --vtop;
    print_vstack("vstore: void");
  }
  else
  {
    /* optimize char/short casts */
    delayed_cast = 0;
    if ((dbt == VT_BYTE || dbt == VT_SHORT) && is_integer_btype(sbt))
    {
      if ((vtop->r & VT_MUSTCAST) && btype_size(dbt) > btype_size(sbt))
        force_charshort_cast();
      delayed_cast = 1;
    }
    else
    {
      gen_cast(&vtop[-1].type);
    }

    // gv(RC_TYPE(dbt)); /* generate value */

    if (delayed_cast)
    {
      vtop->r |= BFVAL(VT_MUSTCAST, (sbt == VT_LLONG) + 1);
      // tcc_warning("deley cast %x -> %x", sbt, dbt);
      vtop->type.t = ft & VT_TYPE;
    }

    /* if lvalue was saved on stack, must read it */
    if ((vtop[-1].r & VT_VALMASK) == VT_LLOCAL)
    {
      if (tcc_state->ir)
      {
        /* IR mode: load the saved pointer value into a vreg, and keep the
         * destination as a dereferenced address (***DEREF***).
         */
        SValue ptr_location;
        memset(&ptr_location, 0, sizeof(ptr_location));
        ptr_location.type.t = VT_PTRDIFF_T;
        ptr_location.r = VT_LOCAL | VT_LVAL;
        ptr_location.c.i = vtop[-1].c.i;

        SValue loaded_ptr;
        memset(&loaded_ptr, 0, sizeof(loaded_ptr));
        loaded_ptr.type.t = VT_PTRDIFF_T;
        loaded_ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &ptr_location, NULL, &loaded_ptr);

        vtop[-1].r &= ~VT_VALMASK;
        vtop[-1].r |= VT_LVAL;
        vtop[-1].vr = loaded_ptr.vr;
        vtop[-1].c.i = 0;
        vtop[-1].sym = NULL;
      }
      else
      {
        if (!nocode_wanted)
          tcc_error("IR-only: VT_LLOCAL reload requires IR");
      }
    }

    r = vtop->r & VT_VALMASK;
    /* two word case handling :
       store second register at word + 4 (or +8 for x86-64)  */
    /* On 32-bit systems, doubles are 64-bit and need two-word handling like long long */
    int is_64bit_type = (PTR_SIZE == 4 && (dbt == VT_DOUBLE || dbt == VT_LDOUBLE || dbt == VT_LLONG)) ||
                        (PTR_SIZE == 8 && dbt == VT_LLONG);
    if (is_64bit_type)
    {
      /* IR generation: handle long long as a single 64-bit value, and always
       * emit IR STORE/ASSIGN instead of calling the backend store() twice.
       *
       * Calling backend store() here is unsafe in IR mode because register
       * allocation/spilling can turn the low bits (VT_VALMASK) into VT_LOCAL
       * (0x32), which is not a physical register.
       */
      if (tcc_state->ir)
      {
        int op = TCCIR_OP_STORE;

        /* Keep the original destination type for a 64-bit store. */
        vtop[-1].type.t = dbt;

        /* Match the single-word behavior: local vreg destinations use ASSIGN. */
        if ((vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr != -1)
          op = TCCIR_OP_ASSIGN;

        /* If source is an lvalue (memory reference), emit LOAD first to get
         * the value, so STORE doesn't try to store memory-to-memory.
         */
        if (vtop->r & VT_LVAL)
        {
          SValue load_dest;
          load_dest.type = vtop->type;
          load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          load_dest.r = 0;
          load_dest.c.i = 0;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);
          vtop->vr = load_dest.vr;
          vtop->r = 0;
        }

        tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
        tcc_ir_put(tcc_state->ir, op, vtop, NULL, &vtop[-1]);

        if (op == TCCIR_OP_ASSIGN)
        {
          /* Assignment expression evaluates to the assigned value. For VT_LOCAL
           * destinations with vregs, return the destination vreg (now updated)
           * so later uses see the correct value.
           *
           * Preserve VT_LOCAL | VT_LVAL for stack-resident destinations so that
           * subsequent dereferences (e.g. *++ptr) properly load the pointer
           * value from the stack slot before dereferencing it.  Without this,
           * r=0 makes the result look like a register rvalue and indir() skips
           * the necessary LOAD, generating e.g. ldrb [stack_addr] instead of
           * ldr tmp,[stack_addr]; ldrb result,[tmp].
           */
          vtop->vr = vtop[-1].vr;
          vtop->r = 0;
        }
      }
    }
    else
    {
      /* single word */
      // store(r, vtop - 1);
      int op = TCCIR_OP_STORE;
      /* Use ASSIGN only for VT_LOCAL destinations that have a valid vreg.
       * Array elements initialized via init_putv have vr=-1 and need STORE. */
      if ((vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr != -1)
      {
        op = TCCIR_OP_ASSIGN;
      }
      /* If source is an lvalue (memory reference), emit LOAD first to get the value.
       * This is required for correctness when both source and destination live
       * in memory (e.g. range initializer replication copies element[lo] into
       * element[lo+1..hi]).
       *
       * Previously we skipped VT_LOCAL lvalues, assuming the backend would
       * handle it implicitly; that loses the load and can store garbage/zero. */
      if (vtop->r & VT_LVAL)
      {
        SValue load_dest;
        load_dest.type = vtop->type;
        load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        load_dest.r = 0;
        load_dest.c.i = 0;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);
        vtop->vr = load_dest.vr;
        vtop->r = 0; /* no longer an lvalue */
      }
      /* If source is a VT_CMP (comparison result stored in flags), we need to
       * materialize it as a 0/1 value before storing. */
      tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, op, vtop, NULL, &vtop[-1]);
      if (op == TCCIR_OP_ASSIGN)
      {
        /* See comment above in the two-word case. */
        vtop->vr = vtop[-1].vr;
        vtop->r = 0;
      }
    }
    vswap();
    vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
    print_vstack("vstore: store");
  }
}

/* post defines POST/PRE add. c is the token ++ or -- */
ST_FUNC void inc(int post, int c)
{
  test_lvalue();
  vdup(); /* save lvalue */
  if (post)
  {
    gv_dup(); /* duplicate value */
    vrotb(3);
    vrotb(3);
  }
  /* add constant */
  vpushi(c - TOK_MID);
  gen_op('+');
  vstore(); /* store value */
  if (post)
    vpop(); /* if post op, return saved value */
  else if (tcc_state->ir)
  {
    /* Pre-increment/decrement: the result of vstore() is the destination vreg
     * with r=0.  If that vreg corresponds to a local variable (a stack slot),
     * later dereference via indir() will see {r=0, vr=local_vreg} and, after
     * the register allocator spills it, generate a single byte/word load
     * directly from the stack slot instead of the required two-step sequence
     * (load pointer from slot, then load through pointer).
     *
     * Fix: emit an explicit LOAD of the stored value into a fresh temp vreg.
     * This materializes the value so that subsequent indir() correctly treats
     * it as a pointer value to dereference, not a stack-slot reference. */
    SValue *sv = vtop;
    if (sv->vr >= 0 && (sv->r & VT_VALMASK) == 0)
    {
      SValue src;
      memset(&src, 0, sizeof(src));
      src.type = sv->type;
      src.r = VT_LOCAL | VT_LVAL;
      src.vr = sv->vr;
      src.c.i = sv->c.i;

      SValue load_dest;
      memset(&load_dest, 0, sizeof(load_dest));
      load_dest.type = sv->type;
      load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &src, NULL, &load_dest);

      sv->vr = load_dest.vr;
      sv->r = 0;
    }
  }
}

ST_FUNC CString *parse_mult_str(const char *msg)
{
  /* read the string */
  if (tok != TOK_STR)
    expect(msg);
  cstr_reset(&initstr);
  while (tok == TOK_STR)
  {
    /* XXX: add \0 handling too ? */
    cstr_cat(&initstr, tokc.str.data, -1);
    next();
  }
  cstr_ccat(&initstr, '\0');
  return &initstr;
}

/* If I is >= 1 and a power of two, returns log2(i)+1.
   If I is 0 returns 0.  */
ST_FUNC int exact_log2p1(int i)
{
  int ret;
  if (!i)
    return 0;
  for (ret = 1; i >= 1 << 8; ret += 8)
    i >>= 8;
  if (i >= 1 << 4)
    ret += 4, i >>= 4;
  if (i >= 1 << 2)
    ret += 2, i >>= 2;
  if (i >= 1 << 1)
    ret++;
  return ret;
}

/* Parse __attribute__((...)) GNUC extension. */
static void parse_attribute(AttributeDef *ad)
{
  int t, n;
  char *astr;

redo:
  if (tok != TOK_ATTRIBUTE1 && tok != TOK_ATTRIBUTE2)
    return;
  next();
  skip('(');
  skip('(');
  while (tok != ')')
  {
    if (tok < TOK_IDENT)
      expect("attribute name");
    t = tok;
    next();
    switch (t)
    {
    case TOK_CLEANUP1:
    case TOK_CLEANUP2:
    {
      Sym *s;

      skip('(');
      s = sym_find(tok);
      if (!s)
      {
        tcc_warning_c(warn_implicit_function_declaration)("implicit declaration of function '%s'",
                                                          get_tok_str(tok, &tokc));
        s = external_global_sym(tok, &func_old_type);
      }
      else if ((s->type.t & VT_BTYPE) != VT_FUNC)
        tcc_error("'%s' is not declared as function", get_tok_str(tok, &tokc));
      ad->cleanup_func = s;
      next();
      skip(')');
      break;
    }
    case TOK_CONSTRUCTOR1:
    case TOK_CONSTRUCTOR2:
      ad->f.func_ctor = 1;
      break;
    case TOK_DESTRUCTOR1:
    case TOK_DESTRUCTOR2:
      ad->f.func_dtor = 1;
      break;
    case TOK_ALWAYS_INLINE1:
    case TOK_ALWAYS_INLINE2:
      ad->f.func_alwinl = 1;
      break;
    case TOK_SECTION1:
    case TOK_SECTION2:
      skip('(');
      astr = parse_mult_str("section name")->data;
      ad->section = find_section(tcc_state, astr);
      skip(')');
      break;
    case TOK_ALIAS1:
    case TOK_ALIAS2:
      skip('(');
      astr = parse_mult_str("alias(\"target\")")->data;
      /* save string as token, for later */
      ad->alias_target = tok_alloc_const(astr);
      skip(')');
      break;
    case TOK_VISIBILITY1:
    case TOK_VISIBILITY2:
      skip('(');
      astr = parse_mult_str("visibility(\"default|hidden|internal|protected\")")->data;
      if (!strcmp(astr, "default"))
        ad->a.visibility = STV_DEFAULT;
      else if (!strcmp(astr, "hidden"))
        ad->a.visibility = STV_HIDDEN;
      else if (!strcmp(astr, "internal"))
        ad->a.visibility = STV_INTERNAL;
      else if (!strcmp(astr, "protected"))
        ad->a.visibility = STV_PROTECTED;
      else
        expect("visibility(\"default|hidden|internal|protected\")");
      skip(')');
      break;
    case TOK_ALIGNED1:
    case TOK_ALIGNED2:
      if (tok == '(')
      {
        next();
        n = expr_const();
        if (n <= 0 || (n & (n - 1)) != 0)
          tcc_error("alignment must be a positive power of two");
        skip(')');
      }
      else
      {
        n = MAX_ALIGN;
      }
      ad->a.aligned = exact_log2p1(n);
      if (n != 1 << (ad->a.aligned - 1))
        tcc_error("alignment of %d is larger than implemented", n);
      break;
    case TOK_PACKED1:
    case TOK_PACKED2:
      ad->a.packed = 1;
      break;
    case TOK_WEAK1:
    case TOK_WEAK2:
      ad->a.weak = 1;
      break;
    case TOK_NAKED1:
      ad->a.naked = 1;
      break;
    case TOK_NODEBUG1:
    case TOK_NODEBUG2:
      ad->a.nodebug = 1;
      break;
    case TOK_UNUSED1:
    case TOK_UNUSED2:
      /* currently, no need to handle it because tcc does not
         track unused objects */
      break;
    case TOK_NORETURN1:
    case TOK_NORETURN2:
      ad->f.func_noreturn = 1;
      break;
    case TOK_PURE1:
    case TOK_PURE2:
      ad->f.func_pure = 1;
      break;
    case TOK_CONST2:
    case TOK_CONST3:
      ad->f.func_const = 1;
      break;
    case TOK_CDECL1:
    case TOK_CDECL2:
    case TOK_CDECL3:
      ad->f.func_call = FUNC_CDECL;
      break;
    case TOK_STDCALL1:
    case TOK_STDCALL2:
    case TOK_STDCALL3:
      ad->f.func_call = FUNC_STDCALL;
      break;
#ifdef TCC_TARGET_I386
    case TOK_REGPARM1:
    case TOK_REGPARM2:
      skip('(');
      n = expr_const();
      if (n > 3)
        n = 3;
      else if (n < 0)
        n = 0;
      if (n > 0)
        ad->f.func_call = FUNC_FASTCALL1 + n - 1;
      skip(')');
      break;
    case TOK_FASTCALL1:
    case TOK_FASTCALL2:
    case TOK_FASTCALL3:
      ad->f.func_call = FUNC_FASTCALLW;
      break;
    case TOK_THISCALL1:
    case TOK_THISCALL2:
    case TOK_THISCALL3:
      ad->f.func_call = FUNC_THISCALL;
      break;
#endif
    case TOK_MODE:
      skip('(');
      switch (tok)
      {
      case TOK_MODE_DI:
        ad->attr_mode = VT_LLONG + 1;
        break;
      case TOK_MODE_QI:
        ad->attr_mode = VT_BYTE + 1;
        break;
      case TOK_MODE_HI:
        ad->attr_mode = VT_SHORT + 1;
        break;
      case TOK_MODE_SI:
      case TOK_MODE_word:
        ad->attr_mode = VT_INT + 1;
        break;
      default:
        tcc_warning("__mode__(%s) not supported\n", get_tok_str(tok, NULL));
        break;
      }
      next();
      skip(')');
      break;
    case TOK_DLLEXPORT:
      ad->a.dllexport = 1;
      break;
    case TOK_NODECORATE:
      ad->a.nodecorate = 1;
      break;
    case TOK_DLLIMPORT:
      ad->a.dllimport = 1;
      break;
    default:
      tcc_warning_c(warn_unsupported)("'%s' attribute ignored", get_tok_str(t, NULL));
      /* skip parameters */
      if (tok == '(')
      {
        int parenthesis = 0;
        do
        {
          if (tok == '(')
            parenthesis++;
          else if (tok == ')')
            parenthesis--;
          next();
        } while (parenthesis && tok != -1);
      }
      break;
    }
    if (tok != ',')
      break;
    next();
  }
  skip(')');
  skip(')');
  goto redo;
}

static Sym *find_field(CType *type, int v, int *cumofs)
{
  Sym *s = type->ref;
  int v1 = v | SYM_FIELD;
  if (!(v & SYM_FIELD))
  { /* top-level call */
    if ((type->t & VT_BTYPE) != VT_STRUCT)
      expect("struct or union");
    if (v < TOK_UIDENT)
      expect("field name");
    if (s->c < 0)
      tcc_error("dereferencing incomplete type '%s'", get_tok_str(s->v & ~SYM_STRUCT, 0));
  }
  while ((s = s->next) != NULL)
  {
    if (s->v == v1)
    {
      *cumofs = s->c;
      return s;
    }
    if ((s->type.t & VT_BTYPE) == VT_STRUCT && s->v >= (SYM_FIRST_ANOM | SYM_FIELD))
    {
      /* try to find field in anonymous sub-struct/union */
      Sym *ret = find_field(&s->type, v1, cumofs);
      if (ret)
      {
        *cumofs += s->c;
        return ret;
      }
    }
  }
  if (!(v & SYM_FIELD))
    tcc_error("field not found: %s", get_tok_str(v, NULL));
  return s;
}

static void check_fields(CType *type, int check)
{
  Sym *s = type->ref;

  while ((s = s->next) != NULL)
  {
    int v = s->v & ~SYM_FIELD;
    if (v < SYM_FIRST_ANOM)
    {
      TokenSym *ts = table_ident[v - TOK_IDENT];
      if (check && (ts->tok & SYM_FIELD))
        tcc_error("duplicate member '%s'", get_tok_str(v, NULL));
      ts->tok ^= SYM_FIELD;
    }
    else if ((s->type.t & VT_BTYPE) == VT_STRUCT)
      check_fields(&s->type, check);
  }
}

static void struct_layout(CType *type, AttributeDef *ad)
{
  int size, align, maxalign, offset, c, bit_pos, bit_size;
  int packed, a, bt, prevbt, prev_bit_size;
  int pcc = !tcc_state->ms_bitfields;
  int pragma_pack = *tcc_state->pack_stack_ptr;
  Sym *f;

  maxalign = 1;
  offset = 0;
  c = 0;
  bit_pos = 0;
  prevbt = VT_STRUCT; /* make it never match */
  prev_bit_size = 0;

  // #define BF_DEBUG

  for (f = type->ref->next; f; f = f->next)
  {
    if (f->type.t & VT_BITFIELD)
      bit_size = BIT_SIZE(f->type.t);
    else
      bit_size = -1;
    size = type_size(&f->type, &align);
    a = f->a.aligned ? 1 << (f->a.aligned - 1) : 0;
    packed = 0;

    if (pcc && bit_size == 0)
    {
      /* in pcc mode, packing does not affect zero-width bitfields */
    }
    else
    {
      /* in pcc mode, attribute packed overrides if set. */
      if (pcc && (f->a.packed || ad->a.packed))
        align = packed = 1;

      /* pragma pack overrides align if lesser and packs bitfields always */
      if (pragma_pack)
      {
        packed = 1;
        if (pragma_pack < align)
          align = pragma_pack;
        /* in pcc mode pragma pack also overrides individual align */
        if (pcc && pragma_pack < a)
          a = 0;
      }
    }
    /* some individual align was specified */
    if (a)
      align = a;

    if (type->ref->type.t == VT_UNION)
    {
      if (pcc && bit_size >= 0)
        size = (bit_size + 7) >> 3;
      offset = 0;
      if (size > c)
        c = size;
    }
    else if (bit_size < 0)
    {
      if (pcc)
        c += (bit_pos + 7) >> 3;
      c = (c + align - 1) & -align;
      offset = c;
      if (size > 0)
        c += size;
      bit_pos = 0;
      prevbt = VT_STRUCT;
      prev_bit_size = 0;
    }
    else
    {
      /* A bit-field.  Layout is more complicated.  There are two
         options: PCC (GCC) compatible and MS compatible */
      if (pcc)
      {
        /* In PCC layout a bit-field is placed adjacent to the
           preceding bit-fields, except if:
           - it has zero-width
           - an individual alignment was given
           - it would overflow its base type container and
             there is no packing */
        if (bit_size == 0)
        {
        new_field:
          c = (c + ((bit_pos + 7) >> 3) + align - 1) & -align;
          bit_pos = 0;
        }
        else if (f->a.aligned)
        {
          goto new_field;
        }
        else if (!packed)
        {
          int a8 = align * 8;
          int ofs = ((c * 8 + bit_pos) % a8 + bit_size + a8 - 1) / a8;
          if (ofs > size / align)
            goto new_field;
        }

        /* in pcc mode, long long bitfields have type int if they fit */
        if (size == 8 && bit_size <= 32)
          f->type.t = (f->type.t & ~VT_BTYPE) | VT_INT, size = 4;

        while (bit_pos >= align * 8)
          c += align, bit_pos -= align * 8;
        offset = c;

        /* In PCC layout named bit-fields influence the alignment
           of the containing struct using the base types alignment,
           except for packed fields (which here have correct align).  */
        if (f->v & SYM_FIRST_ANOM
            // && bit_size // ??? gcc on ARM/rpi does that
        )
          align = 1;
      }
      else
      {
        bt = f->type.t & VT_BTYPE;
        if ((bit_pos + bit_size > size * 8) || (bit_size > 0) == (bt != prevbt))
        {
          c = (c + align - 1) & -align;
          offset = c;
          bit_pos = 0;
          /* In MS bitfield mode a bit-field run always uses
             at least as many bits as the underlying type.
             To start a new run it's also required that this
             or the last bit-field had non-zero width.  */
          if (bit_size || prev_bit_size)
            c += size;
        }
        /* In MS layout the records alignment is normally
           influenced by the field, except for a zero-width
           field at the start of a run (but by further zero-width
           fields it is again).  */
        if (bit_size == 0 && prevbt != bt)
          align = 1;
        prevbt = bt;
        prev_bit_size = bit_size;
      }

      f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
      bit_pos += bit_size;
    }
    if (align > maxalign)
      maxalign = align;

#ifdef BF_DEBUG
    printf("set field %s offset %-2d size %-2d align %-2d", get_tok_str(f->v & ~SYM_FIELD, NULL), offset, size, align);
    if (f->type.t & VT_BITFIELD)
    {
      printf(" pos %-2d bits %-2d", BIT_POS(f->type.t), BIT_SIZE(f->type.t));
    }
    printf("\n");
#endif

    f->c = offset;
    f->r = 0;
  }

  if (pcc)
    c += (bit_pos + 7) >> 3;

  /* store size and alignment */
  a = bt = ad->a.aligned ? 1 << (ad->a.aligned - 1) : 1;
  if (a < maxalign)
    a = maxalign;
  type->ref->r = a;
  if (pragma_pack && pragma_pack < maxalign && 0 == pcc)
  {
    /* can happen if individual align for some member was given.  In
       this case MSVC ignores maxalign when aligning the size */
    a = pragma_pack;
    if (a < bt)
      a = bt;
  }
  c = (c + a - 1) & -a;
  type->ref->c = c;

#ifdef BF_DEBUG
  printf("struct size %-2d align %-2d\n\n", c, a), fflush(stdout);
#endif

  /* check whether we can access bitfields by their type */
  for (f = type->ref->next; f; f = f->next)
  {
    int s, px, cx, c0;
    CType t;

    if (0 == (f->type.t & VT_BITFIELD))
      continue;
    f->type.ref = f;
    f->auxtype = -1;
    bit_size = BIT_SIZE(f->type.t);
    if (bit_size == 0)
      continue;
    bit_pos = BIT_POS(f->type.t);
    size = type_size(&f->type, &align);

    if (bit_pos + bit_size <= size * 8 && f->c + size <= c
#ifdef TCC_TARGET_ARM
        && !(f->c & (align - 1))
#endif
    )
      continue;

    /* try to access the field using a different type */
    c0 = -1, s = align = 1;
    t.t = VT_BYTE;
    for (;;)
    {
      px = f->c * 8 + bit_pos;
      cx = (px >> 3) & -align;
      px = px - (cx << 3);
      if (c0 == cx)
        break;
      s = (px + bit_size + 7) >> 3;
      if (s > 4)
      {
        t.t = VT_LLONG;
      }
      else if (s > 2)
      {
        t.t = VT_INT;
      }
      else if (s > 1)
      {
        t.t = VT_SHORT;
      }
      else
      {
        t.t = VT_BYTE;
      }
      s = type_size(&t, &align);
      c0 = cx;
    }

    if (px + bit_size <= s * 8 && cx + s <= c
#ifdef TCC_TARGET_ARM
        && !(cx & (align - 1))
#endif
    )
    {
      /* update offset and bit position */
      f->c = cx;
      bit_pos = px;
      f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
      if (s != size)
        f->auxtype = t.t;
#ifdef BF_DEBUG
      printf("FIX field %s offset %-2d size %-2d align %-2d "
             "pos %-2d bits %-2d\n",
             get_tok_str(f->v & ~SYM_FIELD, NULL), cx, s, align, px, bit_size);
#endif
    }
    else
    {
      /* fall back to load/store single-byte wise */
      f->auxtype = VT_STRUCT;
#ifdef BF_DEBUG
      printf("FIX field %s : load byte-wise\n", get_tok_str(f->v & ~SYM_FIELD, NULL));
#endif
    }
  }
}

/* enum/struct/union declaration. u is VT_ENUM/VT_STRUCT/VT_UNION */
static void struct_decl(CType *type, int u)
{
  int v, c, size, align, flexible;
  int bit_size, bsize, bt, ut;
  Sym *s, *ss, **ps;
  AttributeDef ad, ad1;
  CType type1, btype;

  memset(&ad, 0, sizeof ad);
  next();
  parse_attribute(&ad);

  v = 0;
  if (tok >= TOK_IDENT) /* struct/enum tag */
    v = tok, next();

  bt = ut = 0;
  if (u == VT_ENUM)
  {
    ut = VT_INT;
    if (tok == ':')
    { /* C2x enum : <type> ... */
      next();
      if (!parse_btype(&btype, &ad1, 0) || !is_integer_btype(btype.t & VT_BTYPE))
        expect("enum type");
      bt = ut = btype.t & (VT_BTYPE | VT_LONG | VT_UNSIGNED | VT_DEFSIGN);
    }
  }

  if (v)
  {
    /* struct already defined ? return it */
    s = struct_find(v);
    if (s && (s->sym_scope == local_scope || (tok != '{' && tok != ';')))
    {
      if (u == s->type.t)
        goto do_decl;
      if (u == VT_ENUM && IS_ENUM(s->type.t)) /* XXX: check integral types */
        goto do_decl;
      tcc_error("redeclaration of '%s'", get_tok_str(v, NULL));
    }
  }
  else
  {
    if (tok != '{')
      expect("struct/union/enum name");
    v = anon_sym++;
  }
  /* Record the original enum/struct/union token.  */
  type1.t = u | ut;
  type1.ref = NULL;
  /* we put an undefined size for struct/union */
  s = sym_push(v | SYM_STRUCT, &type1, 0, bt ? 0 : -1);
  s->r = 0; /* default alignment is zero as gcc */
do_decl:
  type->t = s->type.t;
  type->ref = s;

  if (tok == '{')
  {
    next();
    if (s->c != -1 && !(u == VT_ENUM && s->c == 0)) /* not yet defined typed enum */
      tcc_error("struct/union/enum already defined");
    s->c = -2;
    /* cannot be empty */
    /* non empty enums are not allowed */
    ps = &s->next;
    if (u == VT_ENUM)
    {
      long long ll = 0, pl = 0, nl = 0;
      CType t;
      t.ref = s;
      /* enum symbols have static storage */
      t.t = VT_INT | VT_STATIC | VT_ENUM_VAL;
      if (bt)
        t.t = bt | VT_STATIC | VT_ENUM_VAL;
      for (;;)
      {
        v = tok;
        if (v < TOK_UIDENT)
          expect("identifier");
        ss = sym_find(v);
        if (ss && !local_stack)
          tcc_error("redefinition of enumerator '%s'", get_tok_str(v, NULL));
        next();
        if (tok == '=')
        {
          next();
          ll = expr_const64();
        }
        ss = sym_push(v, &t, VT_CONST, 0);
        ss->enum_val = ll;
        *ps = ss, ps = &ss->next;
        if (ll < nl)
          nl = ll;
        if (ll > pl)
          pl = ll;
        if (tok != ',')
          break;
        next();
        ll++;
        /* NOTE: we accept a trailing comma */
        if (tok == '}')
          break;
      }
      skip('}');

      if (bt)
      {
        t.t = bt;
        s->c = 2;
        goto enum_done;
      }

      /* set integral type of the enum */
      t.t = VT_INT;
      if (nl >= 0)
      {
        if (pl != (unsigned)pl)
          t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
        t.t |= VT_UNSIGNED;
      }
      else if (pl != (int)pl || nl != (int)nl)
        t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);

      /* set type for enum members */
      for (ss = s->next; ss; ss = ss->next)
      {
        ll = ss->enum_val;
        if (ll == (int)ll) /* default is int if it fits */
          continue;
        if (t.t & VT_UNSIGNED)
        {
          ss->type.t |= VT_UNSIGNED;
          if (ll == (unsigned)ll)
            continue;
        }
        ss->type.t = (ss->type.t & ~VT_BTYPE) | (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
      }
      s->c = 1;
    enum_done:
      s->type.t = type->t = t.t | VT_ENUM;
    }
    else
    {
      c = 0;
      flexible = 0;
      while (tok != '}')
      {
        if (!parse_btype(&btype, &ad1, 0))
        {
          if (tok == TOK_STATIC_ASSERT)
          {
            do_Static_assert();
            continue;
          }
          skip(';');
          continue;
        }
        while (1)
        {
          if (flexible)
            tcc_error("flexible array member '%s' not at the end of struct", get_tok_str(v, NULL));
          bit_size = -1;
          v = 0;
          type1 = btype;
          if (tok != ':')
          {
            if (tok != ';')
              type_decl(&type1, &ad1, &v, TYPE_DIRECT);
            if (v == 0)
            {
              if ((type1.t & VT_BTYPE) != VT_STRUCT)
                expect("identifier");
              else
              {
                int v = btype.ref->v;
                if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
                {
                  if (tcc_state->ms_extensions == 0)
                    expect("identifier");
                }
              }
            }
            if (type_size(&type1, &align) < 0)
            {
              if ((u == VT_STRUCT) && (type1.t & VT_ARRAY) && c)
                flexible = 1;
              else
                tcc_error("field '%s' has incomplete type", get_tok_str(v, NULL));
            }
            if ((type1.t & VT_BTYPE) == VT_FUNC || (type1.t & VT_BTYPE) == VT_VOID || (type1.t & VT_STORAGE))
              tcc_error("invalid type for '%s'", get_tok_str(v, NULL));
          }
          if (tok == ':')
          {
            next();
            bit_size = expr_const();
            /* XXX: handle v = 0 case for messages */
            if (bit_size < 0)
              tcc_error("negative width in bit-field '%s'", get_tok_str(v, NULL));
            if (v && bit_size == 0)
              tcc_error("zero width for bit-field '%s'", get_tok_str(v, NULL));
            parse_attribute(&ad1);
          }
          size = type_size(&type1, &align);
          if (bit_size >= 0)
          {
            bt = type1.t & VT_BTYPE;
            if (bt != VT_INT && bt != VT_BYTE && bt != VT_SHORT && bt != VT_BOOL && bt != VT_LLONG)
              tcc_error("bitfields must have scalar type");
            bsize = size * 8;
            if (bit_size > bsize)
            {
              tcc_error("width of '%s' exceeds its type", get_tok_str(v, NULL));
            }
            else if (bit_size == bsize && !ad.a.packed && !ad1.a.packed)
            {
              /* no need for bit fields */
              ;
            }
            else if (bit_size == 64)
            {
              tcc_error("field width 64 not implemented");
            }
            else
            {
              type1.t = (type1.t & ~VT_STRUCT_MASK) | VT_BITFIELD | ((unsigned)bit_size << (VT_STRUCT_SHIFT + 6));
            }
          }
          if (v != 0 || (type1.t & VT_BTYPE) == VT_STRUCT)
          {
            /* Remember we've seen a real field to check
               for placement of flexible array member. */
            c = 1;
          }
          /* If member is a struct or bit-field, enforce
             placing into the struct (as anonymous).  */
          if (v == 0 && ((type1.t & VT_BTYPE) == VT_STRUCT || bit_size >= 0))
          {
            v = anon_sym++;
          }
          if (v)
          {
            ss = sym_push(v | SYM_FIELD, &type1, 0, 0);
            ss->a = ad1.a;
            *ps = ss;
            ps = &ss->next;
          }
          if (tok == ';' || tok == TOK_EOF)
            break;
          skip(',');
        }
        skip(';');
      }
      skip('}');
      parse_attribute(&ad);
      if (ad.cleanup_func)
      {
        tcc_warning("attribute '__cleanup__' ignored on type");
      }
      check_fields(type, 1);
      check_fields(type, 0);
      struct_layout(type, &ad);
      if (debug_modes)
        tcc_debug_fix_anon(tcc_state, type);
    }
  }
}

static void sym_to_attr(AttributeDef *ad, Sym *s)
{
  merge_symattr(&ad->a, &s->a);
  merge_funcattr(&ad->f, &s->f);
}

/* Add type qualifiers to a type. If the type is an array then the qualifiers
   are added to the element type, copied because it could be a typedef. */
static void parse_btype_qualify(CType *type, int qualifiers)
{
  while (type->t & VT_ARRAY)
  {
    type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
    type = &type->ref->type;
  }
  type->t |= qualifiers;
}

/* return 0 if no type declaration. otherwise, return the basic type
   and skip it.
 */
static int parse_btype(CType *type, AttributeDef *ad, int ignore_label)
{
  int t, u, bt, st, type_found, typespec_found, g, n;
  Sym *s;
  CType type1;

  memset(ad, 0, sizeof(AttributeDef));
  type_found = 0;
  typespec_found = 0;
  t = VT_INT;
  bt = st = -1;
  type->ref = NULL;

  while (1)
  {
    switch (tok)
    {
    case TOK_EXTENSION:
      /* currently, we really ignore extension */
      next();
      continue;

      /* basic types */
    case TOK_CHAR:
      u = VT_BYTE;
    basic_type:
      next();
    basic_type1:
      if (u == VT_SHORT || u == VT_LONG)
      {
        if (st != -1 || (bt != -1 && bt != VT_INT))
        tmbt:
          tcc_error("too many basic types");
        st = u;
      }
      else
      {
        if (bt != -1 || (st != -1 && u != VT_INT))
          goto tmbt;
        bt = u;
      }
      if (u != VT_INT)
        t = (t & ~(VT_BTYPE | VT_LONG)) | u;
      typespec_found = 1;
      break;
    case TOK_VOID:
      u = VT_VOID;
      goto basic_type;
    case TOK_SHORT:
      u = VT_SHORT;
      goto basic_type;
    case TOK_INT:
      u = VT_INT;
      goto basic_type;
    case TOK_ALIGNAS:
    {
      int n;
      AttributeDef ad1;
      next();
      skip('(');
      memset(&ad1, 0, sizeof(AttributeDef));
      if (parse_btype(&type1, &ad1, 0))
      {
        type_decl(&type1, &ad1, &n, TYPE_ABSTRACT);
        if (ad1.a.aligned)
          n = 1 << (ad1.a.aligned - 1);
        else
          type_size(&type1, &n);
      }
      else
      {
        n = expr_const();
        if (n < 0 || (n & (n - 1)) != 0)
          tcc_error("alignment must be a positive power of two");
      }
      skip(')');
      ad->a.aligned = exact_log2p1(n);
    }
      continue;
    case TOK_LONG:
      if ((t & VT_BTYPE) == VT_DOUBLE)
      {
        t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LDOUBLE;
      }
      else if ((t & (VT_BTYPE | VT_LONG)) == VT_LONG)
      {
        t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LLONG;
      }
      else
      {
        u = VT_LONG;
        goto basic_type;
      }
      next();
      break;
#ifdef TCC_TARGET_ARM64
    case TOK_UINT128:
      /* GCC's __uint128_t appears in some Linux header files. Make it a
         synonym for long double to get the size and alignment right. */
      u = VT_LDOUBLE;
      goto basic_type;
#endif
    case TOK_BOOL:
      u = VT_BOOL;
      goto basic_type;
    case TOK_COMPLEX:
    case TOK_COMPLEX_GCC:
      /* DONE: Phase 1 - Mark that we saw _Complex, will combine with float/double */
      if (t & VT_COMPLEX)
        tcc_error("duplicate _Complex specifier");
      t |= VT_COMPLEX;
      typespec_found = 1;
      next();
      break;
    case TOK_FLOAT:
      u = VT_FLOAT;
      goto basic_type;
    case TOK_DOUBLE:
      if ((t & (VT_BTYPE | VT_LONG)) == VT_LONG)
      {
        t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LDOUBLE;
      }
      else
      {
        u = VT_DOUBLE;
        goto basic_type;
      }
      next();
      break;
    case TOK_ENUM:
      struct_decl(&type1, VT_ENUM);
    basic_type2:
      u = type1.t;
      type->ref = type1.ref;
      goto basic_type1;
    case TOK_STRUCT:
      struct_decl(&type1, VT_STRUCT);
      goto basic_type2;
    case TOK_UNION:
      struct_decl(&type1, VT_UNION);
      goto basic_type2;

      /* type modifiers */
    case TOK__Atomic:
      next();
      type->t = t;
      parse_btype_qualify(type, VT_ATOMIC);
      t = type->t;
      if (tok == '(')
      {
        parse_expr_type(&type1);
        /* remove all storage modifiers except typedef */
        type1.t &= ~(VT_STORAGE & ~VT_TYPEDEF);
        if (type1.ref)
          sym_to_attr(ad, type1.ref);
        goto basic_type2;
      }
      break;
    case TOK_CONST1:
    case TOK_CONST2:
    case TOK_CONST3:
      type->t = t;
      parse_btype_qualify(type, VT_CONSTANT);
      t = type->t;
      next();
      break;
    case TOK_VOLATILE1:
    case TOK_VOLATILE2:
    case TOK_VOLATILE3:
      type->t = t;
      parse_btype_qualify(type, VT_VOLATILE);
      t = type->t;
      next();
      break;
    case TOK_SIGNED1:
    case TOK_SIGNED2:
    case TOK_SIGNED3:
      if ((t & (VT_DEFSIGN | VT_UNSIGNED)) == (VT_DEFSIGN | VT_UNSIGNED))
        tcc_error("signed and unsigned modifier");
      t |= VT_DEFSIGN;
      next();
      typespec_found = 1;
      break;
    case TOK_REGISTER:
    case TOK_AUTO:
    case TOK_RESTRICT1:
    case TOK_RESTRICT2:
    case TOK_RESTRICT3:
      next();
      break;
    case TOK_UNSIGNED:
      if ((t & (VT_DEFSIGN | VT_UNSIGNED)) == VT_DEFSIGN)
        tcc_error("signed and unsigned modifier");
      t |= VT_DEFSIGN | VT_UNSIGNED;
      next();
      typespec_found = 1;
      break;

      /* storage */
    case TOK_EXTERN:
      g = VT_EXTERN;
      goto storage;
    case TOK_STATIC:
      g = VT_STATIC;
      goto storage;
    case TOK_TYPEDEF:
      g = VT_TYPEDEF;
      goto storage;
    storage:
      if (t & (VT_EXTERN | VT_STATIC | VT_TYPEDEF) & ~g)
        tcc_error("multiple storage classes");
      t |= g;
      next();
      break;
    case TOK_INLINE1:
    case TOK_INLINE2:
    case TOK_INLINE3:
      t |= VT_INLINE;
      next();
      break;
    case TOK_NORETURN3:
      next();
      ad->f.func_noreturn = 1;
      break;
      /* GNUC attribute */
    case TOK_ATTRIBUTE1:
    case TOK_ATTRIBUTE2:
      parse_attribute(ad);
      if (ad->attr_mode)
      {
        u = ad->attr_mode - 1;
        t = (t & ~(VT_BTYPE | VT_LONG)) | u;
      }
      continue;
      /* GNUC typeof */
    case TOK_TYPEOF1:
    case TOK_TYPEOF2:
    case TOK_TYPEOF3:
      next();
      parse_expr_type(&type1);
      /* remove all storage modifiers except typedef */
      type1.t &= ~(VT_STORAGE & ~VT_TYPEDEF);
      if (type1.ref)
        sym_to_attr(ad, type1.ref);
      goto basic_type2;
    case TOK_THREAD_LOCAL:
      tcc_error("_Thread_local is not implemented");
    default:
      if (typespec_found)
        goto the_end;
      s = sym_find(tok);
      if (!s || !(s->type.t & VT_TYPEDEF))
        goto the_end;

      n = tok, next();
      if (tok == ':' && ignore_label)
      {
        /* ignore if it's a label */
        unget_tok(n);
        goto the_end;
      }

      t &= ~(VT_BTYPE | VT_LONG);
      u = t & ~(VT_CONSTANT | VT_VOLATILE), t ^= u;
      type->t = (s->type.t & ~VT_TYPEDEF) | u;
      type->ref = s->type.ref;
      if (t)
        parse_btype_qualify(type, t);
      t = type->t;
      /* get attributes from typedef */
      sym_to_attr(ad, s);
      typespec_found = 1;
      st = bt = -2;
      break;
    }
    type_found = 1;
  }
the_end:
  if (tcc_state->char_is_unsigned)
  {
    if ((t & (VT_DEFSIGN | VT_BTYPE)) == VT_BYTE)
      t |= VT_UNSIGNED;
  }
  /* VT_LONG is used just as a modifier for VT_INT / VT_LLONG */
  bt = t & (VT_BTYPE | VT_LONG);
  if (bt == VT_LONG)
    t |= LONG_SIZE == 8 ? VT_LLONG : VT_INT;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
  if (bt == VT_LDOUBLE)
    t = (t & ~(VT_BTYPE | VT_LONG)) | (VT_DOUBLE | VT_LONG);
#endif
  type->t = t;
  return type_found;
}

/* convert a function parameter type (array to pointer and function to
   function pointer) */
static inline void convert_parameter_type(CType *pt)
{
  /* remove const and volatile qualifiers (XXX: const could be used
     to indicate a const function parameter */
  pt->t &= ~(VT_CONSTANT | VT_VOLATILE);
  /* array must be transformed to pointer according to ANSI C */
  pt->t &= ~(VT_ARRAY | VT_VLA);
  if ((pt->t & VT_BTYPE) == VT_FUNC)
  {
    mk_pointer(pt);
  }
}

ST_FUNC CString *parse_asm_str(void)
{
  skip('(');
  return parse_mult_str("string constant");
}

/* Parse an asm label and return the token */
static int asm_label_instr(void)
{
  int v;
  char *astr;

  next();
  astr = parse_asm_str()->data;
  skip(')');
#ifdef ASM_DEBUG
  printf("asm_alias: \"%s\"\n", astr);
#endif
  v = tok_alloc_const(astr);
  return v;
}

static int post_type(CType *type, AttributeDef *ad, int storage, int td)
{
  int n, l, t1, arg_size, align;
  Sym **plast, *s, *first;
  AttributeDef ad1;
  CType pt;
  TokenString *vla_array_tok = NULL;
  int *vla_array_str = NULL;
  int vla_array_str_on_heap = 0; /* 1 if vla_array_str is heap-allocated, 0 if inline */

  if (tok == '(')
  {
    /* function type, or recursive declarator (return if so) */
    next();
    if (TYPE_DIRECT == (td & (TYPE_DIRECT | TYPE_ABSTRACT)))
      return 0;
    if (tok == ')')
      l = 0;
    else if (parse_btype(&pt, &ad1, 0))
      l = FUNC_NEW;
    else if (td & (TYPE_DIRECT | TYPE_ABSTRACT))
    {
      merge_attr(ad, &ad1);
      return 0;
    }
    else
      l = FUNC_OLD;

    first = NULL;
    plast = &first;
    arg_size = 0;
    ++local_scope;
    if (l)
    {
      for (;;)
      {
        /* read param name and compute offset */
        if (l != FUNC_OLD)
        {
          if ((pt.t & VT_BTYPE) == VT_VOID && tok == ')')
            break;
          type_decl(&pt, &ad1, &n, TYPE_DIRECT | TYPE_ABSTRACT | TYPE_PARAM);
          if ((pt.t & VT_BTYPE) == VT_VOID)
            tcc_error("parameter declared as void");
          if (n == 0)
            n = SYM_FIELD;
        }
        else
        {
          n = tok;
          pt.t = VT_VOID; /* invalid type */
          pt.ref = NULL;
          next();
        }
        if (n < TOK_UIDENT)
          expect("identifier");
        convert_parameter_type(&pt);
        arg_size += (type_size(&pt, &align) + PTR_SIZE - 1) / PTR_SIZE;
        /* these symbols may be evaluated for VLArrays (see below, under
           nocode_wanted) which is why we push them here as normal symbols
           temporarily.  Example: int func(int a, int b[++a]); */
        s = sym_push(n, &pt, VT_LOCAL | VT_LVAL, 0);
        *plast = s;
        plast = &s->next;
        if (tok == ')')
          break;
        skip(',');
        if (l == FUNC_NEW && tok == TOK_DOTS)
        {
          l = FUNC_ELLIPSIS;
          next();
          break;
        }
        if (l == FUNC_NEW && !parse_btype(&pt, &ad1, 0))
          tcc_error("invalid type");
      }
    }
    else
      /* if no parameters, then old type prototype */
      l = FUNC_OLD;
    skip(')');
    /* remove parameter symbols from token table, keep on stack */
    if (first)
    {
      sym_pop(local_stack ? &local_stack : &global_stack, first->prev, 1);
      for (s = first; s; s = s->next)
        s->v |= SYM_FIELD;
    }
    --local_scope;
    /* NOTE: const is ignored in returned type as it has a special
       meaning in gcc / C++ */
    type->t &= ~VT_CONSTANT;
    /* some ancient pre-K&R C allows a function to return an array
       and the array brackets to be put after the arguments, such
       that "int c()[]" means something like "int[] c()" */
    if (tok == '[')
    {
      next();
      skip(']'); /* only handle simple "[]" */
      mk_pointer(type);
    }
    /* we push a anonymous symbol which will contain the function prototype */
    ad->f.func_args = arg_size;
    ad->f.func_type = l;
    s = sym_push(SYM_FIELD, type, 0, 0);
    s->a = ad->a;
    s->f = ad->f;
    s->next = first;
    type->t = VT_FUNC;
    type->ref = s;
  }
  else if (tok == '[')
  {
    int saved_nocode_wanted = nocode_wanted;
    /* array definition */
    next();
    n = -1;
    t1 = 0;
    if (td & TYPE_PARAM)
      while (1)
      {
        /* XXX The optional type-quals and static should only be accepted
           in parameter decls.  The '*' as well, and then even only
           in prototypes (not function defs).  */
        switch (tok)
        {
        case TOK_RESTRICT1:
        case TOK_RESTRICT2:
        case TOK_RESTRICT3:
        case TOK_CONST1:
        case TOK_VOLATILE1:
        case TOK_STATIC:
        case '*':
          next();
          continue;
        default:
          break;
        }
        if (tok != ']')
        {
          /* Code generation is not done now but has to be done
             at start of function. Save code here for later use. */
          nocode_wanted = 1;
          skip_or_save_block(&vla_array_tok);
          unget_tok(0);
          vla_array_str = tok_str_buf(vla_array_tok);
          vla_array_str_on_heap = vla_array_tok->allocated_len > 0;
          begin_macro(vla_array_tok, 2);
          next();
          gexpr();
          end_macro();
          next();
          goto check;
        }
        break;
      }
    else if (tok != ']')
    {
      if (!local_stack || (storage & VT_STATIC))
        vpushi(expr_const());
      else
      {
        /* VLAs (which can only happen with local_stack && !VT_STATIC)
           length must always be evaluated, even under nocode_wanted,
           so that its size slot is initialized (e.g. under sizeof
           or typeof).  */
        nocode_wanted = 0;
        gexpr();
      }
    check:
      if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
      {
        n = vtop->c.i;
        if (n < 0)
          tcc_error("invalid array size");
      }
      else
      {
        if (!is_integer_btype(vtop->type.t & VT_BTYPE))
          tcc_error("size of variable length array should be an integer");
        n = 0;
        t1 = VT_VLA;
      }
    }
    skip(']');
    /* parse next post type */
    post_type(type, ad, storage, (td & ~(TYPE_DIRECT | TYPE_ABSTRACT)) | TYPE_NEST);

    if ((type->t & VT_BTYPE) == VT_FUNC)
      tcc_error("declaration of an array of functions");
    if ((type->t & VT_BTYPE) == VT_VOID || type_size(type, &align) < 0)
      tcc_error("declaration of an array of incomplete type elements");

    t1 |= type->t & VT_VLA;

    if (t1 & VT_VLA)
    {
      if (n < 0)
      {
        if (td & TYPE_NEST)
          tcc_error("need explicit inner array size in VLAs");
      }
      else
      {
        loc -= type_size(&int_type, &align);
        loc &= -align;
        n = loc;

        vpush_type_size(type, &align);
        gen_op('*');
        vset(&int_type, VT_LOCAL | VT_LVAL, n);
        vswap();
        vstore();
      }
    }
    if (n != -1)
      vpop();
    nocode_wanted = saved_nocode_wanted;

    /* we push an anonymous symbol which will contain the array
       element type */
    s = sym_push(SYM_FIELD, type, 0, n);
    type->t = (t1 ? VT_VLA : VT_ARRAY) | VT_PTR;
    type->ref = s;

    if (vla_array_str)
    {
      /* for function args, the top dimension is converted to pointer */
      if ((t1 & VT_VLA) && (td & TYPE_NEST))
        s->vla_array_str = vla_array_str;
      else if (vla_array_str_on_heap)
        tok_str_free_str(vla_array_str);
      /* else: inline buffer, will be freed with TokenString struct */
    }
  }
  return 1;
}

/* Parse a type declarator (except basic type), and return the type
   in 'type'. 'td' is a bitmask indicating which kind of type decl is
   expected. 'type' should contain the basic type. 'ad' is the
   attribute definition of the basic type. It can be modified by
   type_decl().  If this (possibly abstract) declarator is a pointer chain
   it returns the innermost pointed to type (equals *type, but is a different
   pointer), otherwise returns type itself, that's used for recursive calls.  */
static CType *type_decl(CType *type, AttributeDef *ad, int *v, int td)
{
  CType *post, *ret;
  int qualifiers, storage;

  /* recursive type, remove storage bits first, apply them later again */
  storage = type->t & VT_STORAGE;
  type->t &= ~VT_STORAGE;
  post = ret = type;

  while (tok == '*')
  {
    qualifiers = 0;
  redo:
    next();
    switch (tok)
    {
    case TOK__Atomic:
      qualifiers |= VT_ATOMIC;
      goto redo;
    case TOK_CONST1:
    case TOK_CONST2:
    case TOK_CONST3:
      qualifiers |= VT_CONSTANT;
      goto redo;
    case TOK_VOLATILE1:
    case TOK_VOLATILE2:
    case TOK_VOLATILE3:
      qualifiers |= VT_VOLATILE;
      goto redo;
    case TOK_RESTRICT1:
    case TOK_RESTRICT2:
    case TOK_RESTRICT3:
      goto redo;
    /* XXX: clarify attribute handling */
    case TOK_ATTRIBUTE1:
    case TOK_ATTRIBUTE2:
      parse_attribute(ad);
      break;
    }
    mk_pointer(type);
    type->t |= qualifiers;
    if (ret == type)
      /* innermost pointed to type is the one for the first derivation */
      ret = pointed_type(type);
  }

  if (tok == '(')
  {
    /* This is possibly a parameter type list for abstract declarators
       ('int ()'), use post_type for testing this.  */
    if (!post_type(type, ad, 0, td))
    {
      /* It's not, so it's a nested declarator, and the post operations
         apply to the innermost pointed to type (if any).  */
      /* XXX: this is not correct to modify 'ad' at this point, but
         the syntax is not clear */
      parse_attribute(ad);
      post = type_decl(type, ad, v, td);
      skip(')');
    }
    else
      goto abstract;
  }
  else if (tok >= TOK_IDENT && (td & TYPE_DIRECT))
  {
    /* type identifier */
    *v = tok;
    next();
  }
  else
  {
  abstract:
    if (!(td & TYPE_ABSTRACT))
      expect("identifier");
    *v = 0;
  }
  post_type(post, ad, post != ret ? 0 : storage, td & ~(TYPE_DIRECT | TYPE_ABSTRACT));
  parse_attribute(ad);
  type->t |= storage;
  return ret;
}

/* indirection with full error checking and bound check */
ST_FUNC void indir(void)
{
  if ((vtop->type.t & VT_BTYPE) != VT_PTR)
  {
    if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
      return;
    expect("pointer");
  }
  if (vtop->r & VT_LVAL)
  {
    SValue dest;
    svalue_init(&dest);
    dest.type = *pointed_type(&vtop->type);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    // gv(RC_INT);
  }
  vtop->type = *pointed_type(&vtop->type);
  /* After pointer dereference, the result represents the pointed-to object,
   * not the original parameter.  Clear VT_PARAM so that a subsequent
   * gaddrof() (e.g. during c->field struct member access) does NOT emit
   * a spurious LEA of the parameter's stack slot.  Without this, code like
   * c->items[idx] (where c is a register-passed pointer parameter) would
   * compute the address of c's stack slot + field_offset instead of
   * loading c's value and adding the field offset. */
  vtop->r &= ~VT_PARAM;
  /* Arrays and functions are never lvalues */
  if (!(vtop->type.t & (VT_ARRAY | VT_VLA)) && (vtop->type.t & VT_BTYPE) != VT_FUNC)
  {
    vtop->r |= VT_LVAL;
    /* if bound checking, the referenced pointer must be checked */
#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
      vtop->r |= VT_MUSTBOUND;
#endif
  }
}

/* pass a parameter to a function and do type checking and casting */
static void gfunc_param_typed(Sym *func, Sym *arg)
{
  int func_type;
  CType type;

  func_type = func->f.func_type;
  if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL))
  {
    /* ARM EABI AAPCS: Composite types (struct/union) larger than 4 words (16
     * bytes) must be passed by invisible reference even without a prototype,
     * since the ABI convention must be followed regardless of whether the
     * prototype is visible to the caller. */
    if ((vtop->type.t & VT_BTYPE) == VT_STRUCT)
    {
      int align, size = type_size(&vtop->type, &align);
      if (size > 16)
      {
        /* Pass by invisible reference: caller must allocate a temporary copy
         * and pass a pointer to that copy (AAPCS). Passing the original
         * object's address would break C's by-value semantics. */
        if (nocode_wanted)
          return;

        if (!(vtop->r & VT_LVAL))
        {
          /* For now we require an lvalue source; most struct expressions in
           * TCC are materialized as lvalues already. */
          tcc_error("cannot pass large struct by value");
        }

        int temp_vr;
        int tmp_loc = get_temp_local_var(size, align, &temp_vr);

        /* Store the source struct into the temporary destination.
         * vstore() will emit a memmove() for struct types. */
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type = vtop->type;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.vr = temp_vr;
          dst.c.i = tmp_loc;
          vpushv(&dst);
          vswap();
          vstore();
        }

        /* Convert the temp lvalue to a pointer argument. */
        mk_pointer(&vtop->type);
        gaddrof();
        return;
      }
    }

    /* default casting : only need to convert float to double */
    if ((vtop->type.t & VT_BTYPE) == VT_FLOAT)
    {
      gen_cast_s(VT_DOUBLE);
    }
    else if (vtop->type.t & VT_BITFIELD)
    {
      type.t = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
      type.ref = vtop->type.ref;
      gen_cast(&type);
    }
    else if (vtop->r & VT_MUSTCAST)
    {
      force_charshort_cast();
    }
  }
  else if (arg == NULL)
  {
    tcc_error("too many arguments to function");
  }
  else
  {
    type = arg->type;
    type.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */

    /* ARM EABI AAPCS: Composite types (struct/union) larger than 4 words (16 bytes)
     * must be passed by invisible reference - the caller passes a pointer.
     * Check if this is a large struct that should be passed by reference. */
    if ((type.t & VT_BTYPE) == VT_STRUCT)
    {
      int align, size = type_size(&type, &align);
      if (size > 16)
      {
        /* Pass by invisible reference: caller must allocate a temporary copy
         * and pass a pointer to that copy (AAPCS). Passing the original object's
         * address would break C's by-value semantics.
         */
        if (nocode_wanted)
          return;

        if (!(vtop->r & VT_LVAL))
        {
          /* For now we require an lvalue source; most struct expressions in TCC
           * are materialized as lvalues already.
           */
          tcc_error("cannot pass large struct by value");
        }

        int temp_vr;
        int tmp_loc = get_temp_local_var(size, align, &temp_vr);

        /* Store the source struct into the temporary destination.
         * vstore() will emit a memmove() for struct types.
         */
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type = type;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.vr = temp_vr;
          dst.c.i = tmp_loc;
          vpushv(&dst);
          vswap();
          vstore();
        }

        /* Convert the temp lvalue to a pointer argument. */
        mk_pointer(&vtop->type);
        gaddrof();
        return;
      }
    }

    gen_assign_cast(&type);
  }
}

/* parse an expression and return its type without any side effect. */
static void expr_type(CType *type, void (*expr_fn)(void))
{
  nocode_wanted++;
  expr_fn();
  *type = vtop->type;
  vpop();
  nocode_wanted--;
}

/* parse an expression of the form '(type)' or '(expr)' and return its
   type */
static void parse_expr_type(CType *type)
{
  int n;
  AttributeDef ad;

  skip('(');
  if (parse_btype(type, &ad, 0))
  {
    type_decl(type, &ad, &n, TYPE_ABSTRACT);
  }
  else
  {
    expr_type(type, gexpr);
  }
  skip(')');
}

static void parse_type(CType *type)
{
  AttributeDef ad;
  int n;

  if (!parse_btype(type, &ad, 0))
  {
    expect("type");
  }
  type_decl(type, &ad, &n, TYPE_ABSTRACT);
}

static void parse_builtin_params(int nc, const char *args)
{
  char c, sep = '(';
  CType type;
  if (nc)
    nocode_wanted++;
  next();
  if (*args == 0)
    skip(sep);
  while ((c = *args++))
  {
    skip(sep);
    sep = ',';
    if (c == 't')
    {
      parse_type(&type);
      vpush(&type);
      continue;
    }
    expr_eq();
    type.ref = NULL;
    type.t = 0;
    switch (c)
    {
    case 'e':
      continue;
    case 'V':
      type.t = VT_CONSTANT;
    case 'v':
      type.t |= VT_VOID;
      mk_pointer(&type);
      break;
    case 'S':
      type.t = VT_CONSTANT;
    case 's':
      type.t |= char_type.t;
      mk_pointer(&type);
      break;
    case 'i':
      type.t = VT_INT;
      break;
    case 'l':
      type.t = VT_SIZE_T;
      break;
    default:
      break;
    }
    gen_assign_cast(&type);
  }
  skip(')');
  if (nc)
    nocode_wanted--;
}

static void parse_atomic(int atok)
{
  int size, align, arg, t, save = 0;
  CType *atom, *atom_ptr, ct = {0};
  SValue store;
  char buf[40];
  static const char *const templates[] = {/*
                                           * Each entry consists of callback and function template.
                                           * The template represents argument types and return type.
                                           *
                                           * ? void (return-only)
                                           * b bool
                                           * a atomic
                                           * A read-only atomic
                                           * p pointer to memory
                                           * v value
                                           * l load pointer
                                           * s save pointer
                                           * m memory model
                                           */

                                          /* keep in order of appearance in tcctok.h: */
                                          /* __atomic_store */ "alm.?",
                                          /* __atomic_load */ "Asm.v",
                                          /* __atomic_exchange */ "alsm.v",
                                          /* __atomic_compare_exchange */ "aplbmm.b",
                                          /* __atomic_fetch_add */ "avm.v",
                                          /* __atomic_fetch_sub */ "avm.v",
                                          /* __atomic_fetch_or */ "avm.v",
                                          /* __atomic_fetch_xor */ "avm.v",
                                          /* __atomic_fetch_and */ "avm.v",
                                          /* __atomic_fetch_nand */ "avm.v",
                                          /* __atomic_and_fetch */ "avm.v",
                                          /* __atomic_sub_fetch */ "avm.v",
                                          /* __atomic_or_fetch */ "avm.v",
                                          /* __atomic_xor_fetch */ "avm.v",
                                          /* __atomic_and_fetch */ "avm.v",
                                          /* __atomic_nand_fetch */ "avm.v"};
  const char *template = templates[(atok - TOK___atomic_store)];

  atom = atom_ptr = NULL;
  size = 0; /* pacify compiler */
  next();
  skip('(');
  for (arg = 0;;)
  {
    expr_eq();
    switch (template[arg])
    {
    case 'a':
    case 'A':
      atom_ptr = &vtop->type;
      if ((atom_ptr->t & VT_BTYPE) != VT_PTR)
        expect("pointer");
      atom = pointed_type(atom_ptr);
      size = type_size(atom, &align);
      if (size > 8 || (size & (size - 1)) ||
          (atok > TOK___atomic_compare_exchange &&
           (0 == btype_size(atom->t & VT_BTYPE) || (atom->t & VT_BTYPE) == VT_PTR)))
        expect("integral or integer-sized pointer target type");
      /* GCC does not care either: */
      /* if (!(atom->t & VT_ATOMIC))
          tcc_warning("pointer target declaration is missing '_Atomic'"); */
      break;

    case 'p':
      if ((vtop->type.t & VT_BTYPE) != VT_PTR || type_size(pointed_type(&vtop->type), &align) != size)
        tcc_error("pointer target type mismatch in argument %d", arg + 1);
      gen_assign_cast(atom_ptr);
      break;
    case 'v':
      gen_assign_cast(atom);
      break;
    case 'l':
      indir();
      gen_assign_cast(atom);
      break;
    case 's':
      save = 1;
      indir();
      store = *vtop;
      vpop();
      break;
    case 'm':
      gen_assign_cast(&int_type);
      break;
    case 'b':
      ct.t = VT_BOOL;
      gen_assign_cast(&ct);
      break;
    }
    if ('.' == template[++arg])
      break;
    skip(',');
  }
  skip(')');

  ct.t = VT_VOID;
  switch (template[arg + 1])
  {
  case 'b':
    ct.t = VT_BOOL;
    break;
  case 'v':
    ct = *atom;
    break;
  }

  sprintf(buf, "%s_%d", get_tok_str(atok, 0), size);
  vpush_helper_func(tok_alloc_const(buf));
  vrott(arg - save + 1);
  // gfunc_call(arg - save);
  tcc_error("7 implement me");
  vpush(&ct);
  PUT_R_RET(vtop, ct.t);
  t = ct.t & VT_BTYPE;
  if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL)
  {
#ifdef PROMOTE_RET
    vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
    vtop->type.t = VT_INT;
#endif
  }
  gen_cast(&ct);
  if (save)
  {
    vpush(&ct);
    *vtop = store;
    vswap();
    vstore();
  }
}

ST_FUNC void unary(void)
{
  int n, t, align, size, r;
  CType type;
  Sym *s;
  AttributeDef ad;

  /* generate line number info */
  if (debug_modes)
    tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

  type.ref = NULL;
  /* XXX: GCC 2.95.3 does not generate a table although it should be
     better here */
tok_next:
  switch (tok)
  {
  case TOK_EXTENSION:
    next();
    goto tok_next;
  case TOK_LCHAR:
#ifdef TCC_TARGET_PE
    t = VT_SHORT | VT_UNSIGNED;
    goto push_tokc;
#endif
  case TOK_CINT:
  case TOK_CCHAR:
    t = VT_INT;
  push_tokc:
    type.t = t;
    vsetc(&type, VT_CONST, &tokc);
    next();
    break;
  case TOK_CUINT:
    t = VT_INT | VT_UNSIGNED;
    goto push_tokc;
  case TOK_CLLONG:
    t = VT_LLONG;
    goto push_tokc;
  case TOK_CULLONG:
    t = VT_LLONG | VT_UNSIGNED;
    goto push_tokc;
  case TOK_CFLOAT:
    t = VT_FLOAT;
    goto push_tokc;
  case TOK_CDOUBLE:
    t = VT_DOUBLE;
    goto push_tokc;
  case TOK_CLDOUBLE:
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
    t = VT_DOUBLE | VT_LONG;
#else
    t = VT_LDOUBLE;
#endif
    goto push_tokc;
  case TOK_CLONG:
    t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG;
    goto push_tokc;
  case TOK_CULONG:
    t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG | VT_UNSIGNED;
    goto push_tokc;
  case TOK___FUNCTION__:
    if (!gnu_ext)
      goto tok_identifier;
    /* fall thru */
  case TOK___FUNC__:
    tok = TOK_STR;
    cstr_reset(&tokcstr);
    cstr_cat(&tokcstr, funcname, 0);
    tokc.str.size = tokcstr.size;
    tokc.str.data = tokcstr.data;
    goto case_TOK_STR;
  case TOK_LSTR:
#ifdef TCC_TARGET_PE
    t = VT_SHORT | VT_UNSIGNED;
#else
    t = VT_INT;
#endif
    goto str_init;
  case TOK_STR:
  case_TOK_STR:
    /* string parsing */
    t = char_type.t;
  str_init:
    if (tcc_state->warn_write_strings & WARN_ON)
      t |= VT_CONSTANT;
    type.t = t;
    mk_pointer(&type);
    type.t |= VT_ARRAY;
    memset(&ad, 0, sizeof(AttributeDef));
    ad.section = rodata_section;
    {
      /* String literals must always emit data, even in nocode_wanted paths.
       * The IR backend defers code generation, so string data must exist
       * when code is later emitted. Force DATA_ONLY_WANTED to ensure
       * allocation proceeds regardless of nocode_wanted state. */
      int saved_nocode = nocode_wanted;
      nocode_wanted |= DATA_ONLY_WANTED;
      decl_initializer_alloc(&type, &ad, VT_CONST, 2, 0, 0);
      nocode_wanted = saved_nocode;
    }
    break;
  case TOK_SOTYPE:
  case '(':
    t = tok;
    next();
    /* cast ? */
    if (parse_btype(&type, &ad, 0))
    {
      type_decl(&type, &ad, &n, TYPE_ABSTRACT);
      skip(')');
      /* check ISOC99 compound literal */
      if (tok == '{')
      {
        /* data is allocated locally by default */
        if (global_expr)
          r = VT_CONST;
        else
          r = VT_LOCAL;
        /* all except arrays are lvalues */
        if (!(type.t & VT_ARRAY))
          r |= VT_LVAL;
        memset(&ad, 0, sizeof(AttributeDef));
        decl_initializer_alloc(&type, &ad, r, 1, 0, 0);
      }
      else if (t == TOK_SOTYPE)
      { /* from sizeof/alignof (...) */
        vpush(&type);
        return;
      }
      else
      {
        unary();
        gen_cast(&type);
      }
    }
    else if (tok == '{')
    {
      int saved_nocode_wanted = nocode_wanted;
      if (CONST_WANTED && !NOEVAL_WANTED)
        expect("constant");
      if (0 == local_scope)
        tcc_error("statement expression outside of function");
      /* statement expression : we do not accept break/continue
         inside as GCC does.  We do retain the nocode_wanted state,
         as statement expressions can't ever be entered from the
         outside, so any reactivation of code emission (from labels
         or loop heads) can be disabled again after the end of it. */
      block(STMT_EXPR);
      /* If the statement expr can be entered, then we retain the current
         nocode_wanted state (from e.g. a 'return 0;' in the stmt-expr).
         If it can't be entered then the state is that from before the
         statement expression.  */
      if (saved_nocode_wanted)
        nocode_wanted = saved_nocode_wanted;
      skip(')');
    }
    else
    {
      gexpr();
      skip(')');
    }
    break;
  case '*':
    next();
    unary();
    indir();
    break;
  case '&':
    next();
    unary();
    /* functions names must be treated as function pointers,
       except for unary '&' and sizeof. Since we consider that
       functions are not lvalues, we only have to handle it
       there and in function calls. */
    /* arrays can also be used although they are not lvalues */
    if ((vtop->type.t & VT_BTYPE) != VT_FUNC && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
      test_lvalue();
    if (vtop->sym)
    {
      vtop->sym->a.addrtaken = 1;
      /* Mark vreg as address-taken in IR so it gets spilled to stack */
      tcc_ir_set_addrtaken(tcc_state->ir, vtop->sym->vreg);

      /* Check if this is a nested function - need trampoline for address-of.
       * Note: setup_nested_func_trampoline replaces vtop->sym with the
       * trampoline symbol, so after this call vtop->sym no longer points
       * to the nested function symbol. */
      if (vtop->sym->a.nested_func)
        setup_nested_func_trampoline(vtop->sym);
    }
    mk_pointer(&vtop->type);
    gaddrof();
    break;
  case '!':
    next();
    unary();
    gen_test_zero(TOK_EQ);
    break;
  case '~':
    next();
    unary();
    vpushi(-1);
    gen_op('^');
    break;
  case '+':
    next();
    unary();
    if ((vtop->type.t & VT_BTYPE) == VT_PTR)
      tcc_error("pointer not accepted for unary plus");
    /* In order to force cast, we add zero, except for floating point
       where we really need an noop (otherwise -0.0 will be transformed
       into +0.0).  */
    if (!is_float(vtop->type.t))
    {
      vpushi(0);
      gen_op('+');
    }
    break;
  case TOK_REAL:
  case TOK_IMAG:
    /* Phase 4 - __real__ and __imag__ operators */
    t = tok;
    next();
    unary();
    if (!(vtop->type.t & VT_COMPLEX))
    {
      if (t == TOK_REAL)
      {
        /* __real__ on non-complex is a no-op */
      }
      else
      {
        /* __imag__ on non-complex returns 0 */
        vpop();
        vpushi(0);
      }
    }
    else
    {
      /* Phase 4: Extract real or imaginary part from complex value
       * Complex float is stored as { real, imag } - two consecutive floats
       * We need to load the appropriate component
       */
      int is_real = (t == TOK_REAL);
      int base_type = vtop->type.t & VT_BTYPE;
      int result_type;

      /* Determine the result type (real component type) */
      if (base_type == VT_DOUBLE)
        result_type = VT_DOUBLE;
      else
        result_type = VT_FLOAT;

      /* The complex value is on the stack, we need to access its components */
      if ((vtop->r & VT_VALMASK) == VT_LOCAL)
      {
        /* Stack variable: adjust offset to access real or imag part */
        if (!is_real)
        {
          /* Imaginary part is at offset +4 for float, +8 for double */
          int offset = (base_type == VT_DOUBLE) ? 8 : 4;
          vtop->c.i += offset;
        }
        /* Change type to the base floating point type */
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
      }
      else if (vtop->r & VT_LVAL)
      {
        /* L-value: dereference and load appropriate component */
        int offset = is_real ? 0 : ((base_type == VT_DOUBLE) ? 8 : 4);

        /* Add offset to address */
        if (offset > 0)
        {
          vpushi(offset);
          gen_op('+');
        }

        /* Load the value */
        vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type | VT_LVAL;
        vtop->r = (vtop->r & ~VT_VALMASK) | VT_LOCAL;
      }
      else
      {
        /* For register values, we need special handling */
        /* Complex values use register pairs: (r0, r1) for float complex */
        /* Real part is in lower register, imag in higher */
        if (is_real)
        {
          /* Real part is already in pr0_reg, just change type */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type;
        }
        else
        {
          /* Imaginary part is in pr1_reg, need to move it */
          vtop->r = (vtop->r & ~VT_VALMASK) | VT_LOCAL; /* Force reload pattern */
          vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | result_type | VT_LVAL;
          /* For now, mark as needing offset for imag part */
          vtop->c.i = (base_type == VT_DOUBLE) ? 8 : 4;
        }
      }
    }
    break;
  case TOK_SIZEOF:
  case TOK_ALIGNOF1:
  case TOK_ALIGNOF2:
  case TOK_ALIGNOF3:
    t = tok;
    next();
    if (tok == '(')
      tok = TOK_SOTYPE;
    expr_type(&type, unary);
    if (t == TOK_SIZEOF)
    {
      vpush_type_size(&type, &align);
      gen_cast_s(VT_SIZE_T);
    }
    else
    {
      type_size(&type, &align);
      s = NULL;
      if (vtop[1].r & VT_SYM)
        s = vtop[1].sym; /* hack: accessing previous vtop */
      if (s && s->a.aligned)
        align = 1 << (s->a.aligned - 1);
      vpushs(align);
    }
    break;

  case TOK_builtin_expect:
    /* __builtin_expect is a no-op for now */
    parse_builtin_params(0, "ee");
    vpop();
    break;
  case TOK_builtin_types_compatible_p:
    parse_builtin_params(0, "tt");
    vtop[-1].type.t &= ~(VT_CONSTANT | VT_VOLATILE);
    vtop[0].type.t &= ~(VT_CONSTANT | VT_VOLATILE);
    n = is_compatible_types(&vtop[-1].type, &vtop[0].type);
    vtop -= 2;
    print_vstack("unary, builtin_types_compatible_p");
    vpushi(n);
    break;
  case TOK_builtin_choose_expr:
  {
    int64_t c;
    next();
    skip('(');
    c = expr_const64();
    skip(',');
    if (!c)
    {
      nocode_wanted++;
    }
    expr_eq();
    if (!c)
    {
      vpop();
      nocode_wanted--;
    }
    skip(',');
    if (c)
    {
      nocode_wanted++;
    }
    expr_eq();
    if (c)
    {
      vpop();
      nocode_wanted--;
    }
    skip(')');
  }
  break;
  case TOK_builtin_constant_p:
    parse_builtin_params(1, "e");
    n = 1;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST || ((vtop->r & VT_SYM) && vtop->sym->a.addrtaken))
      n = 0;
    vtop--;
    print_vstack("unary, builtin_constant_p");
    vpushi(n);
    break;
  case TOK_builtin_unreachable:
    parse_builtin_params(0, ""); /* just skip '()' */
    type.t = VT_VOID;
    vpush(&type);
    CODE_OFF();
    break;
  case TOK_builtin_trap:
    parse_builtin_params(0, ""); /* just skip '()' */
    /* Generate a trap instruction through the IR */
    tcc_ir_put(tcc_state->ir, TCCIR_OP_TRAP, NULL, NULL, NULL);
    type.t = VT_VOID;
    vpush(&type);
    break;
  case TOK_builtin_frame_address:
  case TOK_builtin_return_address:
  {
    int tok1 = tok;
    int level;
    next();
    skip('(');
    level = expr_const();
    if (level < 0)
      tcc_error("%s only takes positive integers", get_tok_str(tok1, 0));
    skip(')');
    type.t = VT_VOID;
    mk_pointer(&type);
#ifdef TCC_TARGET_ARM
    if (level > 0)
    {
      /* ARM Thumb: frame chain walking for level>0 is not supported.
       * Return NULL, which is a valid implementation
       * (GCC torture tests accept NULL for unsupported levels). */
      vpushi(0);
      vtop->type = type;
    }
    else
    {
      /* level == 0: force standard frame record {FP, LR} */
      tcc_state->force_frame_pointer = 1;
      if (tok1 == TOK_builtin_return_address)
        tcc_state->force_lr_save = 1;
      vset(&type, VT_LOCAL, 0); /* FP value */
      if (tok1 == TOK_builtin_return_address)
      {
        /* LR is at [FP + PTR_SIZE] in the standard frame record */
        vpushi(PTR_SIZE);
        gen_op('+');
        mk_pointer(&vtop->type);
        indir();
      }
    }
#else
    /* Non-ARM targets: original chain-walking implementation */
    tcc_state->force_frame_pointer = 1;
    vset(&type, VT_LOCAL, 0); /* local frame */
    while (level--)
    {
#ifdef TCC_TARGET_RISCV64
      vpushi(2 * PTR_SIZE);
      gen_op('-');
#endif
      mk_pointer(&vtop->type);
      indir(); /* -> parent frame */
    }
    if (tok1 == TOK_builtin_return_address)
    {
#ifdef TCC_TARGET_RISCV64
      vpushi(PTR_SIZE);
      gen_op('-');
#else
      vpushi(PTR_SIZE);
      gen_op('+');
#endif
      mk_pointer(&vtop->type);
      indir();
    }
#endif
  }
  break;
#ifdef TCC_TARGET_RISCV64
  case TOK_builtin_va_start:
    parse_builtin_params(0, "ee");
    r = vtop->r & VT_VALMASK;
    if (r == VT_LLOCAL)
      r = VT_LOCAL;
    if (r != VT_LOCAL)
      tcc_error("__builtin_va_start expects a local variable");
    gen_va_start();
    vstore();
    break;
#endif
#ifdef TCC_TARGET_X86_64
#ifdef TCC_TARGET_PE
  case TOK_builtin_va_start:
    parse_builtin_params(0, "ee");
    r = vtop->r & VT_VALMASK;
    if (r == VT_LLOCAL)
      r = VT_LOCAL;
    if (r != VT_LOCAL)
      tcc_error("__builtin_va_start expects a local variable");
    vtop->r = r;
    vtop->type = char_pointer_type;
    vtop->c.i += 8;
    vstore();
    break;
#else
  case TOK_builtin_va_arg_types:
    parse_builtin_params(0, "t");
    vpushi(classify_x86_64_va_arg(&vtop->type));
    vswap();
    vpop();
    break;
#endif
#endif

#ifdef TCC_TARGET_ARM64
  case TOK_builtin_va_start:
  {
    parse_builtin_params(0, "ee");
    // xx check types
    gen_va_start();
    vpushi(0);
    vtop->type.t = VT_VOID;
    break;
  }
  case TOK_builtin_va_arg:
  {
    parse_builtin_params(0, "et");
    type = vtop->type;
    vpop();
    // xx check types
    gen_va_arg(&type);
    vtop->type = type;
    break;
  }
  case TOK___arm64_clear_cache:
  {
    parse_builtin_params(0, "ee");
    gen_clear_cache();
    vpushi(0);
    vtop->type.t = VT_VOID;
    break;
  }
#endif

  /* atomic operations */
  case TOK___atomic_store:
  case TOK___atomic_load:
  case TOK___atomic_exchange:
  case TOK___atomic_compare_exchange:
  case TOK___atomic_fetch_add:
  case TOK___atomic_fetch_sub:
  case TOK___atomic_fetch_or:
  case TOK___atomic_fetch_xor:
  case TOK___atomic_fetch_and:
  case TOK___atomic_fetch_nand:
  case TOK___atomic_add_fetch:
  case TOK___atomic_sub_fetch:
  case TOK___atomic_or_fetch:
  case TOK___atomic_xor_fetch:
  case TOK___atomic_and_fetch:
  case TOK___atomic_nand_fetch:
    parse_atomic(tok);
    break;

  /* pre operations */
  case TOK_INC:
  case TOK_DEC:
    t = tok;
    next();
    unary();
    inc(0, t);
    break;
  case '-':
    next();
    unary();
    if (is_float(vtop->type.t))
    {
      gen_opif(TOK_NEG);
    }
    else
    {
      vpushi(0);
      vswap();
      gen_op('-');
    }
    break;
  case TOK_LAND:
    if (!gnu_ext)
      goto tok_identifier;
    next();
    /* allow to take the address of a label */
    if (tok < TOK_UIDENT)
      expect("label identifier");
    s = label_find(tok);
    if (!s)
    {
      s = label_push(&global_label_stack, tok, LABEL_FORWARD);
    }
    else
    {
      if (s->r == LABEL_DECLARED)
        s->r = LABEL_FORWARD;
    }
    /* Mark that this label's address is taken (&&label). In IR mode, the
       symbol definition is deferred until after code generation when the
       final code offsets are known.
       Use -3 as special marker (distinct from valid ELF indices >= 0,
       and from -1/-2 used for type descriptors and struct definitions).
       Only set if not already marked/having an ELF symbol. */
    if (s->c <= 0)
      s->c = -3; /* LABEL_ADDR_TAKEN marker */
    if ((s->type.t & VT_BTYPE) != VT_PTR)
    {
      s->type.t = VT_VOID;
      mk_pointer(&s->type);
      s->type.t |= VT_STATIC;
    }
    vpushsym(&s->type, s);
    next();
    break;

  case TOK_GENERIC:
  {
    CType controlling_type;
    int has_default = 0;
    int has_match = 0;
    int learn = 0;
    TokenString *str = NULL;
    int saved_nocode_wanted = nocode_wanted;
    nocode_wanted &= ~CONST_WANTED_MASK;

    next();
    skip('(');
    expr_type(&controlling_type, expr_eq);
    convert_parameter_type(&controlling_type);

    nocode_wanted = saved_nocode_wanted;

    for (;;)
    {
      learn = 0;
      skip(',');
      if (tok == TOK_DEFAULT)
      {
        if (has_default)
          tcc_error("too many 'default'");
        has_default = 1;
        if (!has_match)
          learn = 1;
        next();
      }
      else
      {
        AttributeDef ad_tmp;
        int itmp;
        CType cur_type;

        parse_btype(&cur_type, &ad_tmp, 0);
        type_decl(&cur_type, &ad_tmp, &itmp, TYPE_ABSTRACT);
        if (compare_types(&controlling_type, &cur_type, 0))
        {
          if (has_match)
          {
            tcc_error("type match twice");
          }
          has_match = 1;
          learn = 1;
        }
      }
      skip(':');
      if (learn)
      {
        if (str)
          tok_str_free(str);
        skip_or_save_block(&str);
      }
      else
      {
        skip_or_save_block(NULL);
      }
      if (tok == ')')
        break;
    }
    if (!str)
    {
      char buf[60];
      type_to_str(buf, sizeof buf, &controlling_type, NULL);
      tcc_error("type '%s' does not match any association", buf);
    }
    begin_macro(str, 1);
    next();
    expr_eq();
    if (tok != TOK_EOF)
      expect(",");
    end_macro();
    next();
    break;
  }
  // special qnan , snan and infinity values
  case TOK___NAN__:
    n = 0x7fc00000;
  special_math_val:
    vpushi(n);
    vtop->type.t = VT_FLOAT;
    next();
    break;
  case TOK___SNAN__:
    n = 0x7f800001;
    goto special_math_val;
  case TOK___INF__:
    n = 0x7f800000;
    goto special_math_val;

  default:
  tok_identifier:
    if (tok < TOK_UIDENT)
      tcc_error("expression expected before '%s'", get_tok_str(tok, &tokc));
    t = tok;
    next();
    s = sym_find(t);
    if (!s || IS_ASM_SYM(s))
    {
      /* Check if this identifier is a captured variable from an enclosing function */
      NestedFunc *nf = tcc_state->current_nested_func;
      if (nf && nf->nb_captured > 0)
      {
        /* Search captured_offsets for matching token */
        for (int i = 0; i < nf->nb_captured; i++)
        {
          if (nf->captured_tokens[i] == t)
          {
            /* Found a match - create a fake symbol for this captured variable.
             * The offset is the parent's FP-relative offset (resolved after
             * parent's register allocation). Access goes through R10 (static chain). */
            s = sym_malloc();
            memset(s, 0, sizeof(*s));
            s->v = t;
            s->type = nf->captured_types[i]; /* Use actual captured variable type */
            s->r = VT_LOCAL | VT_LVAL;       /* LOCAL + LVAL so it works as both value and assignment target */
            s->c = nf->captured_offsets[i];  /* Parent's FP offset */
            s->vreg = -1;                    /* No vreg in nested function's IR — pure stack offset via chain */
            s->sym_scope = 0;
            goto found_captured_var;
          }
        }
      }

      const char *name = get_tok_str(t, NULL);
      if (tok != '(')
        tcc_error("'%s' undeclared", name);
      /* for simple function calls, we tolerate undeclared
         external reference to int() function */
      tcc_warning_c(warn_implicit_function_declaration)("implicit declaration of function '%s'", name);
      s = external_global_sym(t, &func_old_type);
    }
  found_captured_var:

    r = s->r;
    /* A symbol that has a register is a local register variable,
       which starts out as VT_LOCAL value.  */
    if ((r & VT_VALMASK) < VT_CONST)
    {
      // parameter is always a local value
      if (!(r & VT_PARAM))
      {
        r = (r & ~VT_VALMASK) | VT_LOCAL;
      }
    }

    vset(&s->type, r, s->c);
    /* Point to s as backpointer (even without r&VT_SYM).
       Will be used by at least the x86 inline asm parser for
       regvars.  */
    vtop->sym = s;
    vtop->vr = s->vreg;

    /* Array-to-pointer decay for captured variables (nested functions).
     * Captured arrays have VT_ARRAY type and VT_LVAL set. They need to
     * decay to pointers for subscript and pointer arithmetic to work. */
    if ((vtop->type.t & VT_ARRAY) && (vtop->r & VT_LVAL))
    {
      gaddrof();
      vtop->type.t &= ~VT_ARRAY;
    }

    if (r & VT_SYM)
    {
      vtop->c.i = 0;
#ifdef TCC_TARGET_PE
      if (s->a.dllimport)
      {
        mk_pointer(&vtop->type);
        vtop->r |= VT_LVAL;
        indir();
      }
#endif
    }
    else if (r == VT_CONST && IS_ENUM_VAL(s->type.t))
    {
      vtop->c.i = s->enum_val;
    }

    /* Implicit function-to-pointer: if a nested function name is used in
     * a non-call context (next token is NOT '('), it needs a trampoline. */
    if (s->a.nested_func && tok != '(')
      setup_nested_func_trampoline(s);

    break;
  }

  /* post operations */
  while (1)
  {
    if (tok == TOK_INC || tok == TOK_DEC)
    {
      inc(1, tok);
      next();
    }
    else if (tok == '.' || tok == TOK_ARROW)
    {
      int qualifiers, cumofs;
      /* field */
      if (tok == TOK_ARROW)
        indir();
      qualifiers = vtop->type.t & (VT_CONSTANT | VT_VOLATILE);
      test_lvalue();
      /* expect pointer on structure */
      next();
      s = find_field(&vtop->type, tok, &cumofs);
      /* add field offset to pointer */
      gaddrof();
      vtop->type = char_pointer_type; /* change type to 'char *' */
      vpushi(cumofs);
      gen_op('+');
      /* change type to field type, and set to lvalue */
      vtop->type = s->type;
      vtop->type.t |= qualifiers;
      /* an array is never an lvalue */
      if (!(vtop->type.t & VT_ARRAY))
      {
        vtop->r |= VT_LVAL;
#ifdef CONFIG_TCC_BCHECK
        /* if bound checking, the referenced pointer must be checked */
        if (tcc_state->do_bounds_check)
          vtop->r |= VT_MUSTBOUND;
#endif
      }
      next();
    }
    else if (tok == '[')
    {
      next();
      gexpr();
      gen_op('+');
      indir();
      skip(']');
    }
    else if (tok == '(')
    {
      SValue ret;
      Sym *sa;
      int nb_args, ret_nregs, ret_align, regsize, variadic;
      TokenString *p, *p2;

      /* function call  */
      if ((vtop->type.t & VT_BTYPE) != VT_FUNC)
      {
        /* pointer test (no array accepted) */
        if ((vtop->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
        {
          vtop->type = *pointed_type(&vtop->type);
          if ((vtop->type.t & VT_BTYPE) != VT_FUNC)
            goto error_func;
        }
        else
        {
        error_func:
          expect("function pointer");
        }
      }
      else
      {
        vtop->r &= ~VT_LVAL; /* no lvalue */
      }
      /* get return type */
      /* Save function symbol before switching to type ref - needed for nested_func check */
      Sym *call_func_sym = vtop->sym;
      s = vtop->type.ref;
      next();

      /* If calling a nested function, emit SET_CHAIN to pass static chain (parent FP).
       * Only emit when the caller is the callee's PARENT.  When the caller is
       * itself a nested function (current_nested_func != NULL) and the callee is
       * a sibling (defined in the same enclosing scope), R10 already holds the
       * correct chain pointer from our own incoming chain — emitting SET_CHAIN
       * would clobber it with R7 which may be an unrelated frame pointer. */
      if (tcc_state->ir && call_func_sym && call_func_sym->a.nested_func)
      {
        int emit_set_chain = 1;
        if (tcc_state->current_nested_func)
        {
          /* Caller is a nested function.  Determine if callee is our child
           * (defined inside our body) or a sibling (defined in the same parent
           * scope).  Only emit SET_CHAIN for child calls. */
          NestedFunc *callee_nf = NULL;
          for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++)
          {
            if (tcc_state->nested_funcs[ni].sym == call_func_sym)
            {
              callee_nf = &tcc_state->nested_funcs[ni];
              break;
            }
          }
          if (callee_nf && callee_nf->parent_nf != tcc_state->current_nested_func)
          {
            /* Sibling call: R10 already has the correct parent FP */
            emit_set_chain = 0;
          }
        }
        if (emit_set_chain)
        {
          /* Emit SET_CHAIN: R10 = FP (current frame pointer) */
          SValue src, dest;
          svalue_init(&src);
          svalue_init(&dest);
          src.type.t = VT_PTR;
          src.r = 0;
          src.vr = -1;
          dest.type.t = VT_PTR;
          dest.r = 0;
          dest.vr = -1;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_SET_CHAIN, &src, NULL, &dest);
        }
      }

      /* Each IR-level call gets a unique call_id so FUNCPARAM* can be bound
       * without fragile nested-depth scanning.
       */
      int call_id = 0;
      if (!NOEVAL_WANTED && tcc_state->ir)
        call_id = tcc_state->ir->next_call_id++;

      sa = s->next; /* first parameter */
      nb_args = regsize = 0;
      /* compute first implicit argument if a structure is returned */
      if ((s->type.t & VT_BTYPE) == VT_STRUCT)
      {
        variadic = (s->f.func_type == FUNC_ELLIPSIS);
        ret_nregs = gfunc_sret(&s->type, variadic, &ret.type, &ret_align, &regsize);
        if (ret_nregs <= 0)
        {
          /* get some space for the returned structure */
          size = type_size(&s->type, &align);
#ifdef TCC_TARGET_ARM64
          /* On arm64, a small struct is return in registers.
             It is much easier to write it to memory if we know
             that we are allowed to write some extra bytes, so
             round the allocated space up to a power of 2: */
          if (size < 16)
            while (size & (size - 1))
              size = (size | (size - 1)) + 1;
#endif
          loc = (loc - size) & -align;
          ret.type = s->type;
          ret.r = VT_LOCAL | VT_LVAL;
          /* pass it as 'int' to avoid structure arg passing
             problems */
          vseti(VT_LOCAL, loc);
#ifdef CONFIG_TCC_BCHECK
          if (tcc_state->do_bounds_check)
            --loc;
#endif
          ret.c = vtop->c;
          if (ret_nregs < 0)
          {
            vtop--;
            print_vstack("unary, function call");
          }
          else
          {
            /* ret_nregs == 0: struct is returned via an implicit first argument
             * (sret pointer). In IR mode we must actually emit the parameter and
             * pop it, otherwise it stays on the value stack and triggers
             * check_vstack() failures (vstack leak).
             *
             * Keep parameter indices 0-based: this implicit argument is param #0.
             */
            if (!NOEVAL_WANTED)
            {
              SValue num;
              svalue_init(&num);
              num.vr = -1;
              num.r = VT_CONST;
              num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
              TCCGEN_DEBUG(
                  "[TCCGEN] FUNCPARAMVAL push: site=sret_param0 call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                  call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)num.c.i), vtop->r, vtop->vr);
              tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, vtop, &num, NULL);
            }
            vtop--;
            nb_args++;
          }
        }
      }
      else
      {
        ret_nregs = 1;
        ret.type = s->type;
      }

      if (ret_nregs > 0)
      {
        /* return in register */
        ret.c.i = 0;
        PUT_R_RET(&ret, ret.type.t);
      }

      p = NULL;
      if (tok != ')')
      {
        r = tcc_state->reverse_funcargs;
        SValue num;
        svalue_init(&num);
        num.vr = -1;
        for (;;)
        {
          if (r)
          {
            skip_or_save_block(&p2);
            p2->prev = p, p = p2;
          }
          else
          {
            /* IR expects 0-based parameter indices.
             * Keep FUNCPARAMVAL numbering consistent across all call sites. */
            expr_eq();
            /* Convert VT_CMP/VT_JMP to actual 0/1 value before passing as
             * parameter */
            if (!NOEVAL_WANTED)
              tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
            gfunc_param_typed(s, sa);
            if (!NOEVAL_WANTED)
            {
              num.r = VT_CONST;
              num.c.i = TCCIR_ENCODE_PARAM(call_id, nb_args);
              TCCGEN_DEBUG(
                  "[TCCGEN] FUNCPARAMVAL push: site=forward_arg call_id=%d param_idx=%d nb_args=%d vtop_r=0x%x "
                  "vtop_vr=%d\n",
                  call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)num.c.i), nb_args, vtop->r, vtop->vr);
              tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, vtop, &num, NULL);
            }
            vtop--; /* consumed */
          }
          nb_args++;
          if (sa)
            sa = sa->next;
          if (tok == ')')
            break;
          skip(',');
        }
      }
      if (sa)
        tcc_error("too few arguments to function");

      if (p)
      { /* with reverse_funcargs */
        for (n = 0; p; p = p2, ++n)
        {
          p2 = p, sa = s;
          do
          {
            sa = sa->next, p2 = p2->prev;
          } while (p2 && sa);
          p2 = p->prev;
          begin_macro(p, 1), next();
          expr_eq();
          gfunc_param_typed(s, sa);
          /* We evaluate right-to-left; assign 0-based parameter indices
           * corresponding to original left-to-right argument positions.
           */
          if (!NOEVAL_WANTED)
          {
            SValue num;
            svalue_init(&num);
            num.vr = -1;
            num.r = VT_CONST;
            num.c.i = TCCIR_ENCODE_PARAM(call_id, nb_args - 1 - n);
            TCCGEN_DEBUG(
                "[TCCGEN] FUNCPARAMVAL push: site=reverse_arg call_id=%d param_idx=%d n=%d nb_args=%d vtop_r=0x%x "
                "vtop_vr=%d\n",
                call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)num.c.i), n, nb_args, vtop->r, vtop->vr);
            tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, vtop, &num, NULL);
          }
          vtop--; /* consumed */
          end_macro();
        }
      }

      next();
      // gfunc_call(nb_args);

      int return_vreg = -1;
      if (NOEVAL_WANTED)
      {
        /* When in sizeof/typeof context, skip IR emission but still handle stack */
        --vtop;
      }
      else if ((s->type.t & VT_BTYPE) == VT_VOID)
      {
        /* In IR mode, make sure the call target is a VALUE (register/temp),
         * not an lvalue. Indirect calls like tabl1[i]() produce an lvalue
         * (memory reference) for tabl1[i]; we must LOAD it to get the actual
         * function pointer value before emitting FUNCCALL.
         * NOTE: We check s->type.t (the function's return type), not vtop->type.t
         * (which is VT_FUNC for function pointers). */
        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, nb_args);
        /* Emit FUNCPARAMVOID for 0-arg calls so backend creates a call site */
        if (nb_args == 0)
        {
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVOID, NULL, &call_id_sv, NULL);
        }
        /* For indirect calls (VT_LVAL set), emit a LOAD to get the function pointer value */
        SValue call_target = *vtop;
        if (vtop->r & VT_LVAL)
        {
          SValue load_dest;
          svalue_init(&load_dest);
          load_dest.type = vtop->type;
          load_dest.r = 0;
          load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);
          call_target = load_dest;
          call_target.r &= ~VT_LVAL; /* Clear VT_LVAL since we now have the value */
        }
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &call_target, &call_id_sv, NULL);
        --vtop;
      }
      else
      {
        SValue dest;
        svalue_init(&dest);
        if (nb_args == 0)
        {
          SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 0);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVOID, NULL, &call_id_sv, NULL);
        }
        /* Use the actual return type so 64-bit/float returns are modeled correctly
         * (e.g., __aeabi_f2d returns a double in R0:R1). */
        dest.type = ret.type;
        dest.r = 0;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        return_vreg = dest.vr;

        /* For indirect calls (VT_LVAL set), emit a LOAD to get the function pointer value */
        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, nb_args);
        SValue call_target = *vtop;
        if (vtop->r & VT_LVAL)
        {
          SValue load_dest;
          svalue_init(&load_dest);
          load_dest.type = vtop->type;
          load_dest.r = 0;
          load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dest);
          call_target = load_dest;
          call_target.r &= ~VT_LVAL; /* Clear VT_LVAL since we now have the value */
        }
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &call_target, &call_id_sv, &dest);
        --vtop;
      }

      if (ret_nregs < 0)
      {
        vsetc(&ret.type, ret.r, &ret.c);
#ifdef TCC_TARGET_RISCV64
        arch_transfer_ret_regs(1);
#endif
      }
      else if (ret_nregs == 0)
      {
        /* Struct returned via sret pointer: the callee already wrote to the
         * sret buffer. Just push the buffer location as an lvalue. */
        vsetc(&ret.type, ret.r, &ret.c);
        /* Do NOT set vtop->vr = return_vreg - there's no return register for sret */
      }
      else
      {
        /* return value */
        n = ret_nregs;
        while (n > 1)
        {
          int rc = reg_classes[ret.r] & ~(RC_INT | RC_FLOAT);
          /* We assume that when a structure is returned in multiple
             registers, their classes are consecutive values of the
             suite s(n) = 2^n */
          rc <<= --n;
          for (r = 0; r < NB_REGS; ++r)
            if (reg_classes[r] & rc)
              break;
          vsetc(&ret.type, r, &ret.c);
          vtop->vr = return_vreg;
        }
        vsetc(&ret.type, ret.r, &ret.c);
        vtop->vr = return_vreg;

        /* handle packed struct return */
        if (((s->type.t & VT_BTYPE) == VT_STRUCT) && ret_nregs)
        {
          int addr, offset;

          size = type_size(&s->type, &align);
          /* We're writing whole regs often, make sure there's enough
             space.  Assume register size is power of 2.  */
          size = (size + regsize - 1) & -regsize;
          if (ret_align > align)
            align = ret_align;
          loc = (loc - size) & -align;
          addr = loc;
          offset = 0;
          for (;;)
          {
            vset(&ret.type, VT_LOCAL | VT_LVAL, addr + offset);
            vswap();
            vstore();
            vtop--;
            print_vstack("unary, function call(2)");
            if (--ret_nregs == 0)
              break;
            offset += regsize;
          }
          vset(&s->type, VT_LOCAL | VT_LVAL, addr);
        }

        /* Promote char/short return values. This is matters only
           for calling function that were not compiled by TCC and
           only on some architectures.  For those where it doesn't
           matter we expect things to be already promoted to int,
           but not larger.  */
        t = s->type.t & VT_BTYPE;
        if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL)
        {
#ifdef PROMOTE_RET
          vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
          vtop->type.t = VT_INT;
#endif
        }
      }
      if (s->f.func_noreturn)
      {
        if (debug_modes)
          tcc_tcov_block_end(tcc_state, -1);
        CODE_OFF();
      }
    }
    else
    {
      break;
    }
  }
}

#ifndef precedence_parser /* original top-down parser */

static void expr_prod(void)
{
  int t;

  unary();
  while ((t = tok) == '*' || t == '/' || t == '%')
  {
    next();
    unary();
    gen_op(t);
  }
}

static void expr_sum(void)
{
  int t;

  expr_prod();
  while ((t = tok) == '+' || t == '-')
  {
    next();
    expr_prod();
    gen_op(t);
  }
}

static void expr_shift(void)
{
  int t;

  expr_sum();
  while ((t = tok) == TOK_SHL || t == TOK_SAR)
  {
    next();
    expr_sum();
    gen_op(t);
  }
}

static void expr_cmp(void)
{
  int t;

  expr_shift();
  while (((t = tok) >= TOK_ULE && t <= TOK_GT) || t == TOK_ULT || t == TOK_UGE)
  {
    next();
    expr_shift();
    gen_op(t);
  }
}

static void expr_cmpeq(void)
{
  int t;

  expr_cmp();
  while ((t = tok) == TOK_EQ || t == TOK_NE)
  {
    next();
    expr_cmp();
    gen_op(t);
  }
}

static void expr_and(void)
{
  expr_cmpeq();
  while (tok == '&')
  {
    next();
    expr_cmpeq();
    gen_op('&');
  }
}

static void expr_xor(void)
{
  expr_and();
  while (tok == '^')
  {
    next();
    expr_and();
    gen_op('^');
  }
}

static void expr_or(void)
{
  expr_xor();
  while (tok == '|')
  {
    next();
    expr_xor();
    gen_op('|');
  }
}

static void expr_landor(int op);

static void expr_land(void)
{
  expr_or();
  if (tok == TOK_LAND)
    expr_landor(tok);
}

static void expr_lor(void)
{
  expr_land();
  if (tok == TOK_LOR)
    expr_landor(tok);
}

#define expr_landor_next(op) op == TOK_LAND ? expr_or() : expr_land()
#else /* defined precedence_parser */
#define expr_landor_next(op) unary(), expr_infix(precedence(op) + 1)
#define expr_lor() unary(), expr_infix(1)

static int precedence(int tok)
{
  switch (tok)
  {
  case TOK_LOR:
    return 1;
  case TOK_LAND:
    return 2;
  case '|':
    return 3;
  case '^':
    return 4;
  case '&':
    return 5;
  case TOK_EQ:
  case TOK_NE:
    return 6;
  relat:
  case TOK_ULT:
  case TOK_UGE:
    return 7;
  case TOK_SHL:
  case TOK_SAR:
    return 8;
  case '+':
  case '-':
    return 9;
  case '*':
  case '/':
  case '%':
    return 10;
  default:
    if (tok >= TOK_ULE && tok <= TOK_GT)
      goto relat;
    return 0;
  }
}
static unsigned char prec[256];
static void init_prec(void)
{
  int i;
  for (i = 0; i < 256; i++)
    prec[i] = precedence(i);
}
#define precedence(i) ((unsigned)i < 256 ? prec[i] : 0)

static void expr_landor(int op);

static void expr_infix(int p)
{
  int t = tok, p2;
  while ((p2 = precedence(t)) >= p)
  {
    if (t == TOK_LOR || t == TOK_LAND)
    {
      expr_landor(t);
    }
    else
    {
      next();
      unary();
      if (precedence(tok) > p2)
        expr_infix(p2 + 1);
      gen_op(t);
    }
    t = tok;
  }
}
#endif

/* Assuming vtop is a value used in a conditional context
   (i.e. compared with zero) return 0 if it's false, 1 if
   true and -1 if it can't be statically determined.  */
static int condition_3way(void)
{
  int c = -1;
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && (!(vtop->r & VT_SYM) || !vtop->sym->a.weak))
  {
    vdup();
    gen_cast_s(VT_BOOL);
    c = vtop->c.i;
    vpop();
  }
  return c;
}

static void expr_landor(int op)
{
  int t = 0, cc = 1, f = 0, i = op == TOK_LAND, c;

  /* In classic (non-IR) codegen, jump-chain sentinel is 0.
     In IR mode, jump-chain sentinel is -1 (see tcc_ir_backpatch). */
  if (tcc_state->ir != NULL)
    t = -1;

  /* Standard branch-based evaluation */
  for (;;)
  {
    c = f ? i : condition_3way();
    if (c < 0)
    {
      cc = 0;
    }
    // save_regs(1), cc = 0;
    else if (c != i)
      nocode_wanted++, f = 1;
    if (tok != op)
      break;
    if (c < 0)
    {
      // t = gvtst(i, t);
      t = tcc_ir_codegen_test_gen(tcc_state->ir, i, t);
    }
    else
      vpop();
    next();
    {
      int saved_nocode = nocode_wanted;
      expr_landor_next(op);
      nocode_wanted = saved_nocode;
    }
  }

  if (cc || f)
  {
    vpop();
    vpushi(i ^ f);
    if (tcc_state->ir == NULL)
    {
      gsym(t);
    }
    else
    {
      tcc_ir_backpatch_to_here(tcc_state->ir, t);
    }
    nocode_wanted -= f;
  }
  else
  {
    gvtst_set(i, t);
    // vset_VT_JMP();
  }
}

static int is_cond_bool(SValue *sv)
{
  /* Only return true for actual comparison results (VT_CMP).
   * Previously this also returned true for constants 0/1, but that caused
   * incorrect code generation for ternary expressions like `x == 0 ? 1 : 0`
   * because the optimization path would generate SETIF instructions that
   * depend on stale condition flags after unconditional branches. */
  if (sv->r == VT_CMP)
    return 1;
  return 0;
}

static void expr_cond(void)
{
  int tt, u, r1, r2, rc, t1, t2, islv, c, g;
  SValue sv;
  CType type;

  expr_lor();
  if (tok == '?')
  {
    next();
    c = condition_3way();
    g = (tok == ':' && gnu_ext);
    tt = -1; /* -1 = no chain */
    if (!g)
    {
      if (c < 0)
      {
        tt = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
      }
      else
      {
        vpop();
      }
    }
    else if (c < 0)
    {
      /* needed to avoid having different registers saved in
         each branch */
      gv_dup();
      tt = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
    }

    if (c == 0)
      nocode_wanted++;
    if (!g)
      gexpr();

    if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
      mk_pointer(&vtop->type);
    sv = *vtop; /* save value to handle it later */
    vtop--;     /* no vpop so that FP stack is not flushed */
    print_vstack("expr_cond");

    if (g)
    {
      u = tt;
    }
    else if (c < 0)
    {
      u = gjmp(-1); /* -1 = no chain */
      tcc_ir_backpatch_to_here(tcc_state->ir, tt);
    }
    else
      u = -1; /* -1 = no chain */

    if (c == 0)
      nocode_wanted--;
    if (c == 1)
      nocode_wanted++;
    skip(':');
    expr_cond();

    if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
      mk_pointer(&vtop->type);

    /* cast operands to correct type according to ISOC rules */
    if (!combine_types(&type, &sv, vtop, '?'))
      type_incompatibility_error(&sv.type, &vtop->type, "type mismatch in conditional expression (have '%s' and '%s')");

    if (c < 0 && is_cond_bool(vtop) && is_cond_bool(&sv))
    {
      /* optimize "if (f ? a > b : c || d) ..." for example, where normally
         "a < b" and "c || d" would be forced to "(int)0/1" first, whereas
         this code jumps directly to the if's then/else branches. */
      t1 = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
      t2 = gjmp(-1); /* -1 = no chain */
      tcc_ir_backpatch_to_here(tcc_state->ir, u);
      vpushv(&sv);
      /* combine jump targets of 2nd op with VT_CMP of 1st op */
      gvtst_set(0, t1);
      gvtst_set(1, t2);
      gen_cast(&type);
      //  tcc_warning("two conditions expr_cond");
      return;
    }

    /* keep structs lvalue by transforming `(expr ? a : b)` to `*(expr ? &a :
      &b)` so that `(expr ? a : b).mem` does not error with "lvalue expected".
      If the condition is statically false (c == 0), the expression reduces to
      the selected operand and is already a proper lvalue, so skip this
      transformation (otherwise we'd call indir() on a non-pointer). */
    islv = (c != 0) && (vtop->r & VT_LVAL) && (sv.r & VT_LVAL) && VT_STRUCT == (type.t & VT_BTYPE);

    if (c != 0)
    {
      /* Arrays must decay to pointers BEFORE gen_cast overwrites the type.
         gen_cast converts array type to pointer type but doesn't compute the
         address. If we don't decay here, the VT_ARRAY flag is lost and later
         gv() won't recognize it needs to call gaddrof().

         Note: Local arrays are stored without VT_LVAL in the symbol table
         (they decay to pointers immediately). So we check for VT_ARRAY
         regardless of VT_LVAL for locals. */
      int is_local_array = ((vtop->r & VT_VALMASK) == VT_LOCAL) && (vtop->type.t & VT_ARRAY);
      int is_lval_array = (vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY);
      if (is_lval_array || is_local_array)
      {
        /* For local arrays without VT_LVAL, temporarily set it for gaddrof */
        if (is_local_array && !(vtop->r & VT_LVAL))
          vtop->r |= VT_LVAL;
        gaddrof();
        vtop->type.t &= ~VT_ARRAY;
      }
      gen_cast(&type);
      if (islv)
      {
        mk_pointer(&vtop->type);
        gaddrof();
      }
      else if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
        gaddrof();
    }
    else
    {
      /* Even if the condition is a compile-time constant, the conditional
         operator's result type is determined from both operands.
         Do not reduce `0 ? a : b` to just `b`'s type; this breaks sizeof/_Generic.
         Cast the selected (false) operand to the combined result type.
         Keep struct lvalues untouched (no &/ * transformation) in this case. */
      /* Arrays must decay here too */
      if ((vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY))
      {
        gaddrof();
        vtop->type.t &= ~VT_ARRAY;
      }
      gen_cast(&type);
    }

    rc = RC_TYPE(type.t);

    tt = r2 = 0;
    int false_vreg = 0; /* Save false branch vreg for IR mode */
    if (c < 0)
    {
      r2 = gv(rc);
      false_vreg = vtop->vr; /* Save the false branch's vreg */
      tt = gjmp(-1);         /* -1 = no chain */
    }
    tcc_ir_backpatch_to_here(tcc_state->ir, u);
    if (c == 1)
      nocode_wanted--;

    /* this is horrible, but we must also convert first
       operand */
    if (c != 0)
    {
      *vtop = sv;
      /* Arrays must decay to pointers BEFORE gen_cast overwrites the type.
         Same logic as for the false branch - handle local arrays without VT_LVAL. */
      int is_local_array = ((vtop->r & VT_VALMASK) == VT_LOCAL) && (vtop->type.t & VT_ARRAY);
      int is_lval_array = (vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY);
      if (is_lval_array || is_local_array)
      {
        /* For local arrays without VT_LVAL, temporarily set it for gaddrof */
        if (is_local_array && !(vtop->r & VT_LVAL))
          vtop->r |= VT_LVAL;
        gaddrof();
        vtop->type.t &= ~VT_ARRAY;
      }
      gen_cast(&type);
      if (islv)
      {
        mk_pointer(&vtop->type);
        gaddrof();
      }
      else if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
        gaddrof();
    }

    if (c < 0)
    {
      r1 = gv(rc);
      /* For IR mode: after both branches are materialized, we need to ensure
       * they converge to the same vreg at the merge point.
       * Generate ASSIGN from true_vreg to false_vreg (which is used at merge). */
      int true_vreg = vtop->vr;
      int true_vreg_valid =
          (true_vreg != -1) && (TCCIR_DECODE_VREG_TYPE(true_vreg) >= 1) && (TCCIR_DECODE_VREG_TYPE(true_vreg) <= 3);
      int false_vreg_valid =
          (false_vreg != -1) && (TCCIR_DECODE_VREG_TYPE(false_vreg) >= 1) && (TCCIR_DECODE_VREG_TYPE(false_vreg) <= 3);
      if (tcc_state->ir && true_vreg_valid && false_vreg_valid && true_vreg != false_vreg)
      {
        /* Copy true branch result to false branch's vreg so both paths use same vreg */
        SValue src, dest;
        svalue_init(&src);
        svalue_init(&dest);
        src.vr = true_vreg;
        src.type = vtop->type;
        dest.vr = false_vreg;
        dest.type = vtop->type;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
        vtop->vr = false_vreg;
      }
      if (!tcc_state->ir)
      {
        move_reg(r2, r1, islv ? VT_PTR : type.t);
        vtop->r = r2;
      }
      tcc_ir_backpatch_to_here(tcc_state->ir, tt);
    }

    if (islv)
      indir();
  }
}

static void expr_eq(void)
{
  int t;

  expr_cond();
  if ((t = tok) == '=' || TOK_ASSIGN(t))
  {
    test_lvalue();
    next();
    if (t == '=')
    {
      expr_eq();
    }
    else
    {
      vdup();
      expr_eq();
      gen_op(TOK_ASSIGN_OP(t));
    }
    vstore();
  }
}

ST_FUNC void gexpr(void)
{
  expr_eq();
  if (tok == ',')
  {
    do
    {
      vpop();
      next();
      expr_eq();
      tcc_ir_codegen_drop_return(tcc_state->ir);
    } while (tok == ',');

    /* convert array & function to pointer */
    convert_parameter_type(&vtop->type);

    /* make builtin_constant_p((1,2)) return 0 (like on gcc) */
    if ((vtop->r & VT_VALMASK) == VT_CONST && nocode_wanted && !CONST_WANTED)
      gv(RC_TYPE(vtop->type.t));
  }
}

/* parse a constant expression and return value in vtop.  */
static void expr_const1(void)
{
  nocode_wanted += CONST_WANTED_BIT;
  expr_cond();
  nocode_wanted -= CONST_WANTED_BIT;
}

/* parse an integer constant and return its value. */
static inline int64_t expr_const64(void)
{
  int64_t c;
  expr_const1();
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
    expect("constant expression");
  c = vtop->c.i;
  vpop();
  return c;
}

/* parse an integer constant and return its value.
   Complain if it doesn't fit 32bit (signed or unsigned).  */
ST_FUNC int expr_const(void)
{
  int c;
  int64_t wc = expr_const64();
  c = wc;
  if (c != wc && (unsigned)c != wc)
    tcc_error("constant exceeds 32 bit");
  return c;
}

/* ------------------------------------------------------------------------- */
/* return from function */
#ifndef TCC_TARGET_ARM64
static void gfunc_return(CType *func_type)
{
  if ((func_type->t & VT_BTYPE) == VT_STRUCT)
  {
    CType type, ret_type;
    int ret_align, ret_nregs, regsize;
    ret_nregs = gfunc_sret(func_type, func_var, &ret_type, &ret_align, &regsize);
    if (ret_nregs < 0)
    {
#ifdef TCC_TARGET_RISCV64
      arch_transfer_ret_regs(0);
#endif
    }
    else if (0 == ret_nregs)
    {
      /* if returning structure, must copy it to implicit
         first pointer arg location */
      type = *func_type;
      mk_pointer(&type);
      vset(&type, VT_LOCAL | VT_LVAL, func_vc);
      indir();
      vswap();
      /* copy structure value to pointer */
      vstore();
    }
    else
    {
      /* returning structure packed into registers */
      int size, addr, align, rc, n;
      size = type_size(func_type, &align);
      if ((align & (ret_align - 1)) && ((vtop->r & VT_VALMASK) < VT_CONST /* pointer to struct */
                                        || (vtop->c.i & (ret_align - 1))))
      {
        loc = (loc - size) & -ret_align;
        addr = loc;
        type = *func_type;
        vset(&type, VT_LOCAL | VT_LVAL, addr);
        vswap();
        vstore();
        vpop();
        vset(&ret_type, VT_LOCAL | VT_LVAL, addr);
      }
      vtop->type = ret_type;
      rc = RC_RET(ret_type.t);
      // printf("struct return: n:%d t:%02x rc:%02x\n", ret_nregs, ret_type.t,
      // rc);
      for (n = ret_nregs; --n > 0;)
      {
        vdup();
        gv(rc);
        vswap();
        incr_offset(regsize);
        /* We assume that when a structure is returned in multiple
           registers, their classes are consecutive values of the
           suite s(n) = 2^n */
        rc <<= 1;
      }
      gv(rc);
      vtop -= ret_nregs - 1;
    }
  }
  else
  {
    // function returns scalar value - ensure it's loaded into a value (not lvalue)
    // This generates proper LOAD IR if vtop is still an lvalue
    if (vtop->r & VT_LVAL)
    {
      /* Load the value first - this ensures proper size is used */
      SValue dest;
      svalue_init(&dest);
      dest.type = vtop->type;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      dest.r = 0;
      dest.c.i = 0;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dest);
      vtop->vr = dest.vr;
      vtop->r = 0; /* no longer an lvalue */
    }
    tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_RETURNVALUE, vtop, NULL, NULL);
  }
  vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
  print_vstack("gfunc_return");
}
#endif

static void check_func_return(void)
{
  if ((func_vt.t & VT_BTYPE) == VT_VOID)
    return;
  if (!strcmp(funcname, "main") && (func_vt.t & VT_BTYPE) == VT_INT)
  {
    /* main returns 0 by default */
    vpushi(0);
    gen_assign_cast(&func_vt);
    gfunc_return(&func_vt);
  }
  else
  {
    tcc_warning("function might return no value: '%s'", funcname);
  }
}

/* ------------------------------------------------------------------------- */
/* switch/case */

static int case_cmp(uint64_t a, uint64_t b)
{
  if (cur_switch->sv.type.t & VT_UNSIGNED)
    return a < b ? -1 : a > b;
  else
    return (int64_t)a<(int64_t)b ? -1 : (int64_t)a>(int64_t) b;
  /* unreachable - all branches above return */
  return 0;
}

static int case_cmp_qs(const void *pa, const void *pb)
{
  return case_cmp((*(struct case_t **)pa)->v1, (*(struct case_t **)pb)->v1);
}

static void case_sort(struct switch_t *sw)
{
  struct case_t **p;
  if (sw->n < 2)
    return;
  qsort(sw->p, sw->n, sizeof *sw->p, case_cmp_qs);
  p = sw->p;
  while (p < sw->p + sw->n - 1)
  {
    if (case_cmp(p[0]->v2, p[1]->v1) >= 0)
    {
      int l1 = p[0]->line, l2 = p[1]->line;
      /* using special format "%i:..." to show specific line */
      tcc_error("%i:duplicate case value", l1 > l2 ? l1 : l2);
    }
    else if (p[0]->v2 + 1 == p[1]->v1 && p[0]->ind == p[1]->ind)
    {
      /* treat "case 1: case 2: case 3:" like "case 1 ... 3: */
      p[1]->v1 = p[0]->v1;
      tcc_free(p[0]);
      memmove(p, p + 1, (--sw->n - (p - sw->p)) * sizeof *p);
    }
    else
      ++p;
  }
}

/* ============================================================================
 * Jump Table Switch Optimization
 * ============================================================================
 * For dense switch statements, use a jump table with TBB/TBH instructions
 * instead of linear/binary search for O(1) dispatch.
 */

/* Check if switch is suitable for jump table optimization.
 * Criteria:
 *   - Optimization enabled (-O1 or higher)
 *   - At least 4 cases
 *   - At least 50% density (num_cases / range >= 0.5)
 *   - Range fits in TBH (<= 65535) for TBB/TBH
 *   - No case ranges (v1 == v2 for all cases)
 *   - Not long long type (to simplify initial implementation)
 */
static int switch_can_use_jump_table(struct switch_t *sw)
{
  /* Only use jump tables when optimization is enabled */
  if (!tcc_state->optimize)
    return 0;

  if (sw->n < 4)
    return 0; /* Too few cases to justify overhead */

  int64_t min_val = sw->p[0]->v1;
  int64_t max_val = sw->p[sw->n - 1]->v2;
  int64_t range = max_val - min_val + 1;

  /* Check density: must be at least 50% filled */
  if (sw->n * 2 < range)
    return 0;

  /* Check range fits in TBH (halfword indexing, max 65536 entries) */
  if (range > 65536)
    return 0;

  /* Check for case ranges (v1 != v2) - not supported initially */
  for (int i = 0; i < sw->n; i++)
  {
    if (sw->p[i]->v1 != sw->p[i]->v2)
      return 0;
  }

  /* Check integer type (not long long for simplicity) */
  if ((sw->sv.type.t & VT_BTYPE) == VT_LLONG)
    return 0;

  return 1;
}

/* Allocate and populate a switch table for jump table generation.
 * Returns the table_id to be used with TCCIR_OP_SWITCH_TABLE.
 */
static int tcc_ir_add_switch_table(TCCIRState *ir, int64_t min_val, int64_t max_val, int default_target,
                                   struct switch_t *sw)
{
  /* Grow array if needed */
  if (ir->num_switch_tables >= ir->switch_tables_capacity)
  {
    ir->switch_tables_capacity = ir->switch_tables_capacity * 2 + 4;
    ir->switch_tables = tcc_realloc(ir->switch_tables, ir->switch_tables_capacity * sizeof(*ir->switch_tables));
  }

  int id = ir->num_switch_tables++;
  TCCIRSwitchTable *table = &ir->switch_tables[id];

  table->min_val = min_val;
  table->max_val = max_val;
  table->default_target = default_target;
  table->num_entries = (int)(max_val - min_val + 1);
  table->targets = tcc_mallocz(table->num_entries * sizeof(int));
  table->table_code_addr = 0;

  /* Fill with default target initially */
  for (int i = 0; i < table->num_entries; i++)
  {
    table->targets[i] = default_target;
  }

  /* Fill in actual case targets */
  for (int i = 0; i < sw->n; i++)
  {
    int idx = (int)(sw->p[i]->v1 - min_val);
    if (idx >= 0 && idx < table->num_entries)
      table->targets[idx] = sw->p[i]->ind;
  }

  return id;
}

/* Generate jump table for switch statement.
 * Emits:
 *   1. Bounds check: if (index - min > max-min) goto default
 *   2. SWITCH_TABLE instruction with table reference
 *
 * Note: Like gcase(), this function does NOT pop the switch value from vtop.
 * The caller is responsible for vpop() after gcase_jump_table returns.
 */
static int gcase_jump_table(struct switch_t *sw, int dsym)
{
  int64_t min_val = sw->p[0]->v1;
  int64_t max_val = sw->p[sw->n - 1]->v2;
  int range = (int)(max_val - min_val);
  TCCIRState *ir = tcc_state->ir;

  /* We need to preserve the original switch value on vtop for the caller.
   * So we work on a duplicated copy. */

  /* Duplicate the switch value for our manipulation */
  vdup();

  /* Adjust index: index = index - min_val (if min_val != 0) */
  if (min_val != 0)
  {
    vpush64(VT_INT, min_val);
    gen_op('-');
  }

  /* Duplicate adjusted index for bounds check */
  vdup();

  /* Compare: if (index > range) goto default
   * Use unsigned comparison since we just subtracted min */
  vpush64(VT_INT, range);
  gen_op(TOK_UGT); /* Unsigned greater than */

  /* Jump to default if out of bounds */
  int bounds_fail = tcc_ir_codegen_test_gen(ir, 0, dsym);

  /* Allocate switch table */
  int table_id = tcc_ir_add_switch_table(ir, min_val, max_val, dsym, sw);

  /* Emit SWITCH_TABLE instruction.
   * vtop currently holds the adjusted index (0 to range).
   * We'll use src2 to store the table_id. */
  SValue table_ref;
  svalue_init(&table_ref);
  table_ref.r = VT_CONST;
  table_ref.c.i = table_id;
  table_ref.type.t = VT_INT;

  /* src1 = adjusted index (current vtop)
   * src2 = table_id (encoded in an SValue)
   * The backend will handle the actual table emission */
  tcc_ir_put(ir, TCCIR_OP_SWITCH_TABLE, vtop, &table_ref, NULL);

  /* Pop our working copy of the adjusted index.
   * The original switch value remains on the stack below. */
  vpop();

  return bounds_fail; /* Return the jump for potential further use */
}

/* dsym is a jump-chain head (index of a JMP instruction) that will ultimately
 * be patched to the default label or fall-through. Never pass raw -1 here. */
static int gcase(struct case_t **base, int len, int dsym)
{
  struct case_t *p;
  SValue dest;
  int t, l2, e;

  t = vtop->type.t & VT_BTYPE;
  if (t != VT_LLONG)
    t = VT_INT;
  while (len)
  {
    /* binary search while len > 8, else linear */
    l2 = len > 8 ? len / 2 : 0;
    p = base[l2];
    vdup(), vpush64(t, p->v2);
    if (l2 == 0 && p->v1 == p->v2)
    {
      int pos = 0;
      gen_op(TOK_EQ); /* jmp to case when equal */
      /* Use -1 (not dsym) as target to avoid corrupting the default chain.
       * tcc_ir_backpatch() follows the jump chain from the target, so passing
       * dsym here would cause it to walk the entire default chain and patch
       * every entry to p->ind, destroying the chain for subsequent cases.
       * With -1, the JUMPIF is independent: on match it is backpatched to
       * p->ind; on mismatch execution falls through to the next case check
       * (or the final JUMP(dsym) at the end of the loop). */
      pos = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
      tcc_ir_backpatch(tcc_state->ir, pos, p->ind);
      // gsym_addr(gvtst(0, 0), p->ind);
    }
    else
    {
      int pos = 0;
      /* case v1 ... v2 */
      gen_op(TOK_GT); /* jmp over when > V2 */
      if (len == 1)   /* last case test jumps to default when false */
      {
        dsym = tcc_ir_codegen_test_gen(tcc_state->ir, 0, dsym);
        e = -1; /* Use -1 so tcc_ir_backpatch_to_here will be a no-op */
      }
      else
      {
        /* Use -1 (not dsym) as target to avoid corrupting the default chain.
         * The e jump will be backpatched independently to fall through.
         * Using -1 ensures backpatching stops at e and doesn't follow any chain. */
        e = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
      }
      vdup(), vpush64(t, p->v1);
      gen_op(TOK_GE); /* jmp to case when >= V1 */
      pos = tcc_ir_codegen_test_gen(tcc_state->ir, 0, p->ind);
      tcc_ir_backpatch(tcc_state->ir, pos, p->ind);
      // gsym_addr(gvtst(0, 0), p->ind);
      dsym = gcase(base, l2, dsym);
      // gsym(e);s
      tcc_ir_backpatch_to_here(tcc_state->ir, e);
    }
    ++l2, base += l2, len -= l2;
  }
  /* jump automagically will suppress more jumps */
  // return gjmp(dsym);
  svalue_init(&dest);
  dest.vr = -1;
  dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
  dest.c.i = dsym;
  return tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
}

static void end_switch(void)
{
  struct switch_t *sw = cur_switch;
  dynarray_reset(&sw->p, &sw->n);
  cur_switch = sw->prev;
  tcc_free(sw);
}

/* ------------------------------------------------------------------------- */
/* __attribute__((cleanup(fn))) */

static void try_call_scope_cleanup(Sym *stop)
{
  Sym *cls = cur_scope->cl.s;

  /* Cleanups must still be emitted in CODE_OFF regions (unreachable by fallthrough)
   * because forward gotos can jump to cleanup landing pads.
   * Still suppress in true no-eval/const-expression contexts.
   */
  if (nocode_wanted & ~CODE_OFF_BIT)
    return;

  for (; cls != stop; cls = cls->next)
  {
    Sym *fs = cls->cleanup_func;
    Sym *vs = cls->prev_tok;

    vpushsym(&fs->type, fs);
    vset(&vs->type, vs->r, vs->c);
    vtop->sym = vs;
    vtop->vr = vs->vreg; /* Set vreg so gaddrof() can compute correct address */
    mk_pointer(&vtop->type);
    gaddrof();
    // gfunc_call(1);
    SValue src1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    svalue_init(&src1);
    src1.vr = -1;
    src1.r = VT_CONST;
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=scope_cleanup call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n",
                 call_id, TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop->r, vtop->vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, vtop, &src1, NULL);
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 1);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[-1], &call_id_sv, NULL);
    vtop -= 2;
  }
}

static void try_call_cleanup_goto(Sym *cleanupstate)
{
  Sym *oc, *cc;
  int ocd, ccd;

  if (!cur_scope->cl.s)
    return;

  /* search NCA of both cleanup chains given parents and initial depth */
  ocd = cleanupstate ? cleanupstate->v & ~SYM_FIELD : 0;
  for (ccd = cur_scope->cl.n, oc = cleanupstate; ocd > ccd; --ocd, oc = oc->next)
    ;
  for (cc = cur_scope->cl.s; ccd > ocd; --ccd, cc = cc->next)
    ;
  for (; cc != oc; cc = cc->next, oc = oc->next, --ccd)
    ;

  try_call_scope_cleanup(cc);
}

/* call 'func' for each __attribute__((cleanup(func))) */
static void block_cleanup(struct scope *o)
{
  int jmp = -1; /* -1 = no pending jump */
  Sym *g, **pg;
  for (pg = &pending_gotos; (g = *pg) && g->c > o->cl.n;)
  {
    if (g->prev_tok->r & LABEL_FORWARD)
    {
      Sym *pcl = g->next;
      if (jmp < 0)
        jmp = gjmp(-1); /* -1 = no chain */
      tcc_ir_backpatch_to_here(tcc_state->ir, pcl->jnext);
      try_call_scope_cleanup(o->cl.s);
      pcl->jnext = gjmp(-1); /* -1 = no chain */
      if (!o->cl.n)
        goto remove_pending;
      g->c = o->cl.n;
      pg = &g->prev;
    }
    else
    {
    remove_pending:
      *pg = g->prev;
      sym_free(g);
    }
  }
  tcc_ir_backpatch_to_here(tcc_state->ir, jmp);
  try_call_scope_cleanup(o->cl.s);
}

/* ------------------------------------------------------------------------- */
/* VLA */

static void vla_restore(int loc)
{
  if (!loc)
    return;

  if (tcc_state->ir)
  {
    SValue src;
    memset(&src, 0, sizeof(src));
    src.type.t = VT_PTR;
    src.r = VT_LOCAL | VT_LVAL;
    src.c.i = loc;
    src.vr = -1;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_SP_RESTORE, &src, NULL, NULL);
  }
  else
  {
    gen_vla_sp_restore(loc);
  }
}

static void vla_leave(struct scope *o)
{
  struct scope *c = cur_scope, *v = NULL;
  for (; c != o && c; c = c->prev)
    if (c->vla.num)
      v = c;
  if (v)
    vla_restore(v->vla.locorig);
}
/* ------------------------------------------------------------------------- */
/* local scopes */

static void new_scope(struct scope *o)
{
  /* copy and link previous scope */
  *o = *cur_scope;
  o->prev = cur_scope;
  cur_scope = o;
  /* Reset VLA bookkeeping for the new scope. The scope struct is copied from
   * the parent, so we must clear these fields or we'll restore SP using the
   * parent's slots. */
  cur_scope->vla.num = 0;
  cur_scope->vla.loc = 0;
  cur_scope->vla.locorig = 0;
  /* NOTE: We no longer unconditionally save SP for every scope. A pre-VLA SP
   * save slot is allocated lazily only if/when the first VLA is declared in
   * this scope. */
  /* record local declaration stack position */
  o->lstk = local_stack;
  o->llstk = local_label_stack;
  ++local_scope;
}

static void prev_scope(struct scope *o, int is_expr)
{
  vla_leave(o->prev);

  if (o->cl.s != o->prev->cl.s)
    block_cleanup(o->prev);

  /* pop locally defined labels */
  label_pop(&local_label_stack, o->llstk, is_expr);

  /* In the is_expr case (a statement expression is finished here),
     vtop might refer to symbols on the local_stack.  Either via the
     type or via vtop->sym.  We can't pop those nor any that in turn
     might be referred to.  To make it easier we don't roll back
     any symbols in that case; some upper level call to block() will
     do that.  We do have to remove such symbols from the lookup
     tables, though.  sym_pop will do that.  */

  /* pop locally defined symbols */
  pop_local_syms(o->lstk, is_expr);
  cur_scope = o->prev;
  --local_scope;
}

/* leave a scope via break/continue(/goto) */
static void leave_scope(struct scope *o)
{
  if (!o)
    return;
  try_call_scope_cleanup(o->cl.s);
  vla_leave(o);
}

/* short versiona for scopes with 'if/do/while/switch' which can
   declare only types (of struct/union/enum) */
static void new_scope_s(struct scope *o)
{
  o->lstk = local_stack;
  ++local_scope;
}

static void prev_scope_s(struct scope *o)
{
  sym_pop(&local_stack, o->lstk, 0);
  --local_scope;
}

/* ------------------------------------------------------------------------- */
/* call block from 'for do while' loops */

static void lblock(int *bsym, int *csym)
{
  struct scope *lo = loop_scope, *co = cur_scope;
  int *b = co->bsym, *c = co->csym;
  if (csym)
  {
    co->csym = csym;
    loop_scope = co;
  }
  co->bsym = bsym;
  block(0);
  co->bsym = b;
  if (csym)
  {
    co->csym = c;
    loop_scope = lo;
  }
}

static void block(int flags)
{
  int a, b, c, d, e, t;
  struct scope o;
  Sym *s;

  if (flags & STMT_EXPR)
  {
    /* default return value is (void) */
    vpushi(0);
    vtop->type.t = VT_VOID;
  }

again:
  t = tok;
  /* If the token carries a value, next() might destroy it. Only with
     invalid code such as f(){"123"4;} */
  if (TOK_HAS_VALUE(t))
    goto expr;
  next();

  if (debug_modes)
    tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_begin(tcc_state);

  if (t == TOK_IF)
  {
    new_scope_s(&o);
    skip('(');
    gexpr();
    skip(')');
    a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    block(0);
    if (tok == TOK_ELSE)
    {
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = -1;     /* Will be patched to end of else block */
      d = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(tcc_state->ir, a);
      CODE_ON(); /* Code after if-branch is reachable via else path */
      next();
      block(0);
      tcc_ir_backpatch_to_here(tcc_state->ir, d);
      CODE_ON(); /* Code after if-else is reachable from both paths */
    }
    else
    {
      tcc_ir_backpatch_to_here(tcc_state->ir, a);
      CODE_ON(); /* Code after if is reachable when condition is false */
    }
    prev_scope_s(&o);
  }
  else if (t == TOK_WHILE)
  {
    SValue dest;
    new_scope_s(&o);
    d = gind();
    skip('(');
    gexpr();
    skip(')');
    // fprintf(stderr, "WHILE_COND: file=%s line=%d r=0x%x type=0x%x vr=%d VT_LVAL=%d VT_VALMASK=0x%x btype=0x%x\n",
    //         file->filename, file->line_num, vtop->r, vtop->type.t, vtop->vr, (vtop->r & VT_LVAL) ? 1 : 0,
    //         vtop->r & VT_VALMASK, vtop->type.t & VT_BTYPE);
    // a = gvtst(1, 0);
    a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    b = -1; /* Initialize continue chain with -1 sentinel */
    lblock(&a, &b);
    // gjmp_addr(d);
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = d;
    d = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // gsym_addr(b, d);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    tcc_ir_backpatch(tcc_state->ir, b, d);
    // gsym(a);
    prev_scope_s(&o);
  }
  else if (t == '{')
  {
    if (debug_modes)
      tcc_debug_stabn(tcc_state, N_LBRAC, ind - func_ind);
    new_scope(&o);

    /* handle local labels declarations */
    while (tok == TOK_LABEL)
    {
      do
      {
        next();
        if (tok < TOK_UIDENT)
          expect("label identifier");
        label_push(&local_label_stack, tok, LABEL_DECLARED);
        next();
      } while (tok == ',');
      skip(';');
    }

    while (tok != '}')
    {
      decl(VT_LOCAL);
      if (tok != '}')
      {
        if (flags & STMT_EXPR)
          vpop();
        block(flags | STMT_COMPOUND);
      }
    }

    prev_scope(&o, flags & STMT_EXPR);
    if (debug_modes)
      tcc_debug_stabn(tcc_state, N_RBRAC, ind - func_ind);
    if (local_scope)
      next();
    else
    {
      /* For main(), always generate return 0 even if nocode_wanted is set
       * (which can happen due to control flow analysis after if/else etc.) */
      if (nocode_wanted && !strcmp(funcname, "main") && (func_vt.t & VT_BTYPE) == VT_INT)
        CODE_ON();
      if (!nocode_wanted)
        check_func_return();
    }
  }
  else if (t == TOK_RETURN)
  {
    b = (func_vt.t & VT_BTYPE) != VT_VOID;
    if (tok != ';')
    {
      gexpr();
      if (b)
      {
        gen_assign_cast(&func_vt);
      }
      else
      {
        if (vtop->type.t != VT_VOID)
          tcc_warning("void function returns a value");
        vtop--;
        print_vstack("block(1)");
      }
    }
    else if (b)
    {
      tcc_warning("'return' with no value");
      b = 0;
    }
    leave_scope(root_scope);
    if (b)
      gfunc_return(&func_vt);
    skip(';');
    /* jump unless last stmt in top-level block */
    if (tok != '}' || local_scope != 1)
    {
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = rsym;   /* Chain return jumps: point to previous rsym */
      rsym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      // rsym = gjmp(rsym);
    }
    if (debug_modes)
      tcc_tcov_block_end(tcc_state, -1);
    CODE_OFF();
  }
  else if (t == TOK_BREAK)
  {
    /* compute jump */
    SValue dest;
    if (!cur_scope->bsym)
      tcc_error("cannot break");
    if (cur_switch && cur_scope->bsym == cur_switch->bsym)
      leave_scope(cur_switch->scope);
    else
      leave_scope(loop_scope);
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = *cur_scope->bsym;
    *cur_scope->bsym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // *cur_scope->bsym = gjmp(*cur_scope->bsym);
    skip(';');
  }
  else if (t == TOK_CONTINUE)
  {
    /* compute jump */
    SValue dest;
    if (!cur_scope->csym)
      tcc_error("cannot continue");
    leave_scope(loop_scope);
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = *cur_scope->csym;
    // *cur_scope->csym = gjmp(*cur_scope->csym);
    *cur_scope->csym = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    skip(';');
  }
  else if (t == TOK_FOR)
  {
    int saved_line_num;
    new_scope(&o);

    skip('(');
    if (tok != ';')
    {
      /* c99 for-loop init decl? */
      if (!decl(VT_JMP))
      {
        /* no, regular for-loop init expr */
        gexpr();
        vpop();
      }
    }
    skip(';');
    a = b = -1; /* Initialize break/continue chains with -1 sentinel */
    c = d = tcc_state->ir->next_instruction_index;
    if (tok != ';')
    {
      gexpr();
      a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    }
    skip(';');
    if (tok != ')')
    {
      // e = gjmp(0);
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = -1;
      e = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      // d = gind();
      c = tcc_state->ir->next_instruction_index;
      gexpr();
      vpop();
      // gjmp_addr(c);
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = d;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(tcc_state->ir, e);
      // gsym(e);
    }
    skip(')');
    /* Save line number before loop body for backward jump */
    saved_line_num = file->line_num;
    lblock(&a, &b);
    // gjmp_addr(d);
    SValue dest;
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = c;
    /* Temporarily restore line number for backward jump instruction */
    {
      int cur_line = file->line_num;
      file->line_num = saved_line_num;
      d = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      file->line_num = cur_line;
    }
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    tcc_ir_backpatch(tcc_state->ir, b, c);
    // gsym_addr(b, d);
    // gsym(a);
    prev_scope(&o, 0);
  }
  else if (t == TOK_DO)
  {
    new_scope_s(&o);
    a = b = -1; /* Initialize break/continue chains with -1 sentinel */
    d = gind();
    lblock(&a, &b);
    /* continue jumps land at the condition check of the do/while */
    tcc_ir_backpatch_to_here(tcc_state->ir, b);
    skip(TOK_WHILE);
    skip('(');
    gexpr();
    skip(')');
    skip(';');
    // c = gvtst(0, 0);
    c = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);

    // gsym_addr(c, d);
    tcc_ir_backpatch(tcc_state->ir, c, d);
    // gsym(a);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    prev_scope_s(&o);
  }
  else if (t == TOK_SWITCH)
  {
    struct switch_t *sw;
    SValue dest;

    sw = tcc_mallocz(sizeof *sw);
    sw->bsym = &a;
    sw->scope = cur_scope;
    sw->prev = cur_switch;
    sw->nocode_wanted = nocode_wanted;
    cur_switch = sw;

    new_scope_s(&o);
    skip('(');
    gexpr();
    skip(')');
    if (!is_integer_btype(vtop->type.t & VT_BTYPE))
      tcc_error("switch value not an integer");
    sw->sv = *vtop--; /* save switch value */
    print_vstack("block(2)");
    a = -1; /* Initialize break chain with -1 sentinel */
    svalue_init(&dest);
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = -1;     /* Initial jump target, will be patched */
    b = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // b = gjmp(0); /* jump to first case */
    lblock(&a, NULL);
    /* If the switch has a default label, no explicit breaks were emitted
     * (a == -1), and the last case ends with dead code (return/goto/continue),
     * then ALL paths through the switch exit without reaching code after it.
     * Must be checked before the implicit break overwrites 'a'. */
    int switch_exits_all = sw->def_sym && (a == -1) && (nocode_wanted & CODE_OFF_BIT);
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = a;
    a = tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    // a = gjmp(a); /* add implicit break */
    /* case lookup */
    // gsym(b);

    prev_scope_s(&o);
    if (sw->nocode_wanted)
      goto skip_switch;
    case_sort(sw);
    sw->bsym = NULL; /* marker for 32bit:gen_opl() */
    vpushv(&sw->sv);
    // gv(RC_INT);
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    /* The switch value is copied into a temporary vreg used by the case
      comparison chain. Preserve the original type so the IR can tag the vreg
      correctly (notably VT_LLONG needs 8-byte spill slots). */
    dest.type = vtop->type;
    c = tcc_state->ir->next_instruction_index; /* save start of case comparisons */
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    /* Build case jump chain; start with empty default chain (-1).
     * Use jump table for dense switches, otherwise fall back to binary search. */
    if (switch_can_use_jump_table(sw))
    {
      d = gcase_jump_table(sw, -1);
    }
    else
    {
      d = gcase(sw->p, sw->n, -1);
    }
    vpop();

    tcc_ir_backpatch(tcc_state->ir, b, c);
    if (sw->def_sym)
      tcc_ir_backpatch(tcc_state->ir, d, sw->def_sym);
    else
      tcc_ir_backpatch_to_here(tcc_state->ir, d);
    // gsym(d);
  skip_switch:
    /* break label */
    // gsym(a);
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    /* If every path through the switch exits (has default, no breaks, last
     * case is dead code), code after the switch is unreachable. Restore
     * CODE_OFF so that check_func_return() is not triggered spuriously. */
    if (switch_exits_all)
      CODE_OFF();
    end_switch();
  }
  else if (t == TOK_CASE)
  {
    struct case_t *cr;
    if (!cur_switch)
      expect("switch");
    cr = tcc_malloc(sizeof(struct case_t));
    dynarray_add(&cur_switch->p, &cur_switch->n, cr);
    t = cur_switch->sv.type.t;
    cr->v1 = cr->v2 = value64(expr_const64(), t);
    if (tok == TOK_DOTS && gnu_ext)
    {
      next();
      cr->v2 = value64(expr_const64(), t);
      if (case_cmp(cr->v2, cr->v1) < 0)
        tcc_warning("empty case range");
    }
    /* case and default are unreachable from a switch under nocode_wanted */
    if (!cur_switch->nocode_wanted)
      cr->ind = gind();
    cr->line = file->line_num;
    skip(':');
    goto block_after_label;
  }
  else if (t == TOK_DEFAULT)
  {
    if (!cur_switch)
      expect("switch");
    if (cur_switch->def_sym)
      tcc_error("too many 'default'");
    cur_switch->def_sym = cur_switch->nocode_wanted ? -1 : gind();
    skip(':');
    goto block_after_label;
  }
  else if (t == TOK_GOTO)
  {
    vla_restore(cur_scope->vla.locorig);
    if (tok == '*' && gnu_ext)
    {
      /* computed goto */
      next();
      gexpr();
      if ((vtop->type.t & VT_BTYPE) != VT_PTR)
        expect("pointer");
      ggoto();
    }
    else if (tok >= TOK_UIDENT)
    {
      s = label_find(tok);
      /* put forward definition if needed */
      if (!s)
        s = label_push(&global_label_stack, tok, LABEL_FORWARD);
      else if (s->r == LABEL_DECLARED)
        s->r = LABEL_FORWARD;

      if (s->r & LABEL_FORWARD)
      {
        /* start new goto chain for cleanups, linked via label->next */
        if (cur_scope->cl.s && !nocode_wanted)
        {
          sym_push2(&pending_gotos, SYM_FIELD, 0, cur_scope->cl.n);
          pending_gotos->prev_tok = s;
          s = sym_push2(&s->next, SYM_FIELD, 0, 0);
          pending_gotos->next = s;
        }
        s->jnext = gjmp(s->jnext);
      }
      else
      {
        SValue dest;
        svalue_init(&dest);
        try_call_cleanup_goto(s->cleanupstate);
        dest.vr = -1;
        dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
        dest.c.i = s->jind;
        // gjmp_addr(s->jind);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      }
      next();
    }
    else
    {
      expect("label identifier");
    }
    skip(';');
  }
  else if (t == TOK_ASM1 || t == TOK_ASM2 || t == TOK_ASM3)
  {
    asm_instr();
  }
  else
  {
    if (tok == ':' && t >= TOK_UIDENT)
    {
      /* label case */
      next();
      s = label_find(t);
      if (s)
      {
        if (s->r == LABEL_DEFINED)
          tcc_error("duplicate label '%s'", get_tok_str(s->v, NULL));
        s->r = LABEL_DEFINED;
        if (s->next)
        {
          Sym *pcl; /* pending cleanup goto */
          for (pcl = s->next; pcl; pcl = pcl->prev)
            if (pcl->jnext >= 0) /* Only backpatch if there's an actual forward jump */
              tcc_ir_backpatch_to_here(tcc_state->ir, pcl->jnext);
          sym_pop(&s->next, NULL, 0);
        }
        else if (s->jnext >= 0) /* Only backpatch if there's an actual forward jump */
          tcc_ir_backpatch_to_here(tcc_state->ir, s->jnext);
      }
      else
      {
        s = label_push(&global_label_stack, t, LABEL_DEFINED);
      }
      s->jind = gind();
      s->cleanupstate = cur_scope->cl.s;

    block_after_label:
    {
      /* Accept attributes after labels (e.g. 'unused') */
      AttributeDef ad_tmp;
      parse_attribute(&ad_tmp);
    }
      if (debug_modes)
        tcc_tcov_reset_ind(tcc_state);
      vla_restore(cur_scope->vla.locorig);

      if (tok != '}')
      {
        if (0 == (flags & STMT_COMPOUND))
          goto again;
        /* C23: insert implicit null-statement whithin compound statement */
      }
      else
      {
        /* we accept this, but it is a mistake */
        tcc_warning_c(warn_all)("deprecated use of label at end of compound statement");
      }
    }
    else
    {
      /* expression case */
      if (t != ';')
      {
        unget_tok(t);
      expr:
        if (flags & STMT_EXPR)
        {
          vpop();
          gexpr();
        }
        else
        {
          gexpr();
          tcc_ir_codegen_drop_return(tcc_state->ir);
          vpop();
        }
        skip(';');
      }
    }
  }

  if (debug_modes)
    tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_end(tcc_state, 0);
}

/* This skips over a stream of tokens containing balanced {} and ()
   pairs, stopping at outer ',' ';' and '}' (or matching '}' if we started
   with a '{').  If STR then allocates and stores the skipped tokens
   in *STR.  This doesn't check if () and {} are nested correctly,
   i.e. "({)}" is accepted.  */
static void skip_or_save_block(TokenString **str)
{
  int braces = tok == '{';
  int level = 0;
  if (str)
    *str = tok_str_alloc();

  while (1)
  {
    int t = tok;
    if (level == 0 && (t == ',' || t == ';' || t == '}' || t == ')' || t == ']'))
      break;
    if (t == TOK_EOF)
    {
      if (str || level > 0)
        tcc_error("unexpected end of file");
      else
        break;
    }
    if (str)
      tok_str_add_tok(*str);
    next();
    if (t == '{' || t == '(' || t == '[')
    {
      level++;
    }
    else if (t == '}' || t == ')' || t == ']')
    {
      level--;
      if (level == 0 && braces && t == '}')
        break;
    }
  }
  if (str)
    tok_str_add(*str, TOK_EOF);
}

#define EXPR_CONST 1
#define EXPR_ANY 2

static void parse_init_elem(int expr_type)
{
  int saved_global_expr;
  switch (expr_type)
  {
  case EXPR_CONST:
    /* compound literals must be allocated globally in this case */
    saved_global_expr = global_expr;
    global_expr = 1;
    expr_const1();
    global_expr = saved_global_expr;
    /* NOTE: symbols are accepted, as well as lvalue for anon symbols
       (compound literals).  */
    if (((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST &&
         ((vtop->r & (VT_SYM | VT_LVAL)) != (VT_SYM | VT_LVAL) || vtop->sym->v < SYM_FIRST_ANOM))
#ifdef TCC_TARGET_PE
        || ((vtop->r & VT_SYM) && vtop->sym->a.dllimport)
#endif
    )
      tcc_error("initializer element is not constant");
    break;
  case EXPR_ANY:
    expr_eq();
    break;
  }
}

#if 1
static void init_assert(init_params *p, int offset)
{
  if (p->sec ? !NODATA_WANTED && offset > p->sec->data_offset : !nocode_wanted && offset > p->local_offset)
    tcc_internal_error("initializer overflow");
}
#else
#define init_assert(sec, offset)
#endif

/* put zeros for variable based init */
static void init_putz(init_params *p, unsigned long c, int size)
{
  init_assert(p, c + size);
  if (p->sec)
  {
    /* nothing to do because globals are already set to zero */
  }
  else
  {
    SValue src1;
    SValue dest;

    vseti(VT_LOCAL, c);
    vpushi(0);
    vpushs(size);

    svalue_init(&src1);
    src1.vr = -1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    /* __aeabi_memset(dest, n, c) on ARM EABI; memset(dest, c, n) elsewhere.
     * TOK_memset maps to __aeabi_memset when TCC_ARM_EABI is defined.
     * Stack is: dest, c, n */
    src1.r = VT_CONST;
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n", call_id,
                 TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-2].r, vtop[-2].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
    TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n", call_id,
                 TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-1].r, vtop[-1].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    TCCGEN_DEBUG("[TCCGEN] FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d\n", call_id,
                 TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[0].r, vtop[0].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &src1, NULL);

    vpush_helper_func(TOK_memset);
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.type.t = vtop[-3].type.t;
    dest.r = 0;
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, &dest);
    vtop -= 4;

    // vtop -= 4;
    // vtop->r = 0;
    // vtop->vr = dest.vr;
    // vtop->r = 0;
    // vtop->vr = dest.vr;

#if defined(TCC_TARGET_ARM) && defined TCC_ARM_EABI
    // vswap(); /* using __aeabi_memset(void*, size_t, int) */
#endif
    // gfunc_call(3);
  }
}

#define DIF_FIRST 1
#define DIF_SIZE_ONLY 2
#define DIF_HAVE_ELEM 4
#define DIF_CLEAR 8

/* delete relocations for specified range c ... c + size. Unfortunatly
   in very special cases, relocations may occur unordered */
static void decl_design_delrels(Section *sec, int c, int size)
{
  ElfW_Rel *rel, *rel2, *rel_end;
  if (!sec || !sec->reloc)
    return;
  rel = rel2 = (ElfW_Rel *)sec->reloc->data;
  rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
  while (rel < rel_end)
  {
    if (rel->r_offset >= c && rel->r_offset < c + size)
    {
      sec->reloc->data_offset -= sizeof *rel;
    }
    else
    {
      if (rel2 != rel)
        memcpy(rel2, rel, sizeof *rel);
      ++rel2;
    }
    ++rel;
  }
}

static void decl_design_flex(init_params *p, Sym *ref, int index)
{
  if (ref == p->flex_array_ref)
  {
    if (index >= ref->c)
      ref->c = index + 1;
  }
  else if (ref->c < 0)
    tcc_error("flexible array has zero size in this context");
}

/* t is the array or struct type. c is the array or struct
   address. cur_field is the pointer to the current
   field, for arrays the 'c' member contains the current start
   index.  'flags' is as in decl_initializer.
   'al' contains the already initialized length of the
   current container (starting at c).  This returns the new length of that.  */
static int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags, int al)
{
  Sym *s, *f;
  int index, index_last, align, l, nb_elems, elem_size;
  unsigned long corig = c;

  elem_size = 0;
  nb_elems = 1;

  if (flags & DIF_HAVE_ELEM)
    goto no_designator;

  if (gnu_ext && tok >= TOK_UIDENT)
  {
    l = tok, next();
    if (tok == ':')
      goto struct_field;
    unget_tok(l);
  }

  /* NOTE: we only support ranges for last designator */
  while (nb_elems == 1 && (tok == '[' || tok == '.'))
  {
    if (tok == '[')
    {
      if (!(type->t & VT_ARRAY))
        expect("array type");
      next();
      index = index_last = expr_const();
      if (tok == TOK_DOTS && gnu_ext)
      {
        next();
        index_last = expr_const();
      }
      skip(']');
      s = type->ref;
      decl_design_flex(p, s, index_last);
      if (index < 0 || index_last >= s->c || index_last < index)
        tcc_error("index exceeds array bounds or range is empty");
      if (cur_field)
        (*cur_field)->c = index_last;
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
      nb_elems = index_last - index + 1;
    }
    else
    {
      int cumofs;
      next();
      l = tok;
    struct_field:
      next();
      f = find_field(type, l, &cumofs);
      if (cur_field)
        *cur_field = f;
      type = &f->type;
      c += cumofs;
    }
    cur_field = NULL;
  }
  if (!cur_field)
  {
    if (tok == '=')
    {
      next();
    }
    else if (!gnu_ext)
    {
      expect("=");
    }
  }
  else
  {
  no_designator:
    if (type->t & VT_ARRAY)
    {
      index = (*cur_field)->c;
      s = type->ref;
      decl_design_flex(p, s, index);
      if (index >= s->c)
        tcc_error("too many initializers");
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
    }
    else
    {
      f = *cur_field;
      /* Skip bitfield padding. Also with size 32 and 64. */
      while (f && (f->v & SYM_FIRST_ANOM) && is_integer_btype(f->type.t & VT_BTYPE))
        *cur_field = f = f->next;
      if (!f)
        tcc_error("too many initializers");
      type = &f->type;
      c += f->c;
    }
  }

  if (!elem_size) /* for structs */
    elem_size = type_size(type, &align);

  /* Using designators the same element can be initialized more
     than once.  In that case we need to delete possibly already
     existing relocations. */
  if (!(flags & DIF_SIZE_ONLY) && c - corig < al)
  {
    decl_design_delrels(p->sec, c, elem_size * nb_elems);
    flags &= ~DIF_CLEAR; /* mark stack dirty too */
  }

  decl_initializer(p, type, c, flags & ~DIF_FIRST, -1);

  if (!(flags & DIF_SIZE_ONLY) && nb_elems > 1)
  {
    Sym aref = {0};
    CType t1;
    int i;
    if (p->sec || (type->t & VT_ARRAY))
    {
      /* make init_putv/vstore believe it were a struct */
      aref.c = elem_size;
      t1.t = VT_STRUCT, t1.ref = &aref;
      type = &t1;
    }
    if (p->sec)
    {
      vpush_ref(type, p->sec, c, elem_size);
      for (i = 1; i < nb_elems; i++)
      {
        vdup();
        init_putv(p, type, c + elem_size * i, -1);
      }
      vpop();
    }
    else
    {
      /* Local range designators: copy the first element's value into each
         subsequent slot using vstore, so stack-relative addressing stays
         correct. */
      for (i = 1; i < nb_elems; i++)
      {
        vset(type, VT_LOCAL | VT_LVAL, c + elem_size * i); /* dest */
        vset(type, VT_LOCAL | VT_LVAL, c);                 /* src */
        vstore();
        vpop(); /* drop dest/result left by vstore */
      }
    }
  }

  c += nb_elems * elem_size;
  if (c - corig > al)
    al = c - corig;
  return al;
}

/* store a value or an expression directly in global data or in local array */
static void init_putv(init_params *p, CType *type, unsigned long c, int vreg)
{
  int bt;
  void *ptr;
  CType dtype;
  int size, align;
  Section *sec = p->sec;
  uint64_t val;

  dtype = *type;
  dtype.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */

  size = type_size(type, &align);
  if (type->t & VT_BITFIELD)
    size = (BIT_POS(type->t) + BIT_SIZE(type->t) + 7) / 8;
  init_assert(p, c + size);

  if (sec)
  {
    /* XXX: not portable */
    /* XXX: generate error if incorrect relocation */
    gen_assign_cast(&dtype);
    bt = type->t & VT_BTYPE;

    if ((vtop->r & VT_SYM) && bt != VT_PTR && (bt != (PTR_SIZE == 8 ? VT_LLONG : VT_INT) || (type->t & VT_BITFIELD)) &&
        !((vtop->r & VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM))
      tcc_error("initializer element is not computable at load time");

    if (NODATA_WANTED)
    {
      vtop--;
      print_vstack("init_putv");
      return;
    }

    ptr = sec->data + c;
    val = vtop->c.i;

    /* XXX: make code faster ? */
    if ((vtop->r & (VT_SYM | VT_CONST)) == (VT_SYM | VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM &&
        /* XXX This rejects compound literals like
           '(void *){ptr}'.  The problem is that '&sym' is
           represented the same way, which would be ruled out
           by the SYM_FIRST_ANOM check above, but also '"string"'
           in 'char *p = "string"' is represented the same
           with the type being VT_PTR and the symbol being an
           anonymous one.  That is, there's no difference in vtop
           between '(void *){x}' and '&(void *){x}'.  Ignore
           pointer typed entities here.  Hopefully no real code
           will ever use compound literals with scalar type.  */
        (vtop->type.t & VT_BTYPE) != VT_PTR)
    {
      /* These come from compound literals, memcpy stuff over.  */
      Section *ssec;
      ElfSym *esym;
      ElfW_Rel *rel;
      esym = elfsym(vtop->sym);
      ssec = tcc_state->sections[esym->st_shndx];
      memmove(ptr, ssec->data + esym->st_value + (int)vtop->c.i, size);
      if (ssec->reloc)
      {
        /* We need to copy over all memory contents, and that
           includes relocations.  Use the fact that relocs are
           created it order, so look from the end of relocs
           until we hit one before the copied region.  */
        unsigned long relofs = ssec->reloc->data_offset;
        while (relofs >= sizeof(*rel))
        {
          relofs -= sizeof(*rel);
          rel = (ElfW_Rel *)(ssec->reloc->data + relofs);
          if (rel->r_offset >= esym->st_value + size)
            continue;
          if (rel->r_offset < esym->st_value)
            break;
          put_elf_reloca(symtab_section, sec, c + rel->r_offset - esym->st_value, ELFW(R_TYPE)(rel->r_info),
                         ELFW(R_SYM)(rel->r_info),
#if PTR_SIZE == 8
                         rel->r_addend
#else
                         0
#endif
          );
        }
      }
    }
    else
    {
      if (type->t & VT_BITFIELD)
      {
        int bit_pos, bit_size, bits, n;
        unsigned char *p, v, m;
        bit_pos = BIT_POS(vtop->type.t);
        bit_size = BIT_SIZE(vtop->type.t);
        p = (unsigned char *)ptr + (bit_pos >> 3);
        bit_pos &= 7, bits = 0;
        while (bit_size)
        {
          n = 8 - bit_pos;
          if (n > bit_size)
            n = bit_size;
          v = val >> bits << bit_pos;
          m = ((1 << n) - 1) << bit_pos;
          *p = (*p & ~m) | (v & m);
          bits += n, bit_size -= n, bit_pos = 0, ++p;
        }
      }
      else
        switch (bt)
        {
        case VT_BOOL:
          *(char *)ptr = val != 0;
          break;
        case VT_BYTE:
          *(char *)ptr = val;
          break;
        case VT_SHORT:
          write16le(ptr, val);
          break;
        case VT_FLOAT:
          write32le(ptr, val);
          break;
        case VT_DOUBLE:
          write64le(ptr, val);
          break;
        case VT_LDOUBLE:
#if defined TCC_IS_NATIVE_387
          /* Host and target platform may be different but both have x87.
             On windows, tcc does not use VT_LDOUBLE, except when it is a
             cross compiler.  In this case a mingw gcc as host compiler
             comes here with 10-byte long doubles, while msvc or tcc won't.
             tcc itself can still translate by asm.
             In any case we avoid possibly random bytes 11 and 12.
          */
          if (sizeof(long double) >= 10)
            memcpy(ptr, &vtop->c.ld, 10);
#ifdef __TINYC__
          else if (sizeof(long double) == sizeof(double))
            __asm__("fldl %1\nfstpt %0\n" : "=m"(*ptr) : "m"(vtop->c.ld));
#endif
          else
#endif
            /* For other platforms it should work natively, but may not work
               for cross compilers */
            if (sizeof(long double) == LDOUBLE_SIZE)
              memcpy(ptr, &vtop->c.ld, LDOUBLE_SIZE);
            else if (sizeof(double) == LDOUBLE_SIZE)
              *(double *)ptr = (double)vtop->c.ld;
            else if (0 == memcmp(ptr, &vtop->c.ld, LDOUBLE_SIZE))
              ; /* nothing to do for 0.0 */
#ifndef TCC_CROSS_TEST
            else
              tcc_error("can't cross compile long double constants");
#endif
          break;

#if PTR_SIZE == 8
        /* intptr_t may need a reloc too, see tcctest.c:relocation_test() */
        case VT_LLONG:
        case VT_PTR:
          if (vtop->r & VT_SYM)
            greloca(sec, vtop->sym, c, R_DATA_PTR, val);
          else
            write64le(ptr, val);
          break;
        case VT_INT:
          write32le(ptr, val);
          break;
#else
        case VT_LLONG:
          write64le(ptr, val);
          break;
        case VT_PTR:
        case VT_INT:
          if (vtop->r & VT_SYM)
          {
            /* Debug check for garbage symbol */
            if (!vtop->sym || vtop->sym->v >= SYM_FIRST_ANOM + 100000)
            {
              tcc_error("internal error: init_putv has garbage sym (v=0x%x, r=0x%x)", vtop->sym ? vtop->sym->v : 0,
                        vtop->r);
            }
            greloc(sec, vtop->sym, c, R_DATA_PTR);
          }
          write32le(ptr, val);
          break;
#endif
        default:
          // tcc_internal_error("unexpected type");
          break;
        }
    }
    vtop--;
    print_vstack("init_putv(2)");
  }
  else
  {
    vset(&dtype, VT_LOCAL | VT_LVAL, c);
    if (vreg == -1)
    {
      /* Array element initialization: do NOT create a new vreg.
       * Instead, keep vr = -1 so that vstore() will recognize this
       * as a memory store, not a variable assignment.
       * The stack offset 'c' in vtop->c.i identifies the destination. */
      vtop->vr = -1;
    }
    else
    {
      vtop->vr = vreg;
      /* Mark long long variables for proper register allocation */
      if ((dtype.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vtop->vr);
      }
    }
    vswap();
    vstore();
    vpop();
  }
}

/* 't' contains the type and storage info. 'c' is the offset of the
   object in section 'sec'. If 'sec' is NULL, it means stack based
   allocation. 'flags & DIF_FIRST' is true if array '{' must be read (multi
   dimension implicit array init handling). 'flags & DIF_SIZE_ONLY' is true if
   size only evaluation is wanted (only for arrays). */
static void decl_initializer(init_params *p, CType *type, unsigned long c, int flags, int vreg)
{
  int len, n, no_oblock, i;
  int size1, align1;
  Sym *s, *f;
  Sym indexsym;
  CType *t1;

  /* generate line number info */
  if (debug_modes && !(flags & DIF_SIZE_ONLY) && !p->sec)
    tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

  if (!(flags & DIF_HAVE_ELEM) && tok != '{' &&
      /* In case of strings we have special handling for arrays, so
         don't consume them as initializer value (which would commit them
         to some anonymous symbol).  */
      tok != TOK_LSTR && tok != TOK_STR &&
      (!(flags & DIF_SIZE_ONLY)
       /* a struct may be initialized from a struct of same type, as in
               struct {int x,y;} a = {1,2}, b = {3,4}, c[] = {a,b};
          In that case we need to parse the element in order to check
          it for compatibility below */
       || (type->t & VT_BTYPE) == VT_STRUCT))
  {
    int ncw_prev = nocode_wanted;
    if ((flags & DIF_SIZE_ONLY) && !p->sec)
      ++nocode_wanted;
    parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
    nocode_wanted = ncw_prev;
    flags |= DIF_HAVE_ELEM;
  }

  if (type->t & VT_ARRAY)
  {
    no_oblock = 1;
    if (((flags & DIF_FIRST) && tok != TOK_LSTR && tok != TOK_STR) || tok == '{')
    {
      skip('{');
      no_oblock = 0;
    }

    s = type->ref;
    n = s->c;
    t1 = pointed_type(type);
    size1 = type_size(t1, &align1);

    /* only parse strings here if correct type (otherwise: handle
       them as ((w)char *) expressions */
    if ((tok == TOK_LSTR &&
#ifdef TCC_TARGET_PE
         (t1->t & VT_BTYPE) == VT_SHORT && (t1->t & VT_UNSIGNED)
#else
         (t1->t & VT_BTYPE) == VT_INT
#endif
             ) ||
        (tok == TOK_STR && (t1->t & VT_BTYPE) == VT_BYTE))
    {
      len = 0;
      cstr_reset(&initstr);
      if (size1 != (tok == TOK_STR ? 1 : sizeof(nwchar_t)))
        tcc_error("unhandled string literal merging");
      while (tok == TOK_STR || tok == TOK_LSTR)
      {
        if (initstr.size)
          initstr.size -= size1;
        if (tok == TOK_STR)
          len += tokc.str.size;
        else
          len += tokc.str.size / sizeof(nwchar_t);
        len--;
        cstr_cat(&initstr, tokc.str.data, tokc.str.size);
        next();
      }
      if (tok != ')' && tok != '}' && tok != ',' && tok != ';' && tok != TOK_EOF)
      {
        /* Not a lone literal but part of a bigger expression.  */
        unget_tok(size1 == 1 ? TOK_STR : TOK_LSTR);
        tokc.str.size = initstr.size;
        tokc.str.data = initstr.data;
        goto do_init_array;
      }

      decl_design_flex(p, s, len);
      if (!(flags & DIF_SIZE_ONLY))
      {
        int nb = n, ch;
        if (len < nb)
          nb = len;
        if (len > nb)
          tcc_warning("initializer-string for array is too long");
        /* in order to go faster for common case (char
           string in global variable, we handle it
           specifically */
        if (p->sec && size1 == 1)
        {
          init_assert(p, c + nb);
          if (!NODATA_WANTED)
            memcpy(p->sec->data + c, initstr.data, nb);
        }
        else
        {
          for (i = 0; i < n; i++)
          {
            if (i >= nb)
            {
              /* only add trailing zero if enough storage (no
                 warning in this case since it is standard) */
              if (flags & DIF_CLEAR)
                break;
              if (n - i >= 4)
              {
                init_putz(p, c + i * size1, (n - i) * size1);
                break;
              }
              ch = 0;
            }
            else if (size1 == 1)
              ch = ((unsigned char *)initstr.data)[i];
            else
              ch = ((nwchar_t *)initstr.data)[i];
            vpushi(ch);
            init_putv(p, t1, c + i * size1, vreg);
          }
        }
      }
    }
    else
    {

    do_init_array:
      indexsym.c = 0;
      f = &indexsym;

    do_init_list:
      /* zero memory once in advance */
      if (!(flags & (DIF_CLEAR | DIF_SIZE_ONLY)))
      {
        init_putz(p, c, n * size1);
        flags |= DIF_CLEAR;
      }

      len = 0;
      /* GNU extension: if the initializer is empty for a flex array,
         it's size is zero.  We won't enter the loop, so set the size
         now.  */
      decl_design_flex(p, s, len);
      while (tok != '}' || (flags & DIF_HAVE_ELEM))
      {
        len = decl_designator(p, type, c, &f, flags, len);
        flags &= ~DIF_HAVE_ELEM;
        if (type->t & VT_ARRAY)
        {
          ++indexsym.c;
          /* special test for multi dimensional arrays (may not
             be strictly correct if designators are used at the
             same time) */
          if (no_oblock && len >= n * size1)
            break;
        }
        else
        {
          if (s->type.t == VT_UNION)
            f = NULL;
          else
            f = f->next;
          if (no_oblock && f == NULL)
            break;
        }

        if (tok == '}')
          break;
        skip(',');
      }
    }
    if (!no_oblock)
      skip('}');
  }
  else if ((flags & DIF_HAVE_ELEM)
           /* Use i_c_parameter_t, to strip toplevel qualifiers.
              The source type might have VT_CONSTANT set, which is
              of course assignable to non-const elements.  */
           && is_compatible_unqualified_types(type, &vtop->type))
  {
    goto one_elem;
  }
  else if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    no_oblock = 1;
    if ((flags & DIF_FIRST) || tok == '{')
    {
      skip('{');
      no_oblock = 0;
    }
    s = type->ref;
    f = s->next;
    n = s->c;
    size1 = 1;
    goto do_init_list;
  }
  else if (tok == '{')
  {
    if (flags & DIF_HAVE_ELEM)
      skip(';');
    next();
    decl_initializer(p, type, c, flags & ~DIF_HAVE_ELEM, vreg);
    skip('}');
  }
  else
  one_elem:
    if ((flags & DIF_SIZE_ONLY))
    {
      /* If we supported only ISO C we wouldn't have to accept calling
         this on anything than an array if DIF_SIZE_ONLY (and even then
         only on the outermost level, so no recursion would be needed),
         because initializing a flex array member isn't supported.
         But GNU C supports it, so we need to recurse even into
         subfields of structs and arrays when DIF_SIZE_ONLY is set.  */
      /* just skip expression */
      if (flags & DIF_HAVE_ELEM)
        vpop();
      else
        skip_or_save_block(NULL);
    }
    else
    {
      if (!(flags & DIF_HAVE_ELEM))
      {
        /* This should happen only when we haven't parsed
           the init element above for fear of committing a
           string constant to memory too early.  */
        if (tok != TOK_STR && tok != TOK_LSTR)
          expect("string constant");
        parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
      }
      if (!p->sec && (flags & DIF_CLEAR) /* container was already zero'd */
          && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0 &&
          btype_size(type->t & VT_BTYPE) /* not for fp constants */
      )
        vpop();
      else
      {
        int align;
        int size = type_size(type, &align);
        /* Don't try to store empty structs (size 0) */
        if (size > 0)
          init_putv(p, type, c, vreg);
        else
          vpop(); /* pop the empty struct value */
      }
    }
}

/* parse an initializer for type 't' if 'has_init' is non zero, and
   allocate space in local or global data space ('r' is either
   VT_LOCAL or VT_CONST). If 'v' is non zero, then an associated
   variable 'v' of scope 'scope' is declared before initializers
   are parsed. If 'v' is zero, then a reference to the new object
   is put in the value stack. If 'has_init' is 2, a special parsing
   is done to handle string constants. */
static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v, int global)
{
  int size, align, addr;
  TokenString *init_str = NULL;
  int vreg = -1;
  Section *sec;
  Sym *flexible_array;
  Sym *sym;
  int saved_nocode_wanted = nocode_wanted;
#ifdef CONFIG_TCC_BCHECK
  int bcheck = tcc_state->do_bounds_check && !NODATA_WANTED;
#endif
  init_params p = {0};

  /* Always allocate static or global variables */
  if (v && (r & VT_VALMASK) == VT_CONST)
    nocode_wanted |= DATA_ONLY_WANTED;

  flexible_array = NULL;
  size = type_size(type, &align);

  /* exactly one flexible array may be initialized, either the
     toplevel array or the last member of the toplevel struct */

  if (size < 0)
  {
    // error out except for top-level incomplete arrays
    // (arrays of incomplete types are handled in array parsing)
    if (!(type->t & VT_ARRAY))
      tcc_error("initialization of incomplete type");

    /* If the base type itself was an array type of unspecified size
       (like in 'typedef int arr[]; arr x = {1};') then we will
       overwrite the unknown size by the real one for this decl.
       We need to unshare the ref symbol holding that size. */
    type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
    p.flex_array_ref = type->ref;
  }
  else if (has_init && (type->t & VT_BTYPE) == VT_STRUCT)
  {
    Sym *field = type->ref->next;
    if (field)
    {
      while (field->next)
        field = field->next;
      if (field->type.t & VT_ARRAY && field->type.ref->c < 0)
      {
        flexible_array = field;
        p.flex_array_ref = field->type.ref;
        size = -1;
      }
    }
  }

  if (size < 0)
  {
    /* If unknown size, do a dry-run 1st pass */
    if (!has_init)
      tcc_error("unknown type size");
    if (has_init == 2)
    {
      /* only get strings */
      init_str = tok_str_alloc();
      while (tok == TOK_STR || tok == TOK_LSTR)
      {
        tok_str_add_tok(init_str);
        next();
      }
      tok_str_add(init_str, TOK_EOF);
    }
    else
      skip_or_save_block(&init_str);
    unget_tok(0);

    /* compute size */
    begin_macro(init_str, 1);
    next();
    decl_initializer(&p, type, 0, DIF_FIRST | DIF_SIZE_ONLY, vreg);
    /* prepare second initializer parsing */
    macro_ptr = tok_str_buf(init_str);
    next();

    /* if still unknown size, error */
    size = type_size(type, &align);
    if (size < 0)
      tcc_error("unknown type size");

    /* If there's a flex member and it was used in the initializer
       adjust size.  */
    if (flexible_array && flexible_array->type.ref->c > 0)
      size += flexible_array->type.ref->c * pointed_size(&flexible_array->type);
  }

  /* take into account specified alignment if bigger */
  if (ad->a.aligned)
  {
    int speca = 1 << (ad->a.aligned - 1);
    if (speca > align)
      align = speca;
  }
  else if (ad->a.packed)
  {
    align = 1;
  }

  if (!v && NODATA_WANTED)
  {
    size = 0, align = 1;
  }

  if ((r & VT_VALMASK) == VT_LOCAL)
  {
    sec = NULL;
#ifdef CONFIG_TCC_BCHECK
    if (bcheck && v)
    {
      /* add padding between stack variables for bound checking */
      loc -= align;
    }
#endif
    if (!((r & VT_LVAL) && ((type->t & VT_BTYPE) != VT_STRUCT)))
    {
      // allocate stack for variables that are not register allocation
      // candidates
      loc = (loc - size) & -align;
    }
    addr = loc;
    p.local_offset = addr + size;
#ifdef CONFIG_TCC_BCHECK
    if (bcheck && v)
    {
      /* add padding between stack variables for bound checking */
      loc -= align;
    }
#endif
    if (v)
    {
      /* local variable */
#ifdef CONFIG_TCC_ASM
      if (ad->asm_label)
      {
        int reg = asm_parse_regvar(ad->asm_label);
        if (reg >= 0)
          r = (r & ~VT_VALMASK) | reg;
      }
#endif
      sym = sym_push(v, type, r, addr);
      vreg = sym->vreg;
      if (ad->cleanup_func)
      {
        Sym *cls = sym_push2(&all_cleanups, SYM_FIELD | ++cur_scope->cl.n, 0, 0);
        cls->prev_tok = sym;
        cls->cleanup_func = ad->cleanup_func;
        cls->next = cur_scope->cl.s;
        cur_scope->cl.s = cls;
      }

      sym->a = ad->a;
    }
    else
    {
      /* push local reference */
      vset(type, r, addr);
    }
  }
  else
  {
    sym = NULL;
    if (v && global)
    {
      /* see if the symbol was already defined */
      sym = sym_find(v);
      if (sym)
      {
        if (p.flex_array_ref && (sym->type.t & type->t & VT_ARRAY) && sym->type.ref->c > type->ref->c)
        {
          /* flex array was already declared with explicit size
                  extern int arr[10];
                  int arr[] = { 1,2,3 }; */
          type->ref->c = sym->type.ref->c;
          size = type_size(type, &align);
        }
        patch_storage(sym, ad, type);
        /* we accept several definitions of the same global variable. */
        if (!has_init && sym->c && elfsym(sym)->st_shndx != SHN_UNDEF)
          goto no_alloc;
      }
    }

    /* allocate symbol in corresponding section */
    sec = ad->section;
    if (!sec)
    {
      CType *tp = type;
      while ((tp->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
        tp = &tp->ref->type;
      if (tp->t & VT_CONSTANT)
      {
        sec = rodata_section;
      }
      else if (has_init)
      {
        sec = data_section;
        /*if (tcc_state->g_debug & 4)
            tcc_warning("rw data: %s", get_tok_str(v, 0));*/
      }
      else if (tcc_state->nocommon)
        sec = bss_section;
    }

    if (sec)
    {
      addr = section_add(sec, size, align);
#ifdef CONFIG_TCC_BCHECK
      /* add padding if bound check */
      if (bcheck)
        section_add(sec, 1, 1);
#endif
    }
    else
    {
      addr = align; /* SHN_COMMON is special, symbol value is align */
      sec = common_section;
    }

    if (v)
    {
      if (!sym)
      {
        sym = sym_push(v, type, r | VT_SYM, 0);
        vreg = sym->vreg;
        patch_storage(sym, ad, NULL);
      }
      /* update symbol definition */
      put_extern_sym(sym, sec, addr, size);
    }
    else
    {
      /* push global reference */
      vpush_ref(type, sec, addr, size);
      sym = vtop->sym;
      vtop->r |= r;
    }

#ifdef CONFIG_TCC_BCHECK
    /* handles bounds now because the symbol must be defined
       before for the relocation */
    if (bcheck)
    {
      addr_t *bounds_ptr;

      greloca(bounds_section, sym, bounds_section->data_offset, R_DATA_PTR, 0);
      /* then add global bound info */
      bounds_ptr = section_ptr_add(bounds_section, 2 * sizeof(addr_t));
      bounds_ptr[0] = 0; /* relocated */
      bounds_ptr[1] = size;
    }
#endif
  }

  if (type->t & VT_VLA)
  {
    int a;

    if (NODATA_WANTED)
      goto no_alloc;

    if (tcc_state->ir)
      tcc_state->force_frame_pointer = 1;

    /* save before-VLA stack pointer if needed */
    if (cur_scope->vla.num == 0)
    {
      if (cur_scope->prev && cur_scope->prev->vla.num)
      {
        cur_scope->vla.locorig = cur_scope->prev->vla.loc;
      }
      else
      {
        /* No outer VLA active: lazily allocate a slot and save the current SP
         * as the "before VLA" restore point for VLAs introduced in this scope. */
        loc -= PTR_SIZE;
        if (tcc_state->ir)
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type.t = VT_PTR;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.c.i = loc;
          dst.vr = -1;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_SP_SAVE, NULL, NULL, &dst);
        }
        else
        {
          gen_vla_sp_save(loc);
        }
        cur_scope->vla.locorig = loc;
      }
    }

    vpush_type_size(type, &a);
    if (tcc_state->ir)
    {
      /* vtop holds the runtime allocation size (bytes). Emit an IR op that
       * adjusts SP and aligns it. */
      SValue size_sv = *vtop;

      SValue align_sv;
      memset(&align_sv, 0, sizeof(align_sv));
      align_sv.type.t = VT_INT;
      align_sv.r = VT_CONST;
      align_sv.c.i = a;
      align_sv.vr = -1;

      tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_ALLOC, &size_sv, &align_sv, NULL);
      vpop();
    }
    else
    {
      gen_vla_alloc(type, a);
    }
#if defined TCC_TARGET_PE && defined TCC_TARGET_X86_64
    /* on _WIN64, because of the function args scratch area, the
       result of alloca differs from RSP and is returned in RAX.  */
    gen_vla_result(addr), addr = (loc -= PTR_SIZE);
#endif

    if (tcc_state->ir)
    {
      SValue dst;
      memset(&dst, 0, sizeof(dst));
      dst.type.t = VT_PTR;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.c.i = addr;
      dst.vr = -1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_SP_SAVE, NULL, NULL, &dst);
    }
    else
    {
      gen_vla_sp_save(addr);
    }
    cur_scope->vla.loc = addr;
    cur_scope->vla.num++;
  }
  else if ((r & VT_VALMASK) == VT_LOCAL
           && struct_has_vla_member(type)
           && !NODATA_WANTED)
  {
    /* The struct contains VLA member(s).  Each VLA field in the struct
       is represented as a pointer; we must dynamically allocate the
       backing storage on the stack and store the pointer into the
       struct field so that subsequent accesses go to valid memory. */
    Sym *f;
    int a;

    if (tcc_state->ir)
      tcc_state->force_frame_pointer = 1;

    for (f = type->ref->next; f; f = f->next)
    {
      if (!(f->type.t & VT_VLA))
        continue;

      /* save before-VLA stack pointer if needed */
      if (cur_scope->vla.num == 0)
      {
        if (cur_scope->prev && cur_scope->prev->vla.num)
        {
          cur_scope->vla.locorig = cur_scope->prev->vla.loc;
        }
        else
        {
          loc -= PTR_SIZE;
          if (tcc_state->ir)
          {
            SValue dst;
            memset(&dst, 0, sizeof(dst));
            dst.type.t = VT_PTR;
            dst.r = VT_LOCAL | VT_LVAL;
            dst.c.i = loc;
            dst.vr = -1;
            tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_SP_SAVE, NULL, NULL, &dst);
          }
          else
          {
            gen_vla_sp_save(loc);
          }
          cur_scope->vla.locorig = loc;
        }
      }

      /* Push VLA runtime size and emit VLA_ALLOC */
      vpush_type_size(&f->type, &a);
      if (tcc_state->ir)
      {
        SValue size_sv = *vtop;
        SValue align_sv;
        memset(&align_sv, 0, sizeof(align_sv));
        align_sv.type.t = VT_INT;
        align_sv.r = VT_CONST;
        align_sv.c.i = a;
        align_sv.vr = -1;
        tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_ALLOC, &size_sv, &align_sv, NULL);
        vpop();
      }
      else
      {
        gen_vla_alloc(&f->type, a);
      }

      /* Save new SP (the VLA data pointer) into the struct field */
      {
        int field_addr = addr + f->c;
        if (tcc_state->ir)
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type.t = VT_PTR;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.c.i = field_addr;
          dst.vr = -1;
          tcc_ir_put(tcc_state->ir, TCCIR_OP_VLA_SP_SAVE, NULL, NULL, &dst);
        }
        else
        {
          gen_vla_sp_save(field_addr);
        }
        cur_scope->vla.loc = field_addr;
      }
      cur_scope->vla.num++;
    }
  }
  else if (has_init)
  {
    p.sec = sec;
    decl_initializer(&p, type, addr, DIF_FIRST, vreg);
    /* patch flexible array member size back to -1, */
    /* for possible subsequent similar declarations */
    if (flexible_array)
      flexible_array->type.ref->c = -1;
  }

no_alloc:
  /* restore parse state if needed */
  if (init_str)
  {
    end_macro();
    next();
  }

  nocode_wanted = saved_nocode_wanted;
}

/* generate vla code saved in post_type() */
static void func_vla_arg_code(Sym *arg)
{
  int align;
  TokenString *vla_array_tok = NULL;

  if (arg->type.ref)
    func_vla_arg_code(arg->type.ref);

  if ((arg->type.t & VT_VLA) && arg->type.ref->vla_array_str)
  {
    loc -= type_size(&int_type, &align);
    loc &= -align;
    arg->type.ref->c = loc;

    unget_tok(0);
    vla_array_tok = tok_str_alloc();
    vla_array_tok->data.str = arg->type.ref->vla_array_str;
    vla_array_tok->allocated_len = 1;
    begin_macro(vla_array_tok, 2); /* alloc=2: don't free borrowed buffer */
    next();
    gexpr();
    end_macro();
    next();
    vpush_type_size(&arg->type.ref->type, &align);
    gen_op('*');
    vset(&int_type, VT_LOCAL | VT_LVAL, arg->type.ref->c);
    vswap();
    vstore();
    vpop();
  }
}

static void func_vla_arg(Sym *sym)
{
  Sym *arg;

  for (arg = sym->type.ref->next; arg; arg = arg->next)
    if ((arg->type.t & VT_BTYPE) == VT_PTR && (arg->type.ref->type.t & VT_VLA))
      func_vla_arg_code(arg->type.ref);
}

/* Forward declaration for nested function compilation */
static void gen_function(Sym *sym);

/* Find NestedFunc by function symbol */
static NestedFunc *find_nested_func_by_sym(Sym *sym)
{
  for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
  {
    if (tcc_state->nested_funcs[i].sym == sym)
      return &tcc_state->nested_funcs[i];
  }
  return NULL;
}

/* Set up trampoline for a nested function whose address is being taken.
 * Creates chain slot and trampoline symbols if not yet created,
 * emits INIT_CHAIN_SLOT IR to store parent FP into the chain slot,
 * and replaces vtop->sym with the trampoline symbol. */
static void setup_nested_func_trampoline(Sym *s)
{
  NestedFunc *nf = find_nested_func_by_sym(s);
  if (!nf)
    return;

  nf->trampoline_needed = 1;

  /* Get the nested function's ELF name for symbol naming */
  const char *func_name = get_tok_str(nf->sym->asm_label ? nf->sym->asm_label : nf->sym->v, NULL);

  /* Create chain slot TCC symbol + ELF symbol in .data (if not already created) */
  if (!nf->chain_slot_tcc_sym)
  {
    Section *data_sec = data_section;
    addr_t offset = section_add(data_sec, 4, 4);

    char chain_name[256];
    snprintf(chain_name, sizeof(chain_name), "__chain_%s", func_name);
    int elf_idx =
        put_elf_sym(symtab_section, offset, 4, ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0, data_sec->sh_num, chain_name);

    /* Initialize to 0 */
    memset(data_sec->data + offset, 0, 4);

    /* Create a TCC Sym so greloc/load_full_const can work with it */
    Sym *cs_sym = sym_malloc();
    memset(cs_sym, 0, sizeof(*cs_sym));
    cs_sym->v = anon_sym++;
    cs_sym->type.t = VT_INT;
    cs_sym->r = VT_CONST | VT_SYM;
    cs_sym->c = elf_idx;
    nf->chain_slot_tcc_sym = cs_sym;
  }

  /* Create trampoline TCC symbol + ELF symbol in .text (if not already created) */
  if (!nf->trampoline_tcc_sym)
  {
    Section *text_sec = cur_text_section;
    char tramp_name[256];
    snprintf(tramp_name, sizeof(tramp_name), "__tramp_%s", func_name);

    /* Placeholder: offset will be updated when trampoline code is emitted */
    int elf_idx =
        put_elf_sym(symtab_section, 0, 24, ELFW(ST_INFO)(STB_LOCAL, STT_FUNC), 0, text_sec->sh_num, tramp_name);

    Sym *tr_sym = sym_malloc();
    memset(tr_sym, 0, sizeof(*tr_sym));
    tr_sym->v = anon_sym++;
    tr_sym->type.t = VT_FUNC;
    tr_sym->r = VT_CONST | VT_SYM;
    tr_sym->c = elf_idx;
    nf->trampoline_tcc_sym = tr_sym;
  }

  /* Emit INIT_CHAIN_SLOT IR: store parent FP to chain slot at runtime */
  if (tcc_state->ir && !NOEVAL_WANTED)
  {
    SValue src, dest;
    svalue_init(&src);
    svalue_init(&dest);
    /* src carries the chain slot symbol so the codegen can emit a
     * LDR + STR sequence with the correct relocation */
    src.type.t = VT_INT;
    src.r = VT_CONST | VT_SYM;
    src.sym = nf->chain_slot_tcc_sym;
    src.c.i = 0;
    src.vr = -1;
    dest.type.t = VT_INT;
    dest.r = 0;
    dest.vr = -1;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_INIT_CHAIN_SLOT, &src, NULL, &dest);
  }

  /* Replace the function symbol with the trampoline symbol */
  vtop->sym = nf->trampoline_tcc_sym;
}

/* Emit trampoline code for a nested function that needs it */
static void emit_trampoline_for_nested_func(NestedFunc *nf)
{
  Section *text_sec = cur_text_section;

  /* Trampoline is 20 bytes: 14 bytes code + 2 bytes NOP + 4+4 literal pool.
   * Plus up to 3 bytes for alignment padding.
   * We must ensure the section buffer can hold these bytes. The codegen
   * sets data_offset = ind at the end, but we're before that point.
   * Use section_prealloc to extend the buffer without moving data_offset. */
  section_prealloc(text_sec, 24);

  /* Align ind to 4-byte boundary for the trampoline */
  while (ind & 3)
  {
    text_sec->data[ind++] = 0x00;
  }

  addr_t tramp_start = ind;

  /* Trampoline layout (20 bytes total, no padding needed):
   *   +0:  LDR  r10, [pc, #8]   ; r10 = chain_slot address (from +12)
   *   +4:  LDR  r10, [r10, #0]  ; r10 = *chain_slot = parent FP value
   *   +8:  LDR  pc, [pc, #4]    ; pc = function address (from +16), tail call
   *   +12: .word chain_slot_addr ; address of chain slot in .data
   *   +16: .word function_addr   ; address of nested function in .text
   *
   * PC-relative offset calculation (Thumb: PC reads as current + 4):
   *   LDR at +0: PC=+4, offset=8  → loads from +12 (chain_slot)
   *   LDR at +8: PC=+12, offset=4 → loads from +16 (function)
   */

  /* LDR R10, [PC, #8] - Thumb-2 encoding: F8DF A008 */
  text_sec->data[ind++] = 0xDF;
  text_sec->data[ind++] = 0xF8;
  text_sec->data[ind++] = 0x08;
  text_sec->data[ind++] = 0xA0;

  /* LDR R10, [R10, #0] - Thumb-2 encoding: F8DA A000 */
  text_sec->data[ind++] = 0xDA;
  text_sec->data[ind++] = 0xF8;
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0xA0;

  /* LDR PC, [PC, #4] - Thumb-2 encoding: F8DF F004 */
  text_sec->data[ind++] = 0xDF;
  text_sec->data[ind++] = 0xF8;
  text_sec->data[ind++] = 0x04;
  text_sec->data[ind++] = 0xF0;

  /* Literal pool entry 1: chain slot address (+12) */
  greloc(text_sec, nf->chain_slot_tcc_sym, ind, R_ARM_ABS32);
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;

  /* Literal pool entry 2: nested function address (+16) */
  greloc(text_sec, nf->sym, ind, R_ARM_ABS32);
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;
  text_sec->data[ind++] = 0x00;

  /* Update the ELF symbol for the trampoline to point to actual code location */
  {
    ElfSym *esym = elfsym(nf->trampoline_tcc_sym);
    if (esym)
    {
      esym->st_value = tramp_start + 1; /* +1 for Thumb bit */
      esym->st_size = ind - tramp_start;
    }
  }

  /* Sync data_offset so the section knows about the trampoline bytes */
  text_sec->data_offset = ind;
}

/* Emit all trampolines needed for nested functions in this parent */
static void emit_all_trampolines(void)
{
  for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
  {
    NestedFunc *nf = &tcc_state->nested_funcs[i];
    if (nf->trampoline_needed)
    {
      emit_trampoline_for_nested_func(nf);
    }
  }
}

/* Saved state for parent function when compiling nested functions */
typedef struct
{
  TCCIRState *ir;
  int loc;
  int ind;
  int rsym;
  int func_ind;
  const char *funcname;
  CType func_vt;
  int func_var;
  int cur_scope;
  int root_scope;
  int loop_scope;
  Sym *local_stack;
  Sym *local_label_stack;
  Sym *global_label_stack;
  unsigned nocode_wanted;
  int local_scope_level;
  int nb_temp_local_vars;
  /* Use mangled names to avoid macro conflicts */
  Section *sec_text;
  struct switch_t *sec_switch;
  /* Temp local vars array */
  struct temp_local_variable tmp_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
} ParentSavedState;

/* Compile all nested functions defined inside a parent function */
static void compile_nested_functions(Sym *parent_sym)
{
  int nb_nested;
  ParentSavedState saved;

  (void)parent_sym; /* currently unused */

  nb_nested = tcc_state->nb_nested_funcs;
  if (nb_nested == 0)
    return;

  /* Save debug state before nested function compilation */
  void *saved_debug_info, *saved_debug_root;
  tcc_debug_save_state(tcc_state, &saved_debug_info, &saved_debug_root);

  /* Save ALL parent global state */
  saved.ir = tcc_state->ir;
  saved.loc = loc;
  saved.ind = ind;
  saved.rsym = rsym;
  saved.func_ind = func_ind;
  saved.funcname = funcname;
  saved.func_vt = func_vt;
  saved.func_var = func_var;
  saved.cur_scope = (int)(intptr_t)cur_scope;
  saved.root_scope = (int)(intptr_t)root_scope;
  saved.loop_scope = (int)(intptr_t)loop_scope;
  saved.local_stack = local_stack;
  saved.local_label_stack = local_label_stack;
  saved.global_label_stack = global_label_stack;
  saved.nocode_wanted = nocode_wanted;
  saved.local_scope_level = local_scope;
  saved.nb_temp_local_vars = nb_temp_local_vars;
  saved.sec_text = cur_text_section;
  saved.sec_switch = cur_switch;
  memcpy(saved.tmp_vars, arr_temp_local_vars, sizeof(arr_temp_local_vars));

  /* Compile each nested function.
   * Use a static index that persists across recursive calls.
   * This ensures each function is compiled exactly once even when
   * gen_function calls compile_nested_functions recursively. */
  static int compile_idx = 0;
  while (compile_idx < tcc_state->nb_nested_funcs)
  {
    NestedFunc *nf = &tcc_state->nested_funcs[compile_idx];

    /* Skip already-compiled functions (safety check) */
    if (nf->compiled)
    {
      compile_idx++;
      continue;
    }

    /* For nested function compilation, start with a fresh local_stack.
     * Captured variable resolution is handled in the identifier lookup code
     * (see tok_identifier in tccgen.c), which checks current_nested_func. */
    local_stack = NULL; /* Start fresh - captured vars handled specially */
    local_scope = 0;

    /* Track current nested function for static chain setup */
    tcc_state->current_nested_func = nf;

    /* Replay saved token stream (same as inline function expansion) */
    tccpp_putfile(nf->filename);
    begin_macro(nf->func_str, 1);
    next(); /* prime the first token - should be '{' */

    /* Set up text section for nested function (same as regular functions) */
    if (!cur_text_section)
      cur_text_section = text_section;

    /* Use the symbol that was already pushed during parsing */
    /* The symbol was pushed with VT_CONST to mark it as a function */

    /* Mark as compiled BEFORE gen_function to prevent recursive recompilation.
     * gen_function may discover inner nested functions (e.g., level2 inside level1)
     * and call compile_nested_functions recursively. If this function isn't marked,
     * the recursive call would try to compile it again (compile_idx is static). */
    nf->compiled = 1;

    gen_function(nf->sym);

    /* gen_function() resets cur_text_section=NULL and ind=0 for safety.
     * Restore them so the next nested function starts at the right offset
     * and compile_nested_functions can report the correct ind to the parent. */
    cur_text_section = saved.sec_text;
    ind = cur_text_section->data_offset;

    /* Clear current nested function */
    tcc_state->current_nested_func = NULL;

    end_macro();

    /* Continue to next nested function. If new ones were discovered during
     * compilation, they'll have indices > compile_idx, and we'll get to them
     * because compile_idx < nb_nested_funcs will still be true. */
    compile_idx++;
  }

  /* Restore ALL parent state */
  tcc_state->ir = saved.ir;
  loc = saved.loc;
  /* NOTE: do NOT restore ind - nested func code is in .text and
     the parent's codegen will emit at the CURRENT ind (after nested funcs) */
  rsym = saved.rsym;
  func_ind = saved.func_ind;
  funcname = saved.funcname;
  func_vt = saved.func_vt;
  func_var = saved.func_var;
  cur_scope = (struct scope *)(intptr_t)saved.cur_scope;
  root_scope = (struct scope *)(intptr_t)saved.root_scope;
  loop_scope = (struct scope *)(intptr_t)saved.loop_scope;
  local_stack = saved.local_stack;
  local_label_stack = saved.local_label_stack;
  global_label_stack = saved.global_label_stack;
  nocode_wanted = saved.nocode_wanted;
  local_scope = saved.local_scope_level;
  nb_temp_local_vars = saved.nb_temp_local_vars;
  cur_text_section = saved.sec_text;
  cur_switch = saved.sec_switch;
  memcpy(arr_temp_local_vars, saved.tmp_vars, sizeof(arr_temp_local_vars));

  /* Restore debug state for parent function */
  tcc_debug_restore_state(tcc_state, saved_debug_info, saved_debug_root);

  /* Emit trampolines for nested functions whose address was taken.
   * Must be done before clearing the nested funcs list. */
  emit_all_trampolines();

  /* Clear nested funcs list after compiling */
  tcc_state->nb_nested_funcs = 0;
}

/* Track which nested function is currently being prescanned.
 * This is needed for multi-level nesting to establish parent-child links. */
static NestedFunc *prescan_current_nf = NULL;

/* Pre-scan a nested function's token stream to identify captured parent variables.
 * This is called during parsing of the parent function, before the parent's block
 * generates IR, so that captured variables can be marked address-taken early.
 * If explicit_parent_nf is non-NULL, it is used as the parent (for nested funcs
 * discovered during gen_function). Otherwise, prescan_current_nf is used. */
static void prescan_captured_vars(NestedFunc *nf, Sym *parent_local_stack, NestedFunc *explicit_parent_nf);

/* Pre-scan a nested function's token stream to identify captured parent variables.
 * This is called during parsing of the parent function, before the parent's block
 * generates IR, so that captured variables can be marked address-taken early. */
static void prescan_captured_vars(NestedFunc *nf, Sym *parent_local_stack, NestedFunc *explicit_parent_nf)
{
  /* If we're already inside a prescan (prescan_current_nf != NULL), this means
   * we discovered a nested function during another nested function's prescan.
   * Skip the prescan of this inner function - it will be handled later when
   * the outer function is compiled and its tokens are replayed. */
  if (prescan_current_nf != NULL)
  {
    /* Just set the parent link so we know the hierarchy */
    nf->parent_nf = prescan_current_nf;
    return;
  }

  /* Set parent_nf for multi-level nesting support.
   * If explicit_parent_nf is provided, use it (for nested funcs discovered
   * during gen_function). Otherwise, use prescan_current_nf (for nested funcs
   * discovered during prescan). */
  nf->parent_nf = explicit_parent_nf;

  /* Save and set current */
  NestedFunc *saved_current = prescan_current_nf;
  prescan_current_nf = nf;
  TokenString *tok_str = nf->func_str;
  const int *tokens;
  int pos;

  if (!tok_str)
    return;

  tokens = tok_str_buf(tok_str);
  pos = 0;

  while (tokens[pos] != TOK_EOF && tokens[pos] != 0)
  {
    int t = tokens[pos];

    if (t >= TOK_IDENT)
    {
      /* Look up this identifier in parent's local stack */
      Sym *s = sym_find2(parent_local_stack, t);
      if (s && ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM)))
      {
        /* Mark as address-taken to force stack allocation */
        s->a.addrtaken = 1;
        /* Also mark in IR so register allocator knows to spill to stack */
        if (tcc_state->ir && s->vreg >= 0)
          tcc_ir_set_addrtaken(tcc_state->ir, s->vreg);

        /* Record the variable if we haven't already */
        int i;
        int already_captured = 0;
        for (i = 0; i < nf->nb_captured; i++)
        {
          if (nf->captured_tokens[i] == t)
          {
            already_captured = 1;
            break;
          }
        }
        if (!already_captured && nf->nb_captured < MAX_CAPTURED_VARS)
        {
          nf->captured_vregs[nf->nb_captured] = s->vreg;
          nf->captured_offsets[nf->nb_captured] = s->c;
          nf->captured_tokens[nf->nb_captured] = t;
          nf->captured_types[nf->nb_captured] = s->type;
          nf->captured_chain_depth[nf->nb_captured] = 1; /* direct parent */
          nf->nb_captured++;
        }
      }
      /* Not found in parent locals — search parent's own captured vars.
       * level1 captured 'a' from main with depth 1, so level2 inherits
       * it with depth 2. */
      else if (nf->parent_nf)
      {
        NestedFunc *parent_nf = nf->parent_nf;
        for (int j = 0; j < parent_nf->nb_captured; j++)
        {
          if (parent_nf->captured_tokens[j] == t)
          {
            /* Guard: check not already captured (e.g. token appears twice) */
            int dup = 0;
            for (int k = 0; k < nf->nb_captured; k++)
              if (nf->captured_tokens[k] == t)
              {
                dup = 1;
                break;
              }
            if (dup)
              break;

            nf->captured_offsets[nf->nb_captured] = parent_nf->captured_offsets[j];
            nf->captured_tokens[nf->nb_captured] = t;
            nf->captured_types[nf->nb_captured] = parent_nf->captured_types[j];
            nf->captured_vregs[nf->nb_captured] = parent_nf->captured_vregs[j];
            nf->captured_chain_depth[nf->nb_captured] = parent_nf->captured_chain_depth[j] + 1;
            /* Child needs multi-hop → parent must save chain at FP-4 */
            if (nf->captured_chain_depth[nf->nb_captured] > 1)
            {
              parent_nf->needs_chain_save = 1;
              /* Also update parent's IR if it's currently being compiled */
              if (tcc_state->ir && tcc_state->ir->has_static_chain)
                tcc_state->ir->needs_chain_save = 1;
            }
            nf->nb_captured++;
            break;
          }
        }
      }
    }
    /* Advance past token. Simple approach: just move forward by 1.
     * A more complete implementation would handle multi-token sequences
     * (e.g., numbers, strings), but this suffices for basic identifier matching. */
    pos++;
  }

  /* Restore previous prescan current */
  prescan_current_nf = saved_current;
}

/* parse a function defined by symbol 'sym' and generate its code in
   'cur_text_section' */
static void gen_function(Sym *sym)
{
  struct scope f = {0};
  TCCIRState *ir;
  Sym *global_label_stack_start; /* save global label stack at function start */
  cur_scope = root_scope = &f;
  nocode_wanted = 0;

  ind = cur_text_section->data_offset;
  /* Reset per-function flags */
  tcc_state->force_frame_pointer = 0;
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_lr_save = 0;

  /* Save global label stack position so we only pop labels from this function */
  global_label_stack_start = global_label_stack;

  if (sym->a.aligned)
  {
    size_t newoff = section_add(cur_text_section, 0, 1 << (sym->a.aligned - 1));
    gen_fill_nops(newoff - ind);
  }

  funcname = get_tok_str(sym->v, NULL);
  func_ind = ind;
  func_vt = sym->type.ref->type;
  func_var = sym->type.ref->f.func_type == FUNC_ELLIPSIS;

  /* NOTE: we patch the symbol size later */
  put_extern_sym(sym, cur_text_section, ind + 1, 0);

  if (sym->type.ref->f.func_ctor)
    add_array(tcc_state, ".init_array", sym->c);
  if (sym->type.ref->f.func_dtor)
    add_array(tcc_state, ".fini_array", sym->c);

  /* put debug symbol */
  tcc_debug_funcstart(tcc_state, sym);

  /* push a dummy symbol to enable local sym storage */
  sym_push2(&local_stack, SYM_FIELD, 0, 0);
#ifdef DEBUG_IR_GEN
  printf("Generating IR for function %s\n", funcname);
#endif
  ir = tcc_ir_alloc();
  tcc_state->ir = ir;
  ir->naked = sym->a.naked;

  /* Check if we're compiling a nested function with captured variables */
  if (tcc_state->current_nested_func && tcc_state->current_nested_func->nb_captured > 0)
  {
    NestedFunc *nf = tcc_state->current_nested_func;
    /* Set up static chain for nested function */
    ir->has_static_chain = 1;
    /* Store captured variable offsets for chain-relative addressing */
    ir->captured_count = nf->nb_captured;
    for (int j = 0; j < nf->nb_captured && j < 32; j++)
    {
      ir->captured_offsets_list[j] = nf->captured_offsets[j];
      ir->captured_chain_depths[j] = nf->captured_chain_depth[j];
    }
    /* Allocate a vreg for the static chain pointer (models R10 as parameter) */
    ir->static_chain_vreg = tcc_ir_get_vreg_static_chain(ir);
    /* Propagate needs_chain_save from NestedFunc to IR */
    ir->needs_chain_save = nf->needs_chain_save;
  }

  /* Initialize FP offset cache for code generation optimization */
  if (tcc_state->opt_fp_offset_cache)
    tcc_ir_opt_fp_cache_init(ir);

  local_scope = 1; /* for function parameters */
  tcc_ir_params_add(ir, &sym->type);

  /* Reserve chain save slot at FP-4 AFTER tcc_ir_params_add (which resets loc).
   * This biases the global `loc` so that no local variable or spill slot
   * occupies FP-4, which is used to save the incoming static chain (R10)
   * for multi-level nested function access.
   * We always reserve FP-4 when has_static_chain is set; the chain save
   * instruction is only emitted during codegen if needs_chain_save is true.
   * This is necessary because needs_chain_save may be discovered late (when
   * inner nested functions are found during body parsing). */
  if (ir->has_static_chain)
    loc -= 4;
  nb_temp_local_vars = 0;
  if (!sym->a.naked)
  {
    // gfunc_prolog(sym);
    // Note: tcc_debug_prolog_epilog(0) is now called from ir/codegen.c
    // after tcc_gen_machine_prolog() so that the DWARF prologue_end
    // marker is emitted at the correct PC (after the machine prolog).
  }

  local_scope = 0;
  rsym = -1; /* Initialize return symbol chain with -1 sentinel */
  func_vla_arg(sym);
  block(0);
  /* Backpatch all return jumps to point to the epilogue (past the end of IR) */
  tcc_ir_backpatch_to_here(ir, rsym);

#ifdef CONFIG_TCC_DEBUG
  if (tcc_state->dump_ir)
  {
    tcc_ir_dump_set_show_physical_regs(0); /* Show only virtual registers */
    printf("=== IR BEFORE OPTIMIZATIONS ===\n");
    tcc_ir_show(ir);
    printf("=== END IR BEFORE OPTIMIZATIONS ===\n");
  }
#endif

  /* Iterative optimization loop
   * Runs optimization passes until no more changes are made,
   * or until max iterations reached. This allows constant propagation
   * to feed into branch folding, which then enables more DCE, etc.
   */
  int iteration = 0;
  const int max_iterations = 10;
  int changes = 0;

  do
  {
    changes = 0;
    iteration++;

    /* Dead code elimination - remove unreachable instructions */
    if (tcc_state->opt_dce)
      changes += tcc_ir_opt_dce(ir);

    /* Phase 1: Constant Propagation with Algebraic Simplification */
    if (tcc_state->opt_const_prop)
      changes += tcc_ir_opt_const_prop(ir);

    /* Phase 1b: TMP Constant Propagation - propagate constants from folded expressions */
    if (tcc_state->opt_const_prop)
      changes += tcc_ir_opt_const_prop_tmp(ir);

    /* Phase 1c: Constant Branch Folding - fold branches with constant conditions
     * This is critical for optimizing conditionals where values are constants.
     * Must run after constant propagation to maximize folding opportunities.
     */
    if (tcc_state->opt_const_prop)
      changes += tcc_ir_opt_branch_folding(ir);

    /* Phase 1d: Value Tracking through Arithmetic - track constants through ADD/SUB
     * This enables folding comparisons like "CMP V0, #1000000" when V0 has a
     * known constant value from previous arithmetic (e.g., V0 = 1234 - 42 = 1192).
     */
    if (tcc_state->opt_const_prop)
      changes += tcc_ir_opt_value_tracking(ir);

    /* Phase 1e: Non-negative value branch folding - fold soft-float comparisons
     * of known non-negative values (e.g. fabs(x)) against zero.
     */
    if (tcc_state->opt_nonneg_fold)
      changes += tcc_ir_opt_nonneg_branch_fold(ir);

    /* Phase 2: Copy Propagation */
    if (tcc_state->opt_copy_prop)
      changes += tcc_ir_opt_copy_prop(ir);

    /* Phase 3: Arithmetic Common Subexpression Elimination */
    if (tcc_state->opt_cse)
      changes += tcc_ir_opt_cse_arith(ir);

  } while (changes > 0 && iteration < max_iterations);

  /* Phase 3b: Global CSE - eliminate redundant computations across basic blocks
   * This catches cases like address calculations in if/else branches where
   * the same computation happens in both branches.
   * NOTE: Currently disabled due to issues with complex control flow (gotos/labels)
   */
  (void)tcc_ir_opt_cse_global;
  // #if 0
  if (tcc_state->opt_cse)
  {
    int gcse_changes = tcc_ir_opt_cse_global(ir);
    if (gcse_changes > 0)
    {
      if (tcc_state->opt_dce)
        tcc_ir_opt_dce(ir); /* Clean up any newly dead code */

      /* GCSE creates TMP<-TMP ASSIGN (copy) instructions. Run copy propagation
       * to propagate these copies, enabling further CSE matches.
       * Example: GCSE replaces T12<-V1 SHL #2 with T12<-T7. Then P0 ADD T12
       * doesn't match P0 ADD T7 until copy prop replaces T12 with T7. */
      for (int gcse_round = 0; gcse_round < 3; gcse_round++)
      {
        int cp = tcc_state->opt_copy_prop ? tcc_ir_opt_copy_prop(ir) : 0;
        if (cp <= 0)
          break;
        int cse2 = tcc_ir_opt_cse_arith(ir);
        cse2 += tcc_ir_opt_cse_global(ir);
        if (tcc_state->opt_dce)
          tcc_ir_opt_dce(ir);
        if (cse2 <= 0)
          break;
      }
    }
  }
  // #endif

#ifdef DEBUG_IR_GEN
  if (iteration > 1)
  {
    printf("OPTIMIZE: Ran %d optimization iterations\n", iteration);
  }
#endif

  /* Phase 2c: Jump Threading - forward jump targets through NOPs and chains
   * This eliminates unnecessary jumps and simplifies control flow.
   */
  if (tcc_state->opt_jump_threading)
  {
    int jump_changes = tcc_ir_opt_jump_threading(ir);
    /* Always run fall-through elimination when jump threading is enabled.
     * Fall-through jumps can appear even without threading changes, e.g.
     * when DCE turns dead code into NOPs making a JMP target the next
     * real instruction.  This is essential for dead-code suppression in
     * tests like 96_nodata_wanted. */
    jump_changes += tcc_ir_opt_eliminate_fallthrough(ir);
    if (jump_changes && tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up any newly unreachable code */
  }

  /* Phase 3b: MLA (Multiply-Accumulate) Fusion - fuse MUL + ADD into MLA */
  /* This should run after CSE so we have clean MUL+ADD patterns */
  if (tcc_state->opt_mla_fusion && tcc_ir_opt_mla_fusion(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up any newly unreachable code */

  /* Phase 3c: Stack Address CSE - hoist repeated stack address computations
   * This enables indexed memory fusion for stack-allocated arrays by
   * creating a vreg to hold the base address instead of recomputing it.
   */
  if (tcc_state->opt_stack_addr_cse && tcc_ir_opt_stack_addr_cse(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up any newly unreachable code */

  /* Phase 4: Indexed Load/Store Fusion - fuse SHL + ADD + LOAD/STORE
   * Pattern: arr[index] -> uses ARM's LDR/STR with scaled register offset
   */
  if (tcc_state->opt_indexed_memory && tcc_ir_opt_indexed_memory_fusion(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up any newly unreachable code */

  /* Phase 4b: Post-Increment Load/Store Fusion - fuse LOAD/STORE + ADD
   * Pattern: *ptr++; -> uses ARM's LDR/STR with post-increment
   */
  if (tcc_state->opt_postinc_fusion && tcc_ir_opt_postinc_fusion(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up any newly unreachable code */

  /* Common subexpression elimination for commutative boolean ops */
  if (tcc_state->opt_bool_cse && tcc_ir_opt_cse_bool(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up unused ops */

  /* Idempotent boolean simplification: BOOL_OP(x, x) -> x */
  if (tcc_state->opt_bool_idempotent && tcc_ir_opt_bool_idempotent(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up unused ops */

  /* Boolean expression simplification - eliminate redundant BOOL_OR/BOOL_AND */
  if (tcc_state->opt_bool_simplify && tcc_ir_opt_bool_simplify(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up unused ops */

  /* Return value optimization - fold LOAD -> RETURNVALUE */
  if (tcc_state->opt_return_value && tcc_ir_opt_return(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up unused ops */

  /* Phase 4: Store-Load Forwarding - replace loads from recently stored addresses
   * CONSERVATIVE: Only handles stack locals whose address is not taken.
   * DISABLED for nested functions with static chain: chain-relative captured
   * variable offsets can numerically match FP-relative local variable offsets,
   * causing the forwarding to confuse aliased values. */
  if (tcc_state->opt_store_load_fwd && !ir->has_static_chain && tcc_ir_opt_sl_forward(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up forwarded loads */

  /* Phase 4: Redundant Store Elimination - remove stores overwritten before read
   * CONSERVATIVE: Only handles stack locals whose address is not taken */
  if (tcc_state->opt_redundant_store && tcc_ir_opt_store_redundant(ir))
    if (tcc_state->opt_dce)
      tcc_ir_opt_dce(ir); /* Clean up dead stores */

  /* Dead store elimination - remove unused ASSIGN instructions */
  if (tcc_state->opt_dead_store)
    tcc_ir_opt_dse(ir);

  /* Phase 5: Loop-Invariant Code Motion - DISABLED
   * The LICM pass has a bug in hoist_const_exprs_from_loop(): instruction
   * indices are not adjusted by total_inserted when reading original
   * instructions during the insertion loop, causing operand_base corruption.
   * This produces invalid loop structures that crash IV strength reduction.
   * TODO: re-enable after the index fix in licm.c is validated. */
  IRLoops *licm_loops = NULL;
#if 0
  if (tcc_state->opt_licm)
    licm_loops = tcc_ir_opt_licm_ex(ir);
#endif

  /* Phase 6: Induction Variable Strength Reduction - transform array indexing
   * from: base + i*stride (SHL + ADD each iteration)
   * to:   ptr += stride (single ADD, enabling post-increment addressing)
   * Uses loop structure from LICM to avoid re-detection index mismatch. */
  if (tcc_state->opt_iv_strength_red)
  {
    if (licm_loops)
      tcc_ir_opt_iv_strength_reduction_with_loops(ir, licm_loops);
    else
      tcc_ir_opt_iv_strength_reduction(ir);
  }
  tcc_ir_free_loops(licm_loops);

  /* Phase 7: Strength Reduction - transform MUL by constant to shift/add */
  if (tcc_state->opt_strength_red)
    tcc_ir_opt_strength_reduction(ir);

  tcc_ir_opt_dce(ir); /* Final pass to mark unreachable code as NOP */

  /* Recompute leafness after IR optimizations.
   * IR construction marks the function non-leaf as soon as a call op is
   * emitted, but DCE/other passes can delete calls.
   *
   * Complex FP operations (FADD/FSUB/FMUL/FDIV on complex operands) are
   * also non-leaf: they expand to __aeabi_f* calls during code generation.
   */
  {
    ir->leaffunc = 1;
    for (int i = 0; i < ir->next_instruction_index; ++i)
    {
      const IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        ir->leaffunc = 0;
        break;
      }
      /* Complex FP ops expand to soft-float BL calls during codegen */
      if (q->op == TCCIR_OP_FADD || q->op == TCCIR_OP_FSUB || q->op == TCCIR_OP_FMUL || q->op == TCCIR_OP_FDIV)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (dest.is_complex)
        {
          ir->leaffunc = 0;
          break;
        }
      }
    }
  }

  nocode_wanted = 0;

  /* reset local stack */
  pop_local_syms(NULL, 0);

  /* Nested calls are now handled at code generation time via backward scan.
   * No IR reordering needed - saves O(n) memory allocations. */

  tcc_ir_liveness_analysis(ir);

  /* Mark return value vregs with incoming_reg0=0 BEFORE allocation
   * so the allocator knows they arrive in r0 and can optimize accordingly */
  tcc_ir_mark_return_value_incoming_regs(ir);

  /* TODO: track float_parameters_count separately for hard float ABI */
  tcc_ls_allocate_registers(&ir->ls, ir->parameters_count, 0, loc);

  /* Reset scratch register cache before codegen */
  tcc_ls_reset_scratch_cache(&ir->ls);

  /* Stack-passed params already live in the incoming argument area.
   * If linear-scan spilled them, drop the local spill slot so we don't bloat
   * the frame or emit pointless prologue copies (e.g. sum40).
   * Must run before we extend `loc` based on spill slots.
   */
  tcc_ir_avoid_spilling_stack_passed_params(ir);

  /* We may have removed a lot of spill slots (stack-passed params). Repack the
   * remaining spill slots so other spills don't keep huge negative offsets. */
  tcc_ls_compact_stack_locations(&ir->ls, loc);

  /* Make sure the final stack frame is large enough for any spill slots.
   * The linear-scan allocator assigns negative FP-relative stack locations;
   * extend `loc` to the most-negative one so spills don't overlap locals.
   */
  {
    int min_stack_loc = 0;
    for (int i = 0; i < ir->ls.next_interval_index; ++i)
    {
      int sl = ir->ls.intervals[i].stack_location;
      if (sl < min_stack_loc)
        min_stack_loc = sl;
    }
    if (min_stack_loc < loc)
      loc = min_stack_loc;
  }

  tcc_ir_patch_live_intervals_registers(ir);
  tcc_ir_register_allocation_params(ir);
  tcc_ir_build_stack_layout(ir);

  /* Compile nested functions AFTER parent's register allocation.
   * At this point, captured variables have their final stack locations
   * assigned by the register allocator (since they're addrtaken, they're spilled).
   * Nested function code is emitted into .text BEFORE the parent's code. */
  if (tcc_state->nb_nested_funcs > 0)
  {
    /* Resolve captured variable offsets from parent's register allocation */
    for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
    {
      NestedFunc *nf = &tcc_state->nested_funcs[i];
      for (int j = 0; j < nf->nb_captured; j++)
      {
        int vreg = nf->captured_vregs[j];
        if (vreg >= 0)
        {
          /* Get the stack location assigned by register allocator */
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
          if (interval && interval->allocation.offset != 0)
            nf->captured_offsets[j] = interval->allocation.offset;
        }
      }
    }
    compile_nested_functions(sym);

    /* Update parent's func_ind and ELF symbol to point after nested function code.
     * ind is now past the nested functions' machine code (not restored). */
    func_ind = ind;
    put_extern_sym(sym, cur_text_section, ind + 1, 0);
  }

  tcc_ir_codegen_generate(ir);
  if (!sym->a.naked)
  {
    tcc_debug_prolog_epilog(tcc_state, 1);
    // gfunc_epilog();
  }

#ifdef CONFIG_TCC_DEBUG
  if (tcc_state->dump_ir)
  {
    tcc_ir_dump_set_show_physical_regs(1); /* Show physical registers with virtual register info */
    printf("=== IR AFTER OPTIMIZATIONS ===\n");
    tcc_ir_show(ir);
    printf("=== END IR AFTER OPTIMIZATIONS ===\n");
  }
#endif

  /* Infer and cache function purity for LICM optimization
   * This allows LICM to hoist calls to pure functions defined in the same TU */
  if (tcc_state->opt_licm && ir && sym)
  {
    /* Forward declare the inference function */
    extern TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState * ir, Sym * func_sym);
    extern void tcc_ir_cache_func_purity(TCCState * s, int func_token, TCCFuncPurity purity);

    TCCFuncPurity purity = tcc_ir_infer_func_purity(ir, sym);
    tcc_ir_cache_func_purity(tcc_state, sym->v, purity);
  }

  /* end of function */
  tcc_debug_funcend(tcc_state, ind - func_ind);

  /* patch symbol size */
  elfsym(sym)->st_size = ind - func_ind;

  cur_text_section->data_offset = ind;
  local_scope = 0;
  /* Only pop labels defined in this function - use saved stack position */
  label_pop(&global_label_stack, global_label_stack_start, 0);
  if (ir && ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }
  sym_pop(&all_cleanups, NULL, 0);

  /* It's better to crash than to generate wrong code */
  cur_text_section = NULL;
  funcname = "";       /* for safety */
  func_vt.t = VT_VOID; /* for safety */
  func_var = 0;        /* for safety */
  ind = 0;             /* for safety */
  func_ind = -1;
  nocode_wanted = DATA_ONLY_WANTED;
  check_vstack();

  /* do this after funcend debug info */
  next();
  tcc_ir_free(ir);
  tcc_state->ir = NULL;
}

static void gen_inline_functions(TCCState *s)
{
  Sym *sym;
  int inline_generated, i;
  struct InlineFunc *fn;

  tcc_open_bf(s, ":inline:", 0);
  /* iterate while inline function are referenced */
  do
  {
    inline_generated = 0;
    for (i = 0; i < s->nb_inline_fns; ++i)
    {
      fn = s->inline_fns[i];
      sym = fn->sym;
      if (sym && (sym->c || !(sym->type.t & VT_INLINE)))
      {
        /* the function was used or forced (and then not internal):
           generate its code and convert it to a normal function */
        fn->sym = NULL;
        tccpp_putfile(fn->filename);
        begin_macro(fn->func_str, 1);
        next();
        if (s->function_sections)
        {
          /* -ffunction-sections: create .text.funcname section */
          /* Merged: use .text instead of .text.funcname to reduce section count */
          cur_text_section = text_section;
        }
        else
        {
          cur_text_section = text_section;
        }
        gen_function(sym);
        end_macro();

        inline_generated = 1;
      }
    }
  } while (inline_generated);
  tcc_close();
}

static void free_inline_functions(TCCState *s)
{
  int i;
  /* free tokens of unused inline functions */
  for (i = 0; i < s->nb_inline_fns; ++i)
  {
    struct InlineFunc *fn = s->inline_fns[i];
    if (fn->sym)
      tok_str_free(fn->func_str);
  }
  dynarray_reset(&s->inline_fns, &s->nb_inline_fns);
}

static void do_Static_assert(void)
{
  int c;
  const char *msg;

  next();
  skip('(');
  c = expr_const();
  msg = "_Static_assert fail";
  if (tok == ',')
  {
    next();
    msg = parse_mult_str("string constant")->data;
  }
  skip(')');
  if (c == 0)
    tcc_error("%s", msg);
  skip(';');
}

/* 'l' is VT_LOCAL or VT_CONST to define default storage type
   or VT_CMP if parsing old style parameter list
   or VT_JMP if parsing c99 for decl: for (int i = 0, ...) */
static int decl(int l)
{
  int v, has_init, r, oldint;
  CType type, btype;
  Sym *sym;
  AttributeDef ad, adbase;
  ElfSym *esym;

  while (1)
  {

    oldint = 0;
    if (!parse_btype(&btype, &adbase, l == VT_LOCAL))
    {
      if (l == VT_JMP)
        return 0;
      /* skip redundant ';' if not in old parameter decl scope */
      if (tok == ';' && l != VT_CMP)
      {
        next();
        continue;
      }
      if (tok == TOK_STATIC_ASSERT)
      {
        do_Static_assert();
        continue;
      }
      if (l != VT_CONST)
        break;
      if (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3)
      {
        /* global asm block */
        asm_global_instr();
        continue;
      }
      if (tok >= TOK_UIDENT)
      {
        /* special test for old K&R protos without explicit int
           type. Only accepted when defining global data */
        btype.t = VT_INT;
        oldint = 1;
      }
      else
      {
        if (tok != TOK_EOF)
          expect("declaration");
        break;
      }
    }

    if (tok == ';')
    {
      if ((btype.t & VT_BTYPE) == VT_STRUCT)
      {
        v = btype.ref->v;
        if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) >= SYM_FIRST_ANOM)
          tcc_warning("unnamed struct/union that defines no instances");
        next();
        continue;
      }
      if (IS_ENUM(btype.t))
      {
        next();
        continue;
      }
    }

    while (1)
    { /* iterate thru each declaration */
      type = btype;
      ad = adbase;
      type_decl(&type, &ad, &v, TYPE_DIRECT);
#if 0
            {
                char buf[500];
                type_to_str(buf, sizeof(buf), &type, get_tok_str(v, NULL));
                printf("type = '%s'\n", buf);
            }
#endif
      if ((type.t & VT_BTYPE) == VT_FUNC)
      {
        if ((type.t & VT_STATIC) && (l != VT_CONST))
          tcc_error("function without file scope cannot be static");
        /* if old style function prototype, we accept a
           declaration list */
        sym = type.ref;
        if (sym->f.func_type == FUNC_OLD && l == VT_CONST)
        {
          func_vt = type;
          decl(VT_CMP);
        }

        if ((type.t & (VT_EXTERN | VT_INLINE)) == (VT_EXTERN | VT_INLINE))
        {
          /* always_inline functions must be handled as if they
             don't generate multiple global defs, even if extern
             inline, i.e. GNU inline semantics for those.  Rewrite
             them into static inline.  */
          if (tcc_state->gnu89_inline || sym->f.func_alwinl)
            type.t = (type.t & ~VT_EXTERN) | VT_STATIC;
          else
            type.t &= ~VT_INLINE; /* always compile otherwise */
        }
      }
      else if (oldint)
      {
        tcc_warning("type defaults to int");
      }

      if (gnu_ext && (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3))
      {
        ad.asm_label = asm_label_instr();
        /* parse one last attribute list, after asm label */
        parse_attribute(&ad);
#if 0
                /* gcc does not allow __asm__("label") with function definition,
                   but why not ... */
                if (tok == '{')
                    expect(";");
#endif
      }

#ifdef TCC_TARGET_PE
      if (ad.a.dllimport || ad.a.dllexport)
      {
        if (type.t & VT_STATIC)
          tcc_error("cannot have dll linkage with static");
        if (type.t & VT_TYPEDEF)
        {
          tcc_warning("'%s' attribute ignored for typedef",
                      ad.a.dllimport ? (ad.a.dllimport = 0, "dllimport") : (ad.a.dllexport = 0, "dllexport"));
        }
        else if (ad.a.dllimport)
        {
          if ((type.t & VT_BTYPE) == VT_FUNC)
            ad.a.dllimport = 0;
          else
            type.t |= VT_EXTERN;
        }
      }
#endif
      if (tok == '{')
      {
        if ((type.t & VT_BTYPE) != VT_FUNC)
          expect("function definition");

        /* reject abstract declarators in function definition
           make old style params without decl have int type */
        sym = type.ref;
        while ((sym = sym->next) != NULL)
        {
          if (!(sym->v & ~SYM_FIELD))
            expect("identifier");
          if (sym->type.t == VT_VOID)
            sym->type = int_type;
        }

        /* apply post-declaraton attributes */
        merge_funcattr(&type.ref->f, &ad.f);

        if (l == VT_LOCAL)
        {
          /* ── nested function definition ── */

          /* Grow nested funcs array if needed */
          if (tcc_state->nb_nested_funcs >= tcc_state->nested_funcs_capacity)
          {
            tcc_state->nested_funcs_capacity =
                tcc_state->nested_funcs_capacity ? tcc_state->nested_funcs_capacity * 2 : 4;
            tcc_state->nested_funcs =
                tcc_realloc(tcc_state->nested_funcs, tcc_state->nested_funcs_capacity * sizeof(NestedFunc));
          }

          /* Get pointer to new nested func slot */
          NestedFunc *nf = &tcc_state->nested_funcs[tcc_state->nb_nested_funcs];
          memset(nf, 0, sizeof(*nf));

          /* Store filename for later */
          pstrncpy(nf->filename, file->filename, sizeof(nf->filename));

          /* Push symbol into LOCAL scope so parent body can reference it */
          /* Use external_sym to get proper type with valid parameter symbols */
          type.t &= ~VT_EXTERN;
          nf->sym = external_sym(v, &type, 0, &ad);
          /* Mark as nested function for static chain handling.
           * Note: This flag MUST be set on the symbol returned by external_sym
           * because that's the symbol that sym_find will return when looking
           * up the function name in the parent body. */
          nf->sym->a.nested_func = 1;
          /* Make nested function STB_LOCAL (not global) */
          nf->sym->type.t |= VT_STATIC;
          /* Name mangling: use GCC convention "funcname.N" */
          {
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s.%d", get_tok_str(v, NULL), tcc_state->nb_nested_funcs);
            nf->sym->asm_label = tok_alloc(mangled, strlen(mangled))->tok;
          }

          /* Create placeholder address for the function */
          put_extern_sym(nf->sym, cur_text_section, 0, 0);

          /* Save the token stream (function body only, not parameters) */
          skip_or_save_block(&nf->func_str);

          /* Pre-scan to identify captured parent variables.
           * If we're inside a nested function's gen_function, current_nested_func
           * is the parent. Pass it explicitly for multi-level nesting. */
          prescan_captured_vars(nf, local_stack, tcc_state->current_nested_func);

          /* Increment count */
          tcc_state->nb_nested_funcs++;

          /* Continue parsing parent body - nested func saved */
          break;
        }
        else if (l != VT_CONST)
        {
          tcc_error("cannot use local functions");
        }

        /* put function symbol */
        type.t &= ~VT_EXTERN;
        sym = external_sym(v, &type, 0, &ad);

        /* static inline functions are just recorded as a kind
           of macro. Their code will be emitted at the end of
           the compilation unit only if they are used */
        if (sym->type.t & VT_INLINE)
        {
          struct InlineFunc *fn;
          fn = tcc_malloc(sizeof *fn + strlen(file->filename));
          strcpy(fn->filename, file->filename);
          fn->sym = sym;
          dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
          skip_or_save_block(&fn->func_str);
        }
        else
        {
          /* compute text section */
          cur_text_section = ad.section;
          if (!cur_text_section)
          {
            if (tcc_state->function_sections)
            {
              /* -ffunction-sections: create .text.funcname section */
              /* Merged: use .text instead of .text.funcname to reduce section count */
              cur_text_section = text_section;
            }
            else
            {
              cur_text_section = text_section;
            }
          }
          else if (cur_text_section->sh_num > bss_section->sh_num)
            cur_text_section->sh_flags = text_section->sh_flags;
          gen_function(sym);
          /* Nested functions are now compiled inside gen_function,
           * before pop_local_syms, so parent locals are still accessible. */
        }
        break;
      }
      else
      {
        if (l == VT_CMP)
        {
          /* find parameter in function parameter list */
          for (sym = func_vt.ref->next; sym; sym = sym->next)
            if ((sym->v & ~SYM_FIELD) == v)
              goto found;
          tcc_error("declaration for parameter '%s' but no such parameter", get_tok_str(v, NULL));
        found:
          if (type.t & VT_STORAGE) /* 'register' is okay */
            tcc_error("storage class specified for '%s'", get_tok_str(v, NULL));
          if (sym->type.t != VT_VOID)
            tcc_error("redefinition of parameter '%s'", get_tok_str(v, NULL));
          convert_parameter_type(&type);
          sym->type = type;
        }
        else if (type.t & VT_TYPEDEF)
        {
          /* save typedefed type  */
          /* XXX: test storage specifiers ? */
          sym = sym_find(v);
          if (sym && sym->sym_scope == local_scope)
          {
            if (!is_compatible_types(&sym->type, &type) || !(sym->type.t & VT_TYPEDEF))
              tcc_error("incompatible redefinition of '%s'", get_tok_str(v, NULL));
            sym->type = type;
          }
          else
          {
            sym = sym_push(v, &type, 0, 0);
          }
          sym->a = ad.a;
          if ((type.t & VT_BTYPE) == VT_FUNC)
            merge_funcattr(&sym->type.ref->f, &ad.f);
          if (debug_modes)
            tcc_debug_typedef(tcc_state, sym);
        }
        else if ((type.t & VT_BTYPE) == VT_VOID && !(type.t & VT_EXTERN))
        {
          tcc_error("declaration of void object");
        }
        else
        {
          r = 0;
          if ((type.t & VT_BTYPE) == VT_FUNC)
          {
            /* external function definition */
            /* specific case for func_call attribute */
            merge_funcattr(&type.ref->f, &ad.f);
          }
          else if (!(type.t & VT_ARRAY))
          {
            /* not lvalue if array */
            r |= VT_LVAL;
          }
          has_init = (tok == '=');
          if (has_init && (type.t & VT_VLA))
            tcc_error("variable length array cannot be initialized");

          if (((type.t & VT_EXTERN) && (!has_init || l != VT_CONST)) ||
              (type.t & VT_BTYPE) == VT_FUNC
              /* as with GCC, uninitialized global arrays with no size
                 are considered extern: */
              || ((type.t & VT_ARRAY) && !has_init && l == VT_CONST && type.ref->c < 0))
          {
            /* external variable or function */
            type.t |= VT_EXTERN;
            external_sym(v, &type, r, &ad);
          }
          else
          {
            if (l == VT_CONST || (type.t & VT_STATIC))
              r |= VT_CONST;
            else
              r |= VT_LOCAL;
            if (has_init)
              next();
            else if (l == VT_CONST)
              /* uninitialized global variables may be overridden */
              type.t |= VT_EXTERN;
            decl_initializer_alloc(&type, &ad, r, has_init, v, l == VT_CONST);
          }

          if (ad.alias_target && l == VT_CONST)
          {
            /* Aliases need to be emitted when their target symbol
               is emitted, even if perhaps unreferenced.
               We only support the case where the base is already
               defined, otherwise we would need deferring to emit
               the aliases until the end of the compile unit.  */
            esym = elfsym(sym_find(ad.alias_target));
            if (!esym)
              tcc_error("unsupported forward __alias__ attribute");
            put_extern_sym2(sym_find(v), esym->st_shndx, esym->st_value, esym->st_size, 1);
          }
        }
        if (tok != ',')
        {
          if (l == VT_JMP)
            return 1;
          skip(';');
          break;
        }
        next();
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
#undef gjmp_addr
#undef gjmp
/* ------------------------------------------------------------------------- */
