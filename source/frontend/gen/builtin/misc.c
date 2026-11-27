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

/* misc.c -- Builtin classification, bit operations and IR call-argument emission.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* GCC __builtin_classify_type return values (C mode) */
#define GCC_TYPE_CLASS_VOID 0
#define GCC_TYPE_CLASS_INTEGER 1
#define GCC_TYPE_CLASS_POINTER 5
#define GCC_TYPE_CLASS_REAL 8
#define GCC_TYPE_CLASS_COMPLEX 9
#define GCC_TYPE_CLASS_FUNCTION 10
#define GCC_TYPE_CLASS_STRUCT 12
#define GCC_TYPE_CLASS_UNION 13
#define GCC_TYPE_CLASS_ARRAY 14
#define GCC_TYPE_CLASS_VECTOR 18

int gcc_classify_type(CType *type)
{
  int bt = type->t & VT_BTYPE;
  int t = type->t;

  switch (bt)
  {
  case VT_VOID:
    return GCC_TYPE_CLASS_VOID;

  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
  case VT_BOOL:
    return GCC_TYPE_CLASS_INTEGER;

  case VT_PTR:
    if (t & VT_ARRAY)
      return GCC_TYPE_CLASS_ARRAY;
    return GCC_TYPE_CLASS_POINTER;

  case VT_FUNC:
    return GCC_TYPE_CLASS_FUNCTION;

  case VT_STRUCT:
    if (IS_UNION(t))
      return GCC_TYPE_CLASS_UNION;
    return GCC_TYPE_CLASS_STRUCT;

  case VT_FLOAT:
  case VT_DOUBLE:
  case VT_LDOUBLE:
    if (t & VT_COMPLEX)
      return GCC_TYPE_CLASS_COMPLEX;
    return GCC_TYPE_CLASS_REAL;

  default:
    return GCC_TYPE_CLASS_INTEGER; /* fallback */
  }
}

/* Emit a single-operand bit-manipulation IR op (TCCIR_OP_CLZ / RBIT / REV /
 * REV16) consuming the 32-bit value on top of the vstack and replacing it with
 * the (unsigned int) result.  Only valid when tcc_machine_has_bit_ops(). */
void gen_bitop1(TccIrOp op)
{
  SValue dest;
  svalue_init(&dest);
  dest.type.t = VT_INT | VT_UNSIGNED;
  dest.type.ref = NULL;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, op, vtop, NULL, &dest);
  vtop->type.t = VT_INT | VT_UNSIGNED;
  vtop->type.ref = NULL;
  vtop->vr = dest.vr;
  vtop->r = 0;
  vtop->c.i = 0; /* Clear c.i to avoid corrupting later operations */
}

/* Push this function's return address, __builtin_return_address(0), as a
 * void *.  The prologue must then save LR even in a leaf; the value is read
 * back from that slot (TCCIR_OP_RETURN_ADDRESS), with no frame pointer. */
ST_FUNC void vpush_return_address(void)
{
  CType type;
  SValue dest;
  type.t = VT_VOID;
  type.ref = NULL;
  mk_pointer(&type);
  tcc_state->force_lr_save = 1;
  svalue_init(&dest);
  dest.type = type;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_RETURN_ADDRESS, NULL, NULL, &dest);
  vpush(&type);
  vtop->vr = dest.vr;
  vtop->r = 0;
  vtop->c.i = 0;
}

/* Emit an IR function call to a library helper for a builtin.
 * Arguments are already on the vstack (1 or 2 args).
 * func_tok: TOK_xxx or tok_alloc_const("name") for the target function
 * argc: number of arguments (1 or 2), already on vstack
 * ret_type: VT_INT, VT_FLOAT, VT_DOUBLE, etc.
 * Pops argc args from vstack, pushes the result. */
void gen_builtin_libcall(int func_tok, int argc, int ret_type)
{
  const int new_call_id = tcc_state->ir->next_call_id++;
  SValue param_num;
  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;

  for (int i = 0; i < argc; i++)
  {
    param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, i);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[i - (argc - 1)], &param_num, NULL);
  }

  vpush_helper_func(func_tok);

  SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, argc);
  SValue dest;
  svalue_init(&dest);
  dest.type.t = ret_type;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);

  vtop -= (argc + 1); /* pop func + args */
  vpushi(0);
  vtop->type.t = ret_type;
  vtop->vr = dest.vr;
  vtop->r = TREG_R0;
}

/* Emit an IR function call with arguments from an SValue array (not from vstack).
 * args[0..argc-1] are the arguments.
 * Pushes the result onto the vstack with the given return type; returns the
 * call's id. */
int gen_ir_call_args(SValue *args, int argc, int func_tok, CType *ret_ctype)
{
  const int new_call_id = tcc_state->ir->next_call_id++;
  SValue param_num;
  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;

  for (int i = 0; i < argc; i++)
  {
    param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, i);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &args[i], &param_num, NULL);
  }

  vpush_helper_func(func_tok);

  SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, argc);
  SValue dest;
  svalue_init(&dest);
  dest.type = *ret_ctype;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);

  --vtop; /* pop function */
  vpushi(0);
  vtop->type = dest.type;
  vtop->vr = dest.vr;
  vtop->r = TREG_R0;
  return new_call_id;
}

/* ------------------------------------------------------------------------ */
/* @min(a, b) and @max(a, b): short builtins, in C and in yasc alike.
 *
 * Unlike a MIN() macro each operand is evaluated once, left to right, and
 * unlike `a < b ? a : b` a signed operand that may be negative does not
 * silently turn into a huge unsigned value: mixing one with an unsigned
 * operand is an error (a non-negative constant is fine, so
 * @min(buflen, PATH_MAX) with a size_t buflen is accepted).  The result has
 * the usual arithmetic type of the two.  Two constants fold to a constant.
 *
 * Integers: the compare, branch and assign `?:` emits, which if-conversion
 * turns into cmp/it/mov.  Floating point: TCCIR_OP_FMIN/FMAX with fmin's NaN
 * rule (the other operand wins): one vminnm.f32/vmaxnm.f32 on an FPv5 FPU
 * (RP2350 user code), a call to fminf/fmin otherwise and for doubles.
 *
 * A regular compiler has no '@' token and no #define can make one: code that
 * gcc also builds spells it through a macro that becomes @min under tcc. */

static int at_operand_maybe_negative(SValue *sv)
{
  int t = sv->type.t;
  int bt = t & VT_BTYPE;
  if (bt == VT_BOOL || (t & VT_UNSIGNED))
    return 0;
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int64_t v = (bt == VT_LLONG) ? (int64_t)sv->c.i : (int64_t)(int32_t)sv->c.i;
    return v < 0;
  }
  return 1;
}

/* the operand on vtop, evaluated now: a constant stays a constant, anything
 * else lands in a temp of its own, so the other operand cannot change it */
static void at_operand(const char *name)
{
  int bt = vtop->type.t & VT_BTYPE;
  if ((vtop->type.t & VT_ARRAY) || !(is_integer_btype(bt) || bt == VT_BOOL || is_float(vtop->type.t)))
  {
    char buf[256];
    type_to_str(buf, sizeof buf, &vtop->type, NULL);
    tcc_error("@%s wants numbers, not '%s'", name, buf);
  }
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST || nocode_wanted)
    return;
  gv(RC_TYPE(vtop->type.t));
  if (vtop->vr >= 0 && TCCIR_DECODE_VREG_TYPE(vtop->vr) != TCCIR_VREG_TYPE_TEMP)
  {
    SValue src = *vtop, dest;
    svalue_init(&dest);
    dest.type = vtop->type;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
    vtop->vr = dest.vr;
  }
}

/* cast vtop[-1] (the first operand) to TYPE */
static void at_cast_first(CType *type)
{
  vswap();
  gen_cast(type);
  vswap();
}

/* RV = the value SV holds */
static void at_copy_to(SValue *sv, int rv, int rc)
{
  SValue dest;
  vpushv(sv);
  gv(rc);
  svalue_init(&dest);
  dest.vr = rv;
  dest.type = vtop->type;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
  vpop();
}

static __attribute__((noinline)) void at_minmax(int is_max)
{
  const char *name = is_max ? "max" : "min";
  CType type;
  SValue a, b;

  skip('(');
  expr_eq();
  at_operand(name);
  skip(',');
  expr_eq();
  at_operand(name);
  skip(')');

  if (!combine_types(&type, &vtop[-1], vtop, '?'))
    type_incompatibility_error(&vtop[-1].type, &vtop->type, "@min/@max of '%s' and '%s'");
  type.t &= ~(VT_CONSTANT | VT_VOLATILE | VT_DEFSIGN);
  if (is_integer_btype(type.t & VT_BTYPE) && (type.t & VT_UNSIGNED))
  {
    for (int i = -1; i <= 0; i++)
      if (at_operand_maybe_negative(&vtop[i]))
      {
        char sb[256], ub[256];
        type_to_str(sb, sizeof sb, &vtop[i].type, NULL);
        type_to_str(ub, sizeof ub, &vtop[i ? 0 : -1].type, NULL);
        tcc_error("@%s of signed '%s' and unsigned '%s': a negative value would compare as a huge one; cast "
                  "one of them",
                  name, sb, ub);
      }
  }

  at_cast_first(&type);
  gen_cast(&type);
  a = vtop[-1];
  b = vtop[0];

  /* two constants: fold */
  if ((a.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && (b.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int bt = type.t & VT_BTYPE, pick_a;
    if (is_float(type.t))
    {
      double x = bt == VT_FLOAT ? a.c.f : bt == VT_DOUBLE ? a.c.d : (double)a.c.ld;
      double y = bt == VT_FLOAT ? b.c.f : bt == VT_DOUBLE ? b.c.d : (double)b.c.ld;
      pick_a = y != y ? 1 : x != x ? 0 : is_max ? x >= y : x <= y;
    }
    else if (type.t & VT_UNSIGNED)
    {
      uint64_t x = a.c.i, y = b.c.i;
      if (bt != VT_LLONG)
        x = (uint32_t)x, y = (uint32_t)y;
      pick_a = is_max ? x >= y : x <= y;
    }
    else
    {
      int64_t x = a.c.i, y = b.c.i;
      if (bt != VT_LLONG)
        x = (int32_t)x, y = (int32_t)y;
      pick_a = is_max ? x >= y : x <= y;
    }
    vtop -= 2;
    vpushv(pick_a ? &a : &b);
    return;
  }

  if (nocode_wanted)
  {
    /* sizeof, typeof, a dead arm: the type is all that is wanted.  Not a
     * constant, though: a static initializer is parsed with nocode_wanted
     * too, and `static int x = @min(g, 3);` must say "not constant" rather
     * than store 3. */
    vtop -= 2;
    vpushv(&b);
    vtop->r = 0;
    return;
  }

  if (is_float(type.t))
  {
    /* TCCIR_OP_FMIN/FMAX: vminnm.f32 inline on an FPv5 FPU, else put as a
     * call to fminf/fmin (ir/gen/softfloat.c) */
    int is_double = (type.t & VT_BTYPE) != VT_FLOAT;
    SValue dest;
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.r = 0;
    dest.type = type;
    tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, is_double);
    tcc_ir_put(tcc_state->ir, is_max ? TCCIR_OP_FMAX : TCCIR_OP_FMIN, &vtop[-1], &vtop[0], &dest);
    vtop -= 2;
    vpushv(&dest);
    return;
  }

  /* the shape `a < b ? a : b` takes in expr_cond_ternary -- a copy into the
   * result in each arm -- so the same if-conversion makes it cmp/it/mov
   * (with the false arm empty it stays a branch):
   *     cmp a, b; jump-if-false F; jmp T
   *   F: r = b; jmp J
   *   T: r = a
   *   J: */
  int rc = RC_TYPE(type.t), tt, u;
  int rv = tcc_ir_get_vreg_temp(tcc_state->ir);
  vpushv(&a);
  vpushv(&b);
  gen_op(is_max ? TOK_GT : TOK_LT); /* '<' is TOK_SHL */
  tt = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
  u = gjmp(-1);
  tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  vtop -= 2; /* the operands: values in temps or constants, nothing to free */
  at_copy_to(&b, rv, rc);
  tt = gjmp(-1);
  tcc_ir_backpatch_to_here(tcc_state->ir, u);
  at_copy_to(&a, rv, rc);
  tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  vpushv(&b);
  vtop->r = 0;
  vtop->vr = rv;
}

/* ---- @src(): where the code is --------------------------------------
 *
 *   printf("%s:%u: %s\n", @src().file, @src().line, @src().func);
 *   void log_warn(const char *msg, yasc_src at = @src());
 *
 * @src() is a value of type struct __yasc_src (yasc_src in <yasc_src.h>),
 * two words passed in two registers:
 *
 *   __fn    the function's header, a const struct __yasc_src_fn in .rodata,
 *           one per function: file (as __FILE__), func (as __func__, "" at
 *           file scope), module (the yasc module, "" outside one)
 *   line    the line, as __LINE__
 *
 * at.file / at.func / at.module read through __fn (yasc_src_header_member).
 * A call that passes @src() costs the line as an immediate and the header's
 * address, which every call in the function shares: nothing per call in
 * .rodata.  A yasc_src is copied as any struct.
 *
 * The header's names are words holding the distance from the word itself to
 * the string (VT_SELF_REL), filled in by the linker (a PC-relative
 * relocation): no load-time relocation, so the header stays in the shared
 * .rodata.  An ordinary pointer there would put it into each process's .data
 * on YasOS (-share-rodata), and a __rodata_relative one would mean nothing in
 * another module (libc.so reading the application's header).  The header is
 * never copied, so no object has its type (yasc_src_object_check).  The
 * value's __fn is an address computed in code, not stored in data -- which is
 * also why @src() has no static form: `static yasc_src g = @src();` would
 * need a load-time relocation.  @src().NAME written out makes nothing at all
 * (src_member).  Default arguments (decl/default_arg.c) make @src() the
 * caller's location. */

static Sym *src_tag, *src_fn_tag;
/* the names' ELF symbols, by .rodata offset (src_name_sym) */
typedef struct SrcName
{
  addr_t off;
  int elf;
} SrcName;
static SrcName *src_names;
static int nb_src_names;
/* the headers, by their names' .rodata offsets (src_header_sym) */
typedef struct SrcHead
{
  addr_t names[3], off;
  Sym *sym; /* one symbol, so a function's calls share its literal pool word */
} SrcHead;
static SrcHead *src_heads;
static int nb_src_heads;

/* a pointer @src() hands out: a view of .rodata, which outlives
   everything -- `* borrows(static)`, so it may be kept and returned */
static void src_pointer(CType *t)
{
  t->ref = sym_pointer_node_b(t, YASC_VIEW, YASC_BORROWS_STATIC);
  t->t = VT_PTR | (t->t & VT_STORAGE);
}

/* struct TAG { FIELDS }, on global_stack */
static Sym *src_struct(int tag, int nf, const char *const *names, CType *types)
{
  Sym *s, **ps;
  CType type = {VT_STRUCT, NULL};
  AttributeDef ad;
  s = sym_push(tag | SYM_STRUCT, &type, 0, -1);
  s->r = 0;
  ps = &s->next;
  for (int i = 0; i < nf; i++)
  {
    Sym *f = sym_push(tok_alloc(names[i], strlen(names[i]))->tok | SYM_FIELD, &types[i], 0, 0);
    *ps = f;
    ps = &f->next;
  }
  type.ref = s;
  memset(&ad, 0, sizeof ad);
  struct_layout(&type, &ad);
  return s;
}

/* struct __yasc_src and its header, made at file scope the first time one
   is named */
Sym *yasc_src_tag(void)
{
  static const char *const hnames[] = {"file", "func", "module"};
  static const char *const vnames[] = {"__fn", "line"};
  Sym *saved_stack = local_stack, *above = global_stack;
  int saved_scope = local_scope;
  CType ht[3], vt[2];

  if (src_tag)
    return src_tag;
  /* on global_stack whatever block the first use is in: the types outlive
     it -- and under an open parameter list, which pops what is above it */
  local_stack = NULL;
  local_scope = 0;
  for (int i = 0; i < 3; i++)
  {
    ht[i] = char_type;
    ht[i].t |= VT_CONSTANT;
    src_pointer(&ht[i]);
    ht[i].t |= VT_SELF_REL | VT_CONSTANT;
  }
  src_fn_tag = src_struct(TOK___yasc_src_fn, 3, hnames, ht);
  vt[0].t = VT_STRUCT | VT_CONSTANT;
  vt[0].ref = src_fn_tag;
  src_pointer(&vt[0]);
  vt[1].t = VT_INT | VT_UNSIGNED;
  vt[1].ref = NULL;
  src_tag = src_struct(TOK___yasc_src, 2, vnames, vt);
  sym_sink_to_bottom(above);
  local_stack = saved_stack;
  local_scope = saved_scope;
  return src_tag;
}

void yasc_src_reset(void)
{
  src_tag = src_fn_tag = NULL;
  tcc_free(src_names);
  src_names = NULL;
  nb_src_names = 0;
  tcc_free(src_heads);
  src_heads = NULL;
  nb_src_heads = 0;
}

/* A PC-relative word: target - &word, filled in by the linker (in the
 * linked image, not at load time: both ends are in the module's .rodata). */
#if defined TCC_TARGET_ARM
#define SRC_REL R_ARM_REL32
#elif defined TCC_TARGET_X86_64
#define SRC_REL R_X86_64_PC64
#endif

/* The ELF symbols on the names, one per string of the unit: the words are
 * relocations against them, so dropping an unreferenced static from .rodata
 * (gc_unreferenced_statics), which moves what follows it, keeps them right
 * -- a distance written by hand would not follow. */
static int src_name_sym(const char *str, addr_t *off_out)
{
  addr_t len = strlen(str) + 1, off = str_lit_pool_intern(str, len);
  Sym *sym;
  *off_out = off;
  for (int i = 0; i < nb_src_names; i++)
    if (src_names[i].off == off)
      return src_names[i].elf;
  sym = get_sym_ref(&char_type, rodata_section, off, len);
  src_names = tcc_realloc(src_names, (nb_src_names + 1) * sizeof *src_names);
  src_names[nb_src_names].off = off;
  src_names[nb_src_names].elf = sym->c;
  return src_names[nb_src_names++].elf;
}

/* the current file's, function's and module's names */
static void src_names_here(const char *names[3])
{
  BufferedFile *bf = file;
  /* a macro's or a pasted buffer's pseudo-file: the file it is in */
  while (bf && bf->filename[0] == ':' && bf->prev)
    bf = bf->prev;
  names[0] = bf ? bf->filename : "";
  names[1] = funcname ? funcname : "";
  names[2] = tcc_state->yasc_module ? tcc_state->yasc_module : "";
}

/* A symbol on the current function's header, written the first time.
 * Nothing is written where no static data is wanted (sizeof, a dead
 * branch): the symbol is then a zero-sized one, as a string literal's. */
static Sym *src_header_sym(void)
{
  CType ht = {VT_STRUCT | VT_CONSTANT, src_fn_tag};
  const char *names[3];
  addr_t offs[3], o;
  int elf[3], align, size = type_size(&ht, &align), i;
  Sym *f;

  if (NOSTATIC_WANTED)
    return get_sym_ref(&ht, rodata_section, rodata_section->data_offset, 0);
  src_names_here(names);
  for (i = 0; i < 3; i++)
    elf[i] = src_name_sym(names[i], &offs[i]);
  for (i = 0; i < nb_src_heads; i++)
    if (!memcmp(src_heads[i].names, offs, sizeof offs))
      return src_heads[i].sym;
  o = section_add(rodata_section, size, align);
  memset(rodata_section->data + o, 0, size); /* the addends: 0 */
  for (i = 0, f = src_fn_tag->next; i < 3; i++, f = f->next)
    put_elf_reloca(symtab_section, rodata_section, o + f->c, SRC_REL, elf[i], 0);
  src_heads = tcc_realloc(src_heads, (nb_src_heads + 1) * sizeof *src_heads);
  memcpy(src_heads[nb_src_heads].names, offs, sizeof offs);
  src_heads[nb_src_heads].off = o;
  {
    /* kept for the unit: on global_stack, under any open parameter list */
    Sym *saved_stack = local_stack, *above = global_stack;
    local_stack = NULL;
    src_heads[nb_src_heads].sym = get_sym_ref(&ht, rodata_section, o, size);
    sym_sink_to_bottom(above);
    local_stack = saved_stack;
  }
  return src_heads[nb_src_heads++].sym;
}

/* `@src().NAME` written out: what it reads is known here, so nothing is
 * made -- the line a constant, a name the address of its string, as __LINE__
 * and __FILE__ would be.  0 when NAME is none of them. */
static int src_member(int name, int line)
{
  static const char *const names[] = {"file", "func", "module", "line"};
  const char *n = get_tok_str(name, NULL), *here[3];
  int i;
  for (i = 0; i < 4 && strcmp(n, names[i]); i++)
    ;
  if (i == 4)
    return 0;
  if (i == 3)
  {
    CValue cv;
    CType ut = {VT_INT | VT_UNSIGNED, NULL};
    cv.i = (unsigned)line;
    vsetc(&ut, VT_CONST, &cv);
    return 1;
  }
  src_names_here(here);
  {
    CType pt = char_type;
    addr_t len = strlen(here[i]) + 1, off;
    pt.t |= VT_CONSTANT;
    src_pointer(&pt);
    if (NOSTATIC_WANTED) /* sizeof, a dead branch: the type only */
      off = rodata_section->data_offset, len = 0;
    else
      off = str_lit_pool_intern(here[i], len);
    vpushsym(&pt, get_sym_ref(&char_type, rodata_section, off, len));
  }
  return 1;
}

/* push @src() for LINE: a new value in a temporary, or MARKER, the stand-in
 * a default argument is checked with (decl/default_arg.c) */
void yasc_src_push(Sym *marker, int line)
{
  CType st;
  int align, size, vr, base;
  SValue dst;
  Sym *lf;

  st.t = VT_STRUCT;
  st.ref = yasc_src_tag();
  if (marker)
  {
    vpushsym(&st, marker);
    vtop->r |= VT_LVAL;
    return;
  }
  if (NOEVAL_WANTED)
  {
    vset(&st, VT_LOCAL | VT_LVAL, 0); /* sizeof, typeof: the type only */
    return;
  }
  if (nocode_wanted & DATA_ONLY_WANTED)
    tcc_error("@src() is made in a function: a static yasc_src would need a relocation at load time (read "
              "@src().line, .file, .func or .module here)");
  size = type_size(&st, &align);
  svalue_init(&dst);
  dst.r = VT_LOCAL | VT_LVAL;
  if (TCC_OPT(tcc_state, optimize))
  {
    /* a frame slot of its own, as a compound literal's: ra:struct_arg_split
       turns an argument built there into the two words in registers, and the
       slot goes */
    loc = tcc_ir_frame_alloc(loc, size, -align);
    tcc_ir_frame_note_type(loc, &st);
    dst.vr = -1;
    dst.c.i = loc;
  }
  else
  {
    /* -O0: one shared temporary, so the frame does not grow per call */
    dst.c.i = get_temp_local_var(size, align, &vr);
    dst.vr = vr;
  }
  base = dst.c.i;
  dst.type = src_tag->next->type; /* __fn */
  vpushv(&dst);
  vpushsym(&dst.type, src_header_sym());
  vstore();
  vpop();
  lf = src_tag->next->next; /* line */
  dst.type = lf->type;
  dst.c.i = base + lf->c;
  vpushv(&dst);
  {
    CValue cv;
    cv.i = (unsigned)line;
    vsetc(&lf->type, VT_CONST, &cv);
  }
  vstore();
  vpop();
  dst.type = st;
  dst.c.i = base;
  vpushv(&dst);
}

/* `at.file`, `.func`, `.module` of a yasc_src (vtop) named by NAME: vtop
 * becomes its header, whose member NAME the caller then reads */
int yasc_src_header_member(int name)
{
  const char *n;
  if (!src_tag || (vtop->type.t & VT_BTYPE) != VT_STRUCT || vtop->type.ref != src_tag)
    return 0;
  n = get_tok_str(name, NULL);
  if (strcmp(n, "file") && strcmp(n, "func") && strcmp(n, "module"))
    return 0;
  vtop->type = src_tag->next->type; /* __fn, at offset 0 */
  indir();
  return 1;
}

/* TYPE is the header, struct __yasc_src_fn, or an array of it */
int yasc_src_object_type(CType *type)
{
  while ((type->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
    type = &type->ref->type;
  return src_fn_tag && (type->t & VT_BTYPE) == VT_STRUCT && type->ref == src_fn_tag;
}

void yasc_src_object_check(CType *type, const char *what)
{
  if (yasc_src_object_type(type))
    tcc_error("%s of type 'struct __yasc_src_fn': a header's names are stored relative to it, so a copy would point "
              "nowhere; use yasc_src",
              what);
}

/* vtop, a self-relative lvalue (a yasc_src name), becomes the pointer it
 * names: the word's address plus the word. */
void self_rel_rvalue(void)
{
  CType ptype = vtop->type, word, bytes;

  ptype.t &= ~(VT_SELF_REL | VT_CONSTANT | VT_VOLATILE);
  word.t = PTR_SIZE == 8 ? VT_LLONG : VT_INT;
  word.ref = NULL;
  bytes.t = VT_BYTE | VT_CONSTANT;
  bytes.ref = NULL;
  mk_pointer(&bytes);
  vdup();
  vtop->type = word;
  gv(RC_INT);   /* lv, w */
  vswap();      /* w, lv */
  gaddrof();
  vtop->type = bytes; /* w, &word */
  vswap();
  gen_op('+');
  vtop->type = ptype;
}

void self_rel_addr_error(void)
{
  tcc_error("cannot take the address of a yasc_src name: its word is a distance, not a pointer");
}

/* '@' NAME ( ... ): tok is '@' */
__attribute__((noinline)) void unary_at_builtin(void)
{
  const int line = file->line_num;
  next();
  const char *name = tok >= TOK_IDENT ? get_tok_str(tok, NULL) : "";
  if (!strcmp(name, "src"))
  {
    next();
    skip('(');
    skip(')');
    if (tok == '.' && !yasc_default_marker())
    {
      next();
      if (src_member(tok, line))
      {
        next();
        return;
      }
      unget_tok('.'); /* not a name of it: the usual "no member" */
    }
    yasc_src_push(yasc_default_marker(), line);
    return;
  }
  if (!strcmp(name, "min") || !strcmp(name, "max"))
  {
    int is_max = name[1] == 'a';
    next();
    at_minmax(is_max);
    return;
  }
  tcc_error("unknown builtin '@%s' (there are @min, @max and @src)", get_tok_str(tok, &tokc));
}
