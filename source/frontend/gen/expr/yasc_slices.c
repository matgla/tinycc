/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* yasc_slices.c -- yasc's slices (docs/plans/yasc_slices.md).
 *
 * `[]T` is a pointer and a length that go together: `s[i]` is checked
 * against `s.len` (a trap otherwise, at every -O; the optimizer drops the
 * checks it proves), and a slice is only made where its length is known --
 * from an array, by slicing `x[a:b]`, or from a pointer with its end written
 * out (`p[0:n]`).  `[:0]T` is a slice whose element at `len` is a zero (a C
 * string: `[:0]const char s = "abc"` has len 3).
 *
 * The representation is the error union's (yasc_errors.c): a 64-bit
 * unsigned value, ptr in the low word, len in the high one -- r0:r1 as an
 * argument and as a result, so a slice costs a function no memory and no
 * copy, at -O0 too.  type.ref is an interned node (ymod YASC_SLICE_NODE)
 * whose type is T and whose c says the sentinel (1: zero).  Regular C sees
 * an `unsigned long long`.
 *
 * A slice is used by indexing, slicing, `.ptr`, `.len`, assignment and
 * initialization from a slice or an array (to a slice of the same element
 * type, which may add const), passing and returning; arithmetic, comparison
 * and casts are errors.  `.ptr` and `.len` are values, not lvalues: a slice
 * changes only as a whole. */

#include "gen_priv.h"

static Sym *slice_nodes;

ST_FUNC int type_is_slice(CType *type)
{
  return (type->t & VT_BTYPE) == VT_LLONG && type->ref && (type->ref->ymod & YASC_SLICE_NODE);
}

static CType *slice_elem(CType *type)
{
  return &type->ref->type;
}

static int slice_sentinel(CType *type)
{
  return type->ref->c != 0;
}

/* the slice type of ELEM (SENTINEL: [:0]) into *TYPE, storage kept */
static void slice_type(CType *type, const CType *elem, int sentinel)
{
  CType et = *elem;
  Sym *n;
  et.t &= ~VT_STORAGE;
  for (n = slice_nodes; n; n = n->prev)
    if (n->type.t == et.t && n->type.ref == et.ref && n->c == sentinel)
      break;
  if (!n)
  {
    n = sym_push2(&slice_nodes, SYM_FIELD, et.t, sentinel);
    n->type.ref = et.ref;
    n->ymod = YASC_SLICE_NODE;
  }
  type->t = VT_LLONG | VT_UNSIGNED | (type->t & VT_STORAGE);
  type->ref = n;
}

/* `[]` (or `[:0]`) before the declaration's type: wrap what the declarator
 * built before its first function or array derivation, as `!T` (type_decl) */
void yasc_slice_wrap(CType *type, int sentinel)
{
  const int bt = type->t & VT_BTYPE;
  int align;
  char buf[256];
  if (bt == VT_VOID || bt == VT_FUNC || (type->t & (VT_VLA | VT_BITFIELD)) || type_size(type, &align) <= 0)
  {
    type_to_str(buf, sizeof buf, type, NULL);
    tcc_error("[]%s: a slice's elements have a size known here", buf);
  }
  if (sentinel && !(is_integer_btype(bt) && !(type->t & VT_ARRAY)))
  {
    type_to_str(buf, sizeof buf, type, NULL);
    tcc_error("[:0]%s: a zero-terminated slice holds integers (characters)", buf);
  }
  CType elem = *type;
  slice_type(type, &elem, sentinel);
}

/* "[]T" / "[:0]T" for type_to_str */
int yasc_slice_to_str(char *buf, int buf_size, CType *type, const char *varstr)
{
  if (!type_is_slice(type))
    return 0;
  const char *pre = slice_sentinel(type) ? "[:0]" : "[]";
  pstrcpy(buf, buf_size, pre);
  type_to_str(buf + strlen(pre), buf_size - (int)strlen(pre), slice_elem(type), varstr);
  return 1;
}

ST_FUNC void yasc_slice_misuse(void)
{
  tcc_error("a slice is used by s[i], s[a:b], s.ptr and s.len; it is assigned and passed whole");
}

/* ------------------------------------------------------------------------- */
/* the representation */

static void as_u64(void)
{
  vtop->type.t = VT_LLONG | VT_UNSIGNED;
  vtop->type.ref = NULL;
}

/* vtop: a slice -> a value both halves can be read from twice: a variable
 * stays (each read loads the same value), anything else goes to an
 * anonymous local (SSA promotes it), as ubsan_fix does for 64-bit values --
 * copies of one register pair on the value stack must not each extend it. */
static void slice_fix(void)
{
  TCCIRState *ir = tcc_state->ir;
  const CType t = vtop->type;
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) && vtop->sym && !vtop->sym->a.addrtaken)
    return;
  int vr = ir ? tcc_ir_get_vreg_var(ir) : -1;
  loc = tcc_ir_frame_alloc(loc, 8, -8);
  if (vr >= 0)
  {
    tcc_ir_assign_physical_register(ir, vr, loc, -1, -1);
    tcc_ir_set_original_offset(ir, vr, loc);
    tcc_ir_set_llong_type(ir, vr);
  }
  SValue lv;
  svalue_init(&lv);
  lv.type = t;
  lv.r = VT_LOCAL | VT_LVAL;
  lv.vr = vr;
  lv.c.i = loc;
  vpushv(&lv);
  vswap();
  as_u64();
  vtop[-1].type.t = VT_LLONG | VT_UNSIGNED;
  vtop[-1].type.ref = NULL;
  fe_conv_quiet++;
  vstore();
  fe_conv_quiet--;
  vtop->type = t;
}

/* push SV's len (unsigned int) / ptr (a view of T) */
static void push_len(const SValue *sv)
{
  vpushv((SValue *)sv);
  as_u64();
  vpushi(32);
  gen_op(TOK_SHR);
  gen_cast_s(VT_INT | VT_UNSIGNED);
}

static void push_ptr(const SValue *sv, CType *elem)
{
  CType pt = *elem;
  vpushv((SValue *)sv);
  as_u64();
  gen_cast_s(VT_INT | VT_UNSIGNED);
  mk_pointer_yasc(&pt, YASC_VIEW);
  gen_cast(&pt);
}

/* vtop: an array -> its address, a view of ELEM */
static void array_decay(CType *elem)
{
  CType pt = *elem;
  if (vtop->r & VT_LVAL)
    gaddrof();
  mk_pointer_yasc(&pt, YASC_VIEW);
  vtop->type = pt;
}

/* stack: ptr (a pointer), len (unsigned) -> a slice of type T */
static void make_slice(CType *t)
{
  gen_cast_s(VT_LLONG | VT_UNSIGNED);
  vpushi(32);
  gen_op(TOK_SHL);
  vswap();
  gen_cast_s(VT_INT | VT_UNSIGNED);
  gen_cast_s(VT_LLONG | VT_UNSIGNED);
  gen_op('|');
  vtop->type = *t;
}

/* an integer index: vtop -> unsigned int, in a register */
static void index_operand(const char *what)
{
  if (!is_integer_btype(vtop->type.t & VT_BTYPE) || (vtop->type.t & VT_BTYPE) == VT_PTR)
    tcc_error("%s is an integer", what);
  gen_cast_s(VT_INT | VT_UNSIGNED);
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    gv(RC_INT);
}

/* -------------------------------------------------------------------------
 * conversions: assignment, initialization, arguments, results */

/* Can elements of type S be seen as elements of type D: the same type, D
 * maybe adding const or volatile */
static int elem_converts(CType *d, CType *s)
{
  if (!compare_types(d, s, 1))
    return 0;
  return ((s->t & ~d->t) & (VT_CONSTANT | VT_VOLATILE)) == 0;
}

/* vtop converted to DT: a slice to a slice, an array to a slice; a slice to
 * anything else, or anything else to a slice, is an error */
void yasc_slice_assign(CType *dt)
{
  CType *st = &vtop->type;
  char b1[256], b2[256];
  const int ds = type_is_slice(dt), ss = type_is_slice(st);
  if (!ds && !ss)
    return;
  type_to_str(b1, sizeof b1, st, NULL);
  type_to_str(b2, sizeof b2, dt, NULL);
  if (!ds)
  {
    if ((dt->t & VT_BTYPE) == VT_VOID)
      return;
    tcc_error("a slice %s is not a %s: use .ptr and .len", b1, b2);
  }
  if (ss)
  {
    if (st->ref == dt->ref)
      return;
    if (!elem_converts(slice_elem(dt), slice_elem(st)) || (slice_sentinel(dt) && !slice_sentinel(st)))
      tcc_error("%s does not convert to %s", b1, b2);
    vtop->type.ref = dt->ref;
    return;
  }
  if ((st->t & VT_ARRAY) && !(st->t & VT_VLA) && st->ref && st->ref->c >= 0 && elem_converts(slice_elem(dt), &st->ref->type))
  {
    /* an array holding a string exactly (a literal: its last element its
       only zero) gives the string; any other array all its elements */
    int n = st->ref->c, slen;
    const char *s = try_get_constant_string(vtop, &slen);
    int string = s && slen + 1 == n;
    if (slice_sentinel(dt) && !string)
      tcc_error("%s to %s: only a string literal is known to end in a zero", b1, b2);
    if (string)
      n = slen;
    array_decay(&st->ref->type);
    vpushi(n);
    vtop->type.t = VT_INT | VT_UNSIGNED;
    make_slice(dt);
    return;
  }
  if (st->t & VT_VLA)
    tcc_error("%s to %s: slice the VLA with its length, v[0:n]", b1, b2);
  tcc_error("%s does not convert to %s: slice it, p[0:n], or take an array", b1, b2);
}

/* -------------------------------------------------------------------------
 * expressions */

/* `s.ptr`, `s.len`: tok is the name after '.' */
void yasc_slice_member(void)
{
  CType t = vtop->type;
  const char *name = get_tok_str(tok, NULL);
  int len;
  if (!strcmp(name, "len"))
    len = 1;
  else if (!strcmp(name, "ptr"))
    len = 0;
  else
    tcc_error("a slice has .ptr and .len, not .%s", name);
  next();
  slice_fix();
  SValue sv = *vtop;
  if (len)
    push_len(&sv);
  else
    push_ptr(&sv, slice_elem(&t));
  vswap();
  vpop();
}

/* ---- the sentinel of a [:0] slice ---------------------------------------
 * `s[s.len]` of a `[:0]T` is its zero: it may be read, and written with a
 * zero (a no-op), never with anything else -- so no use of a `[:0]` slice
 * takes its terminator away.  A write below len may store a zero (the C
 * string gets shorter; the slice keeps its len and its sentinel).  The index
 * is checked `i <=u len` (slice_index); the element it gives is noted here,
 * and a store into it (vstore: `=`, `op=`, `++`, `--`) adds the check
 * `i == len && (T)v != 0` -- nothing when v is a zero constant. */
int yasc_sentinel_live; /* the note is set: vstore looks */
static struct
{
  SValue elem;      /* the element lvalue slice_index gave */
  SValue base, ix;  /* the slice (slice_fix'ed: read again) and the index */
} sentinel_note;

void yasc_sentinel_reset(void)
{
  yasc_sentinel_live = 0;
}

static int same_lvalue(const SValue *a, const SValue *b)
{
  return a->r == b->r && a->vr == b->vr && a->c.i == b->c.i && a->sym == b->sym &&
         (a->type.t & VT_BTYPE) == (b->type.t & VT_BTYPE);
}

/* vstore's hook: stack lvalue, value.  The lvalue the element of a [:0]
   slice: storing a non-zero at its len is a trap (a certain one an error) */
#define SV_IS_CONST(sv) (((sv)->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)

/* vtop: a value -> (ET)value != 0, popped value not: a copy is converted */
static void push_nonzero(const CType *et)
{
  vpushv(vtop);
  fe_conv_quiet++;
  gen_cast((CType *)et);
  fe_conv_quiet--;
  vpushi(0);
  gen_op(TOK_NE);
}

void yasc_sentinel_store(void)
{
  TCCIRState *ir = tcc_state->ir;
  if (!same_lvalue(&vtop[-1], &sentinel_note.elem) || nocode_wanted || !ir)
    return;
  CType et = vtop[-1].type;
  et.t &= ~(VT_CONSTANT | VT_VOLATILE | VT_STORAGE);
  const int vconst = SV_IS_CONST(vtop);
  if (vconst)
  {
    push_nonzero(&et);
    const int nz = vtop->c.i != 0;
    vpop();
    if (!nz)
      return; /* a zero: allowed anywhere up to len */
  }
  else
    gv(RC_TYPE(vtop->type.t)); /* read once: a condition, a call */
  vpushv(&sentinel_note.ix);
  push_len(&sentinel_note.base);
  gen_op(TOK_EQ);
  if (SV_IS_CONST(vtop))
  {
    const int at_len = vtop->c.i != 0;
    vpop();
    if (!at_len)
      return;
    if (vconst)
      tcc_error("a non-zero stored over a [:0] slice's terminator (s[s.len])");
    push_nonzero(&et);
    fe_trap_if();
    return;
  }
  if (vconst)
  {
    fe_trap_if(); /* a non-zero constant: the index below len */
    return;
  }
  /* at len, the value must be a zero */
  const int saved_nocode = nocode_wanted;
  int skip = tcc_ir_codegen_test_gen(ir, 1, -1);
  push_nonzero(&et);
  fe_trap_if();
  tcc_ir_backpatch_to_here(ir, skip);
  nocode_wanted = saved_nocode;
}

/* stack: slice, index -> the element, an lvalue; the index is checked
   against len (a trap) -- up to len itself for a [:0] slice's sentinel */
static void slice_index(void)
{
  CType st = vtop[-1].type;
  const int sentinel = slice_sentinel(&st);
  index_operand("a slice's index");
  vswap();
  slice_fix();
  vswap();
  SValue base = vtop[-1], ix = *vtop;
  vpushv(&ix);
  push_len(&base);
  gen_op(sentinel ? TOK_UGT : TOK_UGE);
  fe_trap_if();
  push_ptr(&base, slice_elem(&st));
  vpushv(&ix);
  gen_op('+');
  vswap();
  vpop();
  vswap();
  vpop();
  indir();
  if (sentinel && !(slice_elem(&st)->t & VT_CONSTANT))
  {
    sentinel_note.elem = *vtop;
    sentinel_note.base = base;
    sentinel_note.ix = ix;
    yasc_sentinel_live = 1;
  }
}

/* stack: base, a -> base[a:b] (HAS_B: b follows, else up to the end); tok
 * is past the ':'.  `base[a:b:0]` is a [:0] slice: base[b] is read and must
 * be a zero (a trap otherwise), so it is inside the base -- b < len, or
 * b <= len for a [:0] base, whose sentinel it may be. */
static void slice_of(int has_b)
{
  CType bt = vtop[-1].type, elem, result;
  int sentinel = 0, have_len = 1, align, terminated = 0, base_sentinel = 0;
  index_operand("a slice's start");
  if (type_is_slice(&bt))
  {
    elem = *slice_elem(&bt);
    vswap();
    slice_fix();
    vswap();
    base_sentinel = slice_sentinel(&bt);
    sentinel = !has_b && base_sentinel; /* s[a:] still ends where s does */
  }
  else if ((bt.t & VT_VLA) || ((bt.t & VT_ARRAY) && (!bt.ref || bt.ref->c < 0)))
  {
    /* a VLA, an array of unknown length: sliced as a pointer, its end said */
    if (!has_b)
      tcc_error("slicing an array whose length is not known here: say its end, a[x:n]");
    elem = bt.ref->type;
    have_len = 0;
    bt.t &= ~VT_ARRAY;
  }
  else if (bt.t & VT_ARRAY)
    elem = bt.ref->type;
  else if ((bt.t & VT_BTYPE) == VT_PTR)
  {
    if (!has_b)
      tcc_error("a pointer has no length: say the slice's end, p[a:b]");
    elem = *pointed_type(&bt);
    have_len = 0;
  }
  else
    tcc_error("only an array, a slice or a pointer is sliced");
  if ((elem.t & VT_BTYPE) == VT_VOID || (elem.t & VT_BTYPE) == VT_FUNC || type_size(&elem, &align) <= 0)
    tcc_error("slicing elements of no known size");
  /* the base's length; b, written or that length */
  SValue base = vtop[-1], a = *vtop, len;
  if (type_is_slice(&bt))
    push_len(&base);
  else if (have_len)
  {
    vpushi(bt.ref->c);
    vtop->type.t = VT_INT | VT_UNSIGNED;
  }
  if (have_len)
  {
    len = *vtop;
    vpop();
  }
  if (has_b)
  {
    gexpr();
    index_operand("a slice's end");
    if (tok == ':')
    {
      next();
      if (tok != TOK_CINT || tokc.i != 0)
        expect("0: x[a:b:0] is a slice ending in a zero");
      next();
      if (!is_integer_btype(elem.t & VT_BTYPE) || (elem.t & VT_ARRAY))
        tcc_error("x[a:b:0]: a zero-terminated slice holds integers (characters)");
      terminated = sentinel = 1;
    }
  }
  else
    vpushv(&len);
  SValue b = *vtop;
  /* stack: base, a, b.  b <= len (b < len when base[b] is read and is not
     the base's own sentinel), a <= b: a trap otherwise */
  if (have_len && has_b)
  {
    vpushv(&b);
    vpushv(&len);
    gen_op(terminated && !base_sentinel ? TOK_UGE : TOK_UGT);
    fe_trap_if();
  }
  vpushv(&a);
  vpushv(&b);
  gen_op(TOK_UGT);
  fe_trap_if();
  /* the base's first element, a view of elem */
  if (type_is_slice(&bt))
    push_ptr(&base, &elem);
  else
  {
    CType pt = elem;
    vpushv(&base);
    if (bt.t & VT_ARRAY)
      array_decay(&elem);
    else
    {
      mk_pointer_yasc(&pt, YASC_VIEW);
      gen_cast(&pt);
    }
  }
  if (terminated)
  {
    /* base[b] == 0: a trap otherwise */
    gv(RC_INT);
    SValue p = *vtop;
    vpushv(&p);
    vpushv(&b);
    gen_op('+');
    indir();
    vpushi(0);
    gen_op(TOK_NE);
    fe_trap_if();
  }
  /* ptr + a, b - a */
  vpushv(&a);
  gen_op('+');
  vpushv(&b);
  vpushv(&a);
  gen_op('-');
  gen_cast_s(VT_INT | VT_UNSIGNED);
  result.t = 0;
  slice_type(&result, &elem, sentinel);
  make_slice(&result);
  /* the result above base, a, b: they go */
  vrott(4);
  vpop();
  vpop();
  vpop();
}

/* yasc: after `x[` -- a slice index, or slicing (`x[a:b]`, `x[a:]`,
 * `x[:b]`, `x[:]`).  1 when handled (']' read), 0 when the subscript is a
 * plain one the caller goes on with: the index then on the stack above x. */
int yasc_subscript(void)
{
  if (tok == ':')
  {
    next();
    vpushi(0);
    vtop->type.t = VT_INT | VT_UNSIGNED;
    slice_of(tok != ']');
    skip(']');
    return 1;
  }
  gexpr();
  if (tok == ':')
  {
    next();
    slice_of(tok != ']');
    skip(']');
    return 1;
  }
  if (type_is_slice(&vtop[-1].type))
  {
    slice_index();
    skip(']');
    return 1;
  }
  if (type_is_slice(&vtop->type))
    tcc_error("i[s]: a slice is indexed s[i]");
  return 0;
}
