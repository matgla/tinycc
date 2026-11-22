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

/* objsize.c -- object sizes, and the checks built on them.
 *
 * __builtin_object_size(p, type) and __builtin_dynamic_object_size(p, type)
 * (gcc's semantics: type 0/2 the whole object, 1/3 the closest member
 * subobject; 0/1 an upper bound, (size_t)-1 when unknown, 2/3 a lower bound,
 * 0 when unknown).  Everything is decided in the frontend, as the argument is
 * parsed, so the answer is the same at every -O level (gcc's improves with
 * optimisation; ours does not change):
 *
 * - a declared object: a local or global variable, a string literal, a
 *   compound literal -- the bytes from the pointer to its end;
 * - a member: `s.m`, `p->m`, `&s.m[2]`, followed through `.`, `->`, `[]`,
 *   `&`, `*`, casts and pointer +/- an integer (ObjRec below).  A trailing
 *   array member reached through a pointer, or of one element or less, is
 *   flexible: no subobject size (as gcc);
 * - an allocation: the result of a call to an alloc_size function (and of
 *   malloc, calloc, realloc, aligned_alloc, memalign, alloca by name, unless
 *   -fno-builtin) with constant sizes, directly or through a local pointer
 *   variable that every store so far gave such a result (or null) -- see the
 *   tracker below for when a variable is followed;
 * - __builtin_dynamic_object_size also answers at run time: a VLA, and
 *   malloc(n) / calloc(n, k) through a variable whose n is a local variable
 *   not stored to since.
 *
 * Not followed: a parameter (also in an inline expansion: the caller's
 * argument is not looked at, so -O2 says what -O0 does), a pointer loaded
 * from memory, a variable whose address is taken.
 *
 * The checks at calls (objsize_call_*):
 * - -Wstringop-overflow: memcpy, memmove, mempcpy, memset, bzero (whole
 *   object), strcpy, stpcpy, strcat, strncpy, stpncpy, strncat, snprintf's
 *   bound (closest subobject; level 1 uses the whole object) with a constant
 *   length, or a constant source string, larger than the destination;
 * - -Wformat-overflow: sprintf/vsprintf whose literal format's shortest
 *   output (constants formatted, unknown values at their shortest) does not
 *   fit; -Wformat-truncation: the same for snprintf/vsnprintf against the
 *   bound -- level 1 (-Wall) when the call's result is not used, level 2 always;
 * - -Walloc-size-larger-than=N: an allocation's constant size (product)
 *   over N, PTRDIFF_MAX by default; -Walloc-mul-overflow (not in -Wall): the
 *   size argument of a one-size allocator is a multiplication that may
 *   overflow (an operand not a constant, and no known bound keeps it in range);
 * - -Wfree-nonheap-object: free, realloc or a malloc(dealloc) deallocator
 *   given a declared object, or a tracked allocation plus a nonzero offset;
 * - -Wmismatched-dealloc: a tracked allocation given to a deallocator that is
 *   not its allocator's (free family vs malloc(fclose, 1), ...).
 */

#include "gen_priv.h"

int objsize_ctx;
ObjRec objsize_rec;

/* the result of the last allocation call, until a store takes it */
static struct
{
  SValue sv;
  ObjHeap h;
  unsigned char on;
} last_alloc;

/* the last multiplication parsed in a context (-Walloc-mul-overflow) */
static struct
{
  SValue sv;
  unsigned char on, risky;
} last_mul;

static int is_digit_c(int c)
{
  return c >= '0' && c <= '9';
}

static int sv_same(const SValue *a, const SValue *b)
{
  return a->r == b->r && a->vr == b->vr && a->c.i == b->c.i && a->sym == b->sym;
}

int objsize_rec_is(const SValue *sv)
{
  return objsize_rec.on && sv_same(sv, &objsize_rec.snap);
}

static void rec_snap(void)
{
  objsize_rec.snap = *vtop;
}

void objsize_enter(ObjRec *saved)
{
  *saved = objsize_rec;
  objsize_rec.on = 0;
  objsize_ctx++;
}

void objsize_leave(const ObjRec *saved)
{
  objsize_ctx--;
  objsize_rec = *saved;
}

/* ------------------------------------------------------------------------- */
/* The allocation tracker.
 *
 * A local pointer variable (not a parameter, not static) is followed while
 * every store to it so far is an allocation's result or null: the sizes
 * merge (the smallest and the largest), and so do the deallocators.  A store
 * of anything else, its address taken, or a goto label after it marks it
 * (SymAttr.possibly_written, which only globals use otherwise) for good.  The
 * merge covers the forward paths; a later store can only reach an earlier use
 * through a back edge, so a use in a loop (or a while/for header) that began
 * after the variable's declaration knows nothing -- unless the variable is
 * const, which only its initializer stores to. */

/* the loop header (while/for condition and step) being parsed */
int objsize_loop_hdr;
/* variables an allocation's size came from (os_watch) in the unit */
static int objsize_nb_watch;
/* ObjHeap.dealloc for the free family known by name */
#define DEALLOC_FREE (-2)

static int os_local_ptr(const Sym *s)
{
  return s && (s->r & VT_VALMASK) == VT_LOCAL && !(s->v & (SYM_FIELD | SYM_STRUCT)) &&
         (s->v & ~SYM_FIELD) < SYM_FIRST_ANOM && (s->type.t & (VT_BTYPE | VT_ARRAY | VT_VLA)) == VT_PTR;
}

static int os_usable(Sym *s, const SymLocalFacts *f)
{
  if (!f || f->os_state != 1 || s->a.possibly_written || s->a.addrtaken)
    return 0;
  if (s->type.t & VT_CONSTANT)
    return 1;
  if (objsize_loop_hdr)
    return 0;
  return !loop_scope || block_declared_in_loop(s);
}

static void heap_merge(ObjHeap *a, const ObjHeap *b)
{
  if (a->known && b->known)
  {
    if (b->lo < a->lo)
      a->lo = b->lo;
    if (b->hi > a->hi)
      a->hi = b->hi;
  }
  else
    a->known = 0;
  if (!a->dealloc)
    a->dealloc = b->dealloc, a->alloc_tok = b->alloc_tok, a->alloc_line = b->alloc_line;
  else if (b->dealloc && b->dealloc != a->dealloc)
    a->dealloc = -1;
  else if (b->dealloc)
    a->alloc_tok = b->alloc_tok, a->alloc_line = b->alloc_line;
  a->nsym = NULL; /* the dynamic size follows one allocation only */
  a->decl |= b->decl;
}

/* the result of the last conditional whose arms are both known (until a
   store takes it) */
static struct
{
  SValue sv;
  ObjHeap h;
  unsigned char on;
} last_cond;

/* What the pointer value SV points into, as the tracker follows it: an
   allocation's result, a conditional of known arms, or the address of a
   declared object (its bytes from there, decl).  0 when not known. */
int objsize_value_heap(SValue *sv, ObjHeap *h)
{
  ObjInfo oi;
  if (NOEVAL_WANTED || (sv->type.t & VT_BTYPE) != VT_PTR)
    return 0;
  if (last_alloc.on && sv_same(sv, &last_alloc.sv))
  {
    *h = last_alloc.h;
    return 1;
  }
  if (last_cond.on && sv_same(sv, &last_cond.sv))
  {
    *h = last_cond.h;
    return 1;
  }
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && sv->c.i == 0)
  {
    memset(h, 0, sizeof *h); /* null: no size, any deallocator */
    return 1;
  }
  objsize_info(sv, &oi);
  if (oi.kind == OI_HEAP)
  {
    /* a followed allocation, maybe plus an offset */
    *h = oi.h;
    h->known = oi.known0;
    h->lo = (unsigned)oi.min0;
    h->hi = (unsigned)oi.max0;
    if (oi.off || oi.off_unknown)
      h->dealloc = -1, h->decl = 1, h->nsym = NULL; /* not its start */
    return 1;
  }
  if (oi.kind != OI_DECL || !oi.known0)
    return 0;
  memset(h, 0, sizeof *h);
  h->known = 1;
  h->lo = (unsigned)oi.min0;
  h->hi = (unsigned)oi.max0;
  h->dealloc = -1;
  h->decl = 1;
  return 1;
}

/* The right-hand side of a store to a local pointer variable is parsed in a
   context, so what it points into is known at the store */
void objsize_rhs_begin(ObjRec *saved)
{
  objsize_enter(saved);
}

void objsize_rhs_end(const ObjRec *saved)
{
  ObjHeap h;
  if (objsize_value_heap(vtop, &h))
  {
    last_cond.sv = *vtop;
    last_cond.h = h;
    last_cond.on = 1;
  }
  objsize_leave(saved);
}

static void rec_from_alloc(const ObjHeap *h);

/* alloca / __builtin_alloca returned vtop, of size SIZE */
void objsize_alloca_result(SValue *size)
{
  ObjHeap h;
  int big;
  memset(&h, 0, sizeof h);
  if (fe_int_const(size))
  {
    int64_t n = fe_const_value(size, &big);
    if (!big && n >= 0 && n <= 0xffffffffll)
      h.known = 1, h.lo = h.hi = (unsigned)n;
  }
  h.dealloc = -1;
  h.decl = 1; /* not free's */
  if (objsize_ctx)
    rec_from_alloc(&h);
  if (!NOEVAL_WANTED)
  {
    last_alloc.sv = *vtop;
    last_alloc.h = h;
    last_alloc.on = 1;
  }
}

/* the conditional just parsed (vtop) has arms T and F */
void objsize_cond_result(const ObjHeap *t, const ObjHeap *f)
{
  if (NOEVAL_WANTED)
    return;
  last_cond.h = *t;
  heap_merge(&last_cond.h, f);
  last_cond.sv = *vtop;
  last_cond.on = 1;
}

/* vstore: DST (a local's lvalue) was given SRC */
void objsize_store(SValue *dst, SValue *src)
{
  Sym *s = dst->sym;
  SymLocalFacts *f;
  ObjHeap h;
  /* an initializer's store names the variable by its vreg only: it was
     just declared, near the top of local_stack */
  if (!s && dst->vr >= 0 && (dst->r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) &&
      (dst->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && !NOEVAL_WANTED)
    for (Sym *p = local_stack; p; p = p->prev)
      if (p->vreg == dst->vr && (p->r & VT_VALMASK) == VT_LOCAL)
      {
        s = p;
        break;
      }

  if (NOEVAL_WANTED || !s || !(dst->r & VT_LVAL) || !os_local_ptr(s) || dst->type.ref != s->type.ref ||
      (dst->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR)
  {
    /* a store to a variable an allocation's size came from */
    if (objsize_nb_watch && s && !NOEVAL_WANTED)
    {
      f = sym_facts_peek(s);
      if (f && f->os_watch)
        f->os_stores++;
    }
    return;
  }
  if (s->a.possibly_written)
    return;
  /* an allocation (an inline expansion's result is the lvalue of its return
     variable), a conditional of known arms, null, a declared object */
  if (!objsize_value_heap(src, &h))
  {
    s->a.possibly_written = 1;
    return;
  }
  if (last_alloc.on && sv_same(src, &last_alloc.sv))
    last_alloc.on = 0;
  if (last_cond.on && sv_same(src, &last_cond.sv))
    last_cond.on = 0;
  f = sym_facts(s);
  if (!f->os_state)
  {
    f->os_state = 1;
    f->os_heap = h;
  }
  else
    heap_merge(&f->os_heap, &h);
}

/* `&v` on a local: a pointer variable is no longer followed */
void objsize_addr_taken(Sym *s)
{
  if (os_local_ptr(s) && !NOEVAL_WANTED)
    s->a.possibly_written = 1;
}

/* a goto label: what a jump back to it carries is not known */
void objsize_label(void)
{
  Sym *s;
  if (NOEVAL_WANTED)
    return;
  for (s = local_stack; s; s = s->prev)
    if (os_local_ptr(s) && !(s->type.t & VT_CONSTANT))
      s->a.possibly_written = 1;
}

/* ------------------------------------------------------------------------- */
/* What the value being parsed points into (ObjRec) */

/* An identifier S was pushed */
void objsize_ident(Sym *s)
{
  const SymLocalFacts *f;
  if ((s->type.t & VT_VLA) && (s->r & VT_VALMASK) == VT_LOCAL)
  {
    memset(&objsize_rec, 0, sizeof objsize_rec);
    objsize_rec.on = 1;
    objsize_rec.kind = OR_VLA;
    objsize_rec.base = s;
    objsize_rec.form = OR_VAL;
    rec_snap();
    return;
  }
  if (!os_local_ptr(s) || !os_usable(s, f = sym_facts_peek(s)))
  {
    /* a declared object: a local, a parameter, a global or static (not a
       function); a pointer parameter's value is not looked at (form ADDR) */
    if ((s->type.t & VT_BTYPE) == VT_FUNC || (s->v & (SYM_FIELD | SYM_STRUCT)) ||
        ((s->r & VT_VALMASK) != VT_LOCAL && (s->r & (VT_VALMASK | VT_SYM)) != (VT_CONST | VT_SYM) &&
         !((s->r & VT_VALMASK) < VT_CONST && (s->r & VT_PARAM))))
      return;
    memset(&objsize_rec, 0, sizeof objsize_rec);
    objsize_rec.on = 1;
    objsize_rec.kind = OR_DECL;
    objsize_rec.base = s;
    objsize_rec.form = (s->type.t & VT_ARRAY) ? OR_VAL : OR_ADDR;
    rec_snap();
    return;
  }
  memset(&objsize_rec, 0, sizeof objsize_rec);
  objsize_rec.on = 1;
  objsize_rec.kind = OR_HEAP;
  objsize_rec.h = f->os_heap;
  objsize_rec.base = s;
  objsize_rec.form = OR_VAL;
  rec_snap();
}

/* A parameter of an inline evaluation was pushed as the caller's value:
   nothing is known of it (what -O0's call would say) */
void objsize_param(void)
{
  memset(&objsize_rec, 0, sizeof objsize_rec);
  objsize_rec.on = 1;
  objsize_rec.param = 1;
  objsize_rec.form = OR_VAL;
  rec_snap();
}

/* `.` / `->` is about to be applied to vtop */
int objsize_member_begin(ObjRec *saved)
{
  int had = objsize_rec_is(vtop);
  *saved = objsize_rec;
  objsize_rec.on = 0;
  return had;
}

/* ... and named FIELD at CUMOFS: vtop is the member now */
void objsize_member_end(const ObjRec *saved, int had, int arrow, Sym *field, int cumofs, int flex)
{
  ObjRec r;
  int align, sz;
  memset(&r, 0, sizeof r);
  if (had && (saved->param || saved->form == (arrow ? OR_VAL : OR_ADDR)))
  {
    r = *saved;
    r.off += cumofs;
  }
  r.on = 1;
  r.sub = 1;
  sz = (field->type.t & VT_VLA) ? -1 : type_size(&field->type, &align);
  r.sub_flex = flex || sz < 0;
  r.sub_size = sz < 0 ? 0 : sz;
  r.sub_off = 0;
  r.form = (field->type.t & (VT_ARRAY | VT_VLA)) ? OR_VAL : OR_ADDR;
  objsize_rec = r;
  rec_snap();
}

/* indir() made vtop's lvalue of what it pointed to */
void objsize_after_indir(void)
{
  if (objsize_rec.form != OR_VAL)
  {
    objsize_rec.on = 0; /* a pointer loaded from inside the object */
    return;
  }
  objsize_rec.form = OR_ADDR;
  rec_snap();
}

/* `&` took vtop's address; WAS_ARRAY: of an array (or a function) */
void objsize_after_addr(int was_array)
{
  if (objsize_rec.form != OR_ADDR && !was_array)
  {
    objsize_rec.on = 0; /* &p of a followed pointer variable: not its object */
    return;
  }
  objsize_rec.form = OR_VAL;
  rec_snap();
}

/* An allocation's result is vtop (in a context) */
static void rec_from_alloc(const ObjHeap *h)
{
  memset(&objsize_rec, 0, sizeof objsize_rec);
  objsize_rec.on = 1;
  objsize_rec.kind = OR_HEAP;
  objsize_rec.h = *h;
  objsize_rec.form = OR_VAL;
  rec_snap();
}

/* the largest value an integer operand may have: 0 when not known */
static int int_bound(SValue *sv, unsigned long long *out)
{
  int t = sv->type.t, bits;
  if (!is_integer_btype(t & VT_BTYPE))
    return 0;
  if (svalue_get_conservative_max_u64(sv, out))
    return 1;
  bits = (t & VT_BITFIELD) ? BIT_SIZE(t) : (t & VT_BTYPE) == VT_BOOL ? 1 : btype_size(t & VT_BTYPE) * 8;
  if (bits >= 32)
    return 0;
  *out = (1ull << bits) - 1;
  return 1;
}

int objsize_op_begin(ObjOpHook *h, int op)
{
  SValue *p = vtop - 1, *i = vtop;
  int align, esz;

  memset(h, 0, sizeof *h);
  if (op == '*')
  {
    unsigned long long a, b;
    h->mul = 1;
    if ((fe_int_const(vtop) && fe_int_const(vtop - 1)) ||
        (int_bound(vtop - 1, &a) && int_bound(vtop, &b) && (!a || !b || a <= 0xffffffffull / b)))
      h->risky = 0;
    else
      h->risky = 1;
    return 1;
  }
  if (!objsize_rec.on)
    return 0;
  if (op != '+' && op != '-')
  {
    if (objsize_rec_is(vtop) || objsize_rec_is(vtop - 1))
      objsize_rec.on = 0;
    return 0;
  }
  if (!objsize_rec_is(p))
  {
    if (op != '+' || !objsize_rec_is(i))
      return 0;
    p = vtop, i = vtop - 1;
  }
  if ((p->type.t & VT_BTYPE) != VT_PTR || !is_integer_btype(i->type.t & VT_BTYPE) ||
      (objsize_rec.form != OR_VAL && !(p->type.t & (VT_ARRAY | VT_VLA))))
  {
    objsize_rec.on = 0;
    return 0;
  }
  {
    CType *pt = pointed_type(&p->type);
    esz = (pt->t & VT_VLA) ? -1 : (pt->t & VT_BTYPE) == VT_VOID ? 1 : type_size(pt, &align);
  }
  if (esz > 0 && fe_int_const(i))
  {
    int big;
    int64_t k = fe_const_value(i, &big);
    h->delta = big ? 0 : k * esz;
    h->unknown = big;
    if (op == '-')
      h->delta = -h->delta;
  }
  else
    h->unknown = 1;
  h->active = 1;
  return 1;
}

void objsize_op_end(ObjOpHook *h)
{
  if (h->mul)
  {
    last_mul.sv = *vtop;
    last_mul.on = 1;
    last_mul.risky = h->risky;
    return;
  }
  if (!h->active)
    return;
  if (h->unknown)
    objsize_rec.off_unknown = 1;
  objsize_rec.off += h->delta;
  objsize_rec.sub_off += h->delta;
  objsize_rec.form = OR_VAL;
  rec_snap();
}

/* ------------------------------------------------------------------------- */
/* What a pointer value points into */

/* Whether an object of type T holds a pointer at byte offset OFF. */
static int ptr_slot_at(CType *t, int off)
{
  if (t->t & VT_ARRAY)
  {
    int align, esz = type_size(&t->ref->type, &align);
    return esz > 0 && ptr_slot_at(&t->ref->type, off % esz);
  }
  if ((t->t & VT_BTYPE) == VT_PTR)
    return off == 0;
  if ((t->t & VT_BTYPE) == VT_STRUCT)
  {
    Sym *f;
    for (f = sym_first(t->ref); f; f = f->next)
    {
      int align, fsz;
      if (f->type.t & VT_BITFIELD)
        continue;
      fsz = type_size(&f->type, &align);
      if (off >= (int)f->c && off < (int)f->c + fsz && ptr_slot_at(&f->type, off - (int)f->c))
        return 1;
    }
  }
  return 0;
}

/* the local variable (array, struct, VLA or address-taken scalar) holding
   frame offset OFF */
static Sym *local_at(int off)
{
  Sym *s;
  for (s = local_stack; s; s = s->prev)
  {
    int align, sz;
    if ((s->r & VT_VALMASK) != VT_LOCAL || (s->v & (SYM_FIELD | SYM_STRUCT)))
      continue;
    /* a scalar the register allocator places has no frame offset */
    if ((s->r & VT_LVAL) && (s->type.t & VT_BTYPE) != VT_STRUCT && !(s->type.t & (VT_ARRAY | VT_VLA)))
      continue;
    sz = type_size(&s->type, &align);
    if (sz <= 0)
      continue;
    if (off >= (int)s->c && off < (int)s->c + sz)
      return s;
  }
  return NULL;
}

static unsigned long long rem(long long size, long long off)
{
  return off < 0 || off > size ? 0 : (unsigned long long)(size - off);
}

void objsize_info(SValue *sv, ObjInfo *oi)
{
  int rec = objsize_rec_is(sv) && (objsize_rec.form == OR_VAL || (sv->type.t & (VT_ARRAY | VT_VLA)));

  memset(oi, 0, sizeof *oi);
  if (rec && objsize_rec.param)
    return;
  if (rec && objsize_rec.kind == OR_HEAP)
  {
    const ObjHeap *h = &objsize_rec.h;
    oi->kind = OI_HEAP;
    oi->h = *h;
    oi->off = objsize_rec.off;
    oi->off_unknown = objsize_rec.off_unknown;
    if (h->known)
    {
      oi->known0 = 1;
      oi->max0 = oi->off_unknown ? h->hi : rem(h->hi, oi->off);
      oi->min0 = oi->off_unknown ? 0 : rem(h->lo, oi->off);
    }
  }
  else if (rec && objsize_rec.kind == OR_DECL)
  {
    Sym *b = objsize_rec.base;
    int align;
    long long size = type_size(&b->type, &align);
    /* the address of a global, plus a constant: its value says the offset
       (a constant added to it folds into the symbol's offset, where the
       record may not see it) */
    if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM) && sv->sym == b && !objsize_rec.off_unknown &&
        (long long)sv->c.i != objsize_rec.off)
    {
      long long missed = (long long)sv->c.i - objsize_rec.off;
      objsize_rec.off += missed;
      objsize_rec.sub_off += missed;
    }
    if (size <= 0 && (b->r & VT_SYM))
    {
      ElfSym *esym = elfsym(b);
      size = esym ? (long long)esym->st_size : 0;
    }
    oi->kind = OI_DECL;
    oi->obj = b;
    oi->off = objsize_rec.off;
    oi->off_unknown = objsize_rec.off_unknown;
    if (size > 0)
    {
      oi->known0 = 1;
      oi->max0 = oi->off_unknown ? (unsigned long long)size : rem(size, oi->off);
      oi->min0 = oi->off_unknown ? 0 : oi->max0;
    }
  }
  else if (rec && objsize_rec.kind == OR_VLA)
  {
    oi->kind = OI_DECL;
    oi->obj = objsize_rec.base;
    oi->vla = objsize_rec.base;
    oi->off = objsize_rec.off;
    oi->off_unknown = objsize_rec.off_unknown;
  }
  else if ((sv->r & VT_VALMASK) == VT_LOCAL && !(sv->r & VT_SYM))
  {
    int off = (int)sv->c.i, align, lval_ptr;
    Sym *s = local_at(off);
    /* a pointer variable's value (plus a constant, without code): not an
       address in the frame */
    if (sv->sym && (sv->sym->r & VT_VALMASK) == VT_LOCAL && (sv->sym->type.t & VT_BTYPE) == VT_PTR &&
        !(sv->sym->type.t & (VT_ARRAY | VT_VLA)) && !sv->sym->a.addrtaken)
      s = NULL;
    lval_ptr = (sv->r & VT_LVAL) && !(sv->type.t & (VT_ARRAY | VT_VLA));
    if (!s && !lval_ptr && sv->sym && (sv->sym->r & VT_VALMASK) == VT_LOCAL && sv->sym->a.addrtaken &&
        !(sv->sym->v & (SYM_FIELD | SYM_STRUCT)))
    {
      s = sv->sym; /* &x of an address-taken scalar */
      off = (int)s->c;
    }
    /* a pointer loaded from the frame: what it points to is elsewhere */
    if (s && lval_ptr && ((sv->type.t & VT_BTYPE) != VT_STRUCT) && ptr_slot_at(&s->type, off - (int)s->c))
      s = NULL;
    if (s && lval_ptr && (sv->type.t & VT_BTYPE) == VT_PTR)
      s = NULL;
    if (s && !(s->type.t & VT_VLA))
    {
      long long size = type_size(&s->type, &align);
      oi->kind = OI_DECL;
      oi->obj = s;
      oi->off = off - (int)s->c;
      oi->known0 = 1;
      oi->max0 = oi->min0 = rem(size, oi->off);
    }
  }
  else if ((sv->r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) && sv->sym &&
           (!(sv->r & VT_LVAL) || (sv->type.t & VT_ARRAY)) && (sv->type.t & VT_BTYPE) != VT_FUNC)
  {
    ElfSym *esym = elfsym(sv->sym);
    if (esym && esym->st_size > 0 && ELFW(ST_TYPE)(esym->st_info) != STT_FUNC)
    {
      oi->kind = OI_DECL;
      oi->obj = sv->sym;
      oi->off = (long long)sv->c.i;
      oi->known0 = 1;
      oi->max0 = oi->min0 = rem(esym->st_size, oi->off);
    }
  }
  /* the closest subobject */
  if (rec && objsize_rec.sub && !objsize_rec.sub_flex && !objsize_rec.param)
  {
    oi->known1 = 1;
    oi->max1 = objsize_rec.off_unknown ? objsize_rec.sub_size : rem(objsize_rec.sub_size, objsize_rec.sub_off);
    oi->min1 = objsize_rec.off_unknown ? 0 : oi->max1;
  }
  else if (!(rec && objsize_rec.sub) && (sv->type.t & VT_ARRAY) && !(sv->type.t & VT_VLA) && oi->known0)
  {
    /* an array value: the closest subobject is that array */
    int align, sz = type_size(&sv->type, &align);
    oi->known1 = 1;
    oi->max1 = oi->min1 = sz > 0 && (unsigned long long)sz < oi->max0 ? (unsigned long long)sz : oi->max0;
  }
  else if (oi->known0)
  {
    oi->known1 = 1;
    oi->max1 = oi->max0;
    oi->min1 = oi->min0;
  }
}

/* __builtin_object_size's answer for TYPE */
unsigned long long objsize_result(const ObjInfo *oi, int type)
{
  int known = type & 1 ? oi->known1 : oi->known0;
  if (!known)
    return type & 2 ? 0 : (unsigned long long)(addr_t)-1;
  return type & 1 ? (type & 2 ? oi->min1 : oi->max1) : (type & 2 ? oi->min0 : oi->max0);
}

/* __builtin_dynamic_object_size where the static answer is unknown: push the
   run-time size and return 1, or return 0 */
int objsize_push_dynamic(const ObjInfo *oi, int type)
{
  if ((type & 1) && oi->known1)
    return 0;
  if (oi->vla && !oi->off_unknown && oi->off == 0)
  {
    int align;
    vpush_type_size(&oi->vla->type, &align);
    gen_cast_s(VT_SIZE_T);
    return 1;
  }
  if (oi->kind == OI_HEAP && oi->h.nsym && !oi->off_unknown && oi->off == 0)
  {
    Sym *n = oi->h.nsym;
    const SymLocalFacts *f = sym_facts_peek(n);
    if (!f || f->os_stores != oi->h.nstores || n->a.addrtaken)
      return 0;
    int r = n->r;
    if ((r & VT_VALMASK) < VT_CONST && !(r & VT_PARAM))
      r = (r & ~VT_VALMASK) | VT_LOCAL;
    vset(&n->type, r, n->c);
    vtop->sym = n;
    vtop->vr = n->vreg;
    gen_cast_s(VT_SIZE_T);
    if (oi->h.factor > 1)
    {
      vpushs(oi->h.factor);
      gen_op('*');
    }
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* The functions the checks know */

enum
{
  OF_MEM = 1,   /* memcpy(dst, src, len) and kin: the whole object */
  OF_STRCPY,    /* strcpy(dst, src) */
  OF_STRCAT,    /* strcat(dst, src) */
  OF_STRN,      /* strncpy(dst, src, len) */
  OF_STRNCAT,   /* strncat(dst, src, len) */
  OF_SPRINTF,   /* sprintf(dst, fmt, ...) */
  OF_SNPRINTF,  /* snprintf(dst, len, fmt, ...) */
  OF_ALLOC,     /* an allocator (size_a/size_b) */
  OF_FREE,      /* a deallocator only */
};

typedef struct ObjFn
{
  char name[14];
  unsigned char kind;
  signed char len, fmt; /* the length / format argument (from 0), -1 none */
  unsigned char va;     /* the arguments are a va_list */
  unsigned char size_a, size_b, dealloc_arg; /* from 1; 0 none */
  unsigned char heap;   /* the result is released by free */
} ObjFn;

static const ObjFn obj_fns[] = {
    {"memcpy", OF_MEM, 2, -1, 0, 0, 0, 0, 0},       {"memmove", OF_MEM, 2, -1, 0, 0, 0, 0, 0},
    {"mempcpy", OF_MEM, 2, -1, 0, 0, 0, 0, 0},      {"memset", OF_MEM, 2, -1, 0, 0, 0, 0, 0},
    {"bzero", OF_MEM, 1, -1, 0, 0, 0, 0, 0},        {"strcpy", OF_STRCPY, -1, -1, 0, 0, 0, 0, 0},
    {"stpcpy", OF_STRCPY, -1, -1, 0, 0, 0, 0, 0},   {"strcat", OF_STRCAT, -1, -1, 0, 0, 0, 0, 0},
    {"strncpy", OF_STRN, 2, -1, 0, 0, 0, 0, 0},     {"stpncpy", OF_STRN, 2, -1, 0, 0, 0, 0, 0},
    {"strncat", OF_STRNCAT, 2, -1, 0, 0, 0, 0, 0},  {"sprintf", OF_SPRINTF, -1, 1, 0, 0, 0, 0, 0},
    {"vsprintf", OF_SPRINTF, -1, 1, 1, 0, 0, 0, 0}, {"snprintf", OF_SNPRINTF, 1, 2, 0, 0, 0, 0, 0},
    {"vsnprintf", OF_SNPRINTF, 1, 2, 1, 0, 0, 0, 0},
    {"malloc", OF_ALLOC, -1, -1, 0, 1, 0, 0, 1},    {"calloc", OF_ALLOC, -1, -1, 0, 1, 2, 0, 1},
    {"realloc", OF_ALLOC, -1, -1, 0, 2, 0, 1, 1},   {"aligned_alloc", OF_ALLOC, -1, -1, 0, 2, 0, 0, 1},
    {"memalign", OF_ALLOC, -1, -1, 0, 2, 0, 0, 1},  {"alloca", OF_ALLOC, -1, -1, 0, 1, 0, 0, 0},
    {"strdup", OF_ALLOC, -1, -1, 0, 0, 0, 0, 1},    {"strndup", OF_ALLOC, -1, -1, 0, 0, 0, 0, 1},
    {"free", OF_FREE, -1, -1, 0, 0, 0, 1, 0},
};

static const ObjFn *obj_fn_named(const char *name)
{
  if (!strncmp(name, "__builtin_", 10))
    name += 10;
  /* most calls are to none of them: the first letter rules them out */
  switch (name[0])
  {
  case 'm':
  case 'b':
  case 's':
  case 'v':
  case 'c':
  case 'r':
  case 'a':
  case 'f':
    break;
  default:
    return NULL;
  }
  for (size_t i = 0; i < countof(obj_fns); i++)
    if (!strcmp(name, obj_fns[i].name))
      return &obj_fns[i];
  return NULL;
}

/* An allocation function's declaration AD (alloc_size, malloc, malloc(d, i))
   goes into its facts F; the deallocator it names learns which argument it
   releases. */
void objsize_apply_alloc_attrs(Sym *s, SymLocalFacts *f, AttributeDef *ad)
{
  if (ad->attr_alloc_a)
    f->alloc_size_a = ad->attr_alloc_a, f->alloc_size_b = ad->attr_alloc_b;
  if (ad->attr_malloc)
    f->alloc_malloc = 1;
  if (ad->attr_dealloc)
  {
    Sym *d = sym_find(ad->attr_dealloc);
    f->alloc_dealloc = ad->attr_dealloc;
    f->alloc_dealloc_arg = ad->attr_dealloc_arg > 0 && ad->attr_dealloc_arg < 256 ? ad->attr_dealloc_arg : 1;
    if (d && (d->type.t & VT_BTYPE) == VT_FUNC)
    {
      SymLocalFacts *df = sym_facts(d);
      d->a.attr_facts = 1;
      df->dealloc_arg = f->alloc_dealloc_arg;
    }
  }
  (void)s;
}

#define OC_MAXARGS 16

struct ObjCall
{
  Sym *fsym;
  const char *name;
  const ObjFn *fn;   /* by name, or NULL */
  unsigned char kind;
  unsigned char size_a, size_b, dealloc_arg, heap;
  int dealloc_tok;   /* the deallocator of what it returns */
  int line;
  unsigned char discarded, trunc_pending, at_end;
  int nargs;
  SValue arg[OC_MAXARGS];
  unsigned char have[OC_MAXARGS];
  ObjInfo dst, freed;       /* the destination, the pointer released */
  unsigned char mul_risky;  /* the size argument was a risky multiplication */
  ObjRec saved;
  unsigned char in_ctx;
  unsigned char fixit_done; /* its bound fix-it was given (objsize_bound_fixit) */
  Sym *nsym[2];             /* size arguments that are local variables */
  Sym *dep;                 /* deprecated: reported with the arguments in */
  int dep_line;
};

ObjCall *objsize_cur_call;

static int any_check_wanted(void)
{
  return tcc_warn_wanted(warn_stringop_overflow) || tcc_warn_wanted(warn_format_overflow) ||
         tcc_warn_wanted(warn_format_truncation) || tcc_warn_wanted(warn_alloc_size_larger_than) ||
         tcc_warn_wanted(warn_alloc_mul_overflow) || tcc_warn_wanted(warn_free_nonheap_object) ||
         tcc_warn_wanted(warn_mismatched_dealloc);
}

/* A call to FSYM (the function itself, NULL through a pointer) begins */
ObjCall *objsize_call_begin(Sym *fsym)
{
  const SymLocalFacts *f = NULL;
  const ObjFn *fn = NULL;
  ObjCall *oc;
  const char *name;
  int dep_line;

  if (!fsym || (fsym->type.t & VT_BTYPE) != VT_FUNC || tcc_state->reverse_funcargs)
    return NULL;
  if (fsym->a.attr_facts)
  {
    f = sym_facts_peek(fsym);
    if (f && !f->alloc_size_a && !f->alloc_malloc && !f->alloc_dealloc && !f->dealloc_arg)
      f = NULL;
  }
  name = get_tok_str(fsym->v, NULL);
  if (!(fsym->type.t & VT_STATIC) && !(tcc_state->no_builtin_funcs & NO_BUILTIN_ALL))
    fn = obj_fn_named(name);
  if (!f && !fn)
    return NULL; /* a waiting deprecated report goes out (unary_funcall) */
  /* a deprecated sprintf and kin, reported once the arguments are in */
  dep_line = attr_dep_call_take(fsym);
  /* the string and format functions matter to the warnings only */
  if (!f && fn->kind < OF_ALLOC && !any_check_wanted() && !dep_line)
    return NULL;
  oc = tcc_mallocz(sizeof *oc);
  oc->fsym = fsym;
  if (dep_line)
    oc->dep = fsym, oc->dep_line = dep_line > 0 ? dep_line : 0;
  oc->name = name;
  oc->fn = fn;
  oc->line = file ? file->line_num : 0;
  if (fn)
  {
    oc->kind = fn->kind;
    oc->size_a = fn->size_a, oc->size_b = fn->size_b;
    oc->dealloc_arg = fn->dealloc_arg;
    oc->heap = fn->heap;
    if (fn->heap)
      oc->dealloc_tok = DEALLOC_FREE;
  }
  if (f)
  {
    if (f->alloc_size_a)
      oc->size_a = f->alloc_size_a, oc->size_b = f->alloc_size_b;
    if (f->alloc_size_a || f->alloc_malloc || f->alloc_dealloc)
    {
      if (!oc->kind || oc->kind == OF_FREE)
        oc->kind = OF_ALLOC;
      oc->heap = f->alloc_malloc || f->alloc_dealloc || oc->heap;
    }
    if (f->alloc_dealloc)
      oc->dealloc_tok = f->alloc_dealloc;
    if (f->dealloc_arg)
    {
      oc->dealloc_arg = f->dealloc_arg;
      if (!oc->kind)
        oc->kind = OF_FREE;
    }
  }
  return oc;
}

/* whether argument I is a pointer the checks look into */
static int arg_wanted(ObjCall *oc, int i)
{
  if (oc->kind && oc->kind < OF_ALLOC && i == 0)
    return 1;
  if (oc->dealloc_arg && i + 1 == oc->dealloc_arg)
    return 1;
  /* a size: whether it is a multiplication (-Walloc-mul-overflow) */
  if (oc->kind == OF_ALLOC && oc->size_a && !oc->size_b && i + 1 == oc->size_a &&
      tcc_warn_wanted(warn_alloc_mul_overflow))
    return 1;
  return 0;
}

/* argument I is about to be parsed */
void objsize_call_arg_before(ObjCall *oc, int i)
{
  last_mul.on = 0;
  if (arg_wanted(oc, i))
  {
    objsize_enter(&oc->saved);
    oc->in_ctx = 1;
  }
}

/* argument I is on vtop, as written */
void objsize_call_arg(ObjCall *oc, int i)
{
  if (oc->in_ctx)
  {
    if (oc->kind && oc->kind < OF_ALLOC && i == 0)
      objsize_info(vtop, &oc->dst);
    if (oc->dealloc_arg && i + 1 == oc->dealloc_arg)
    {
      objsize_info(vtop, &oc->freed);
      /* the result of an allocation, given straight to it */
      if (oc->freed.kind == OI_UNKNOWN && last_alloc.on && sv_same(vtop, &last_alloc.sv))
      {
        oc->freed.kind = OI_HEAP;
        oc->freed.h = last_alloc.h;
      }
    }
    objsize_leave(&oc->saved);
    oc->in_ctx = 0;
  }
  if (oc->kind == OF_ALLOC && (i + 1 == oc->size_a || i + 1 == oc->size_b))
  {
    if (last_mul.on && sv_same(vtop, &last_mul.sv))
      oc->mul_risky = last_mul.risky;
    /* malloc(n): n, for __builtin_dynamic_object_size */
    Sym *n = vtop->sym;
    if (n && n->vreg >= 0 && vtop->vr == n->vreg && !(vtop->r & VT_SYM) && is_integer_btype(n->type.t & VT_BTYPE) &&
        !(n->type.t & (VT_ARRAY | VT_BITFIELD)) && (n->type.t & VT_BTYPE) == (vtop->type.t & VT_BTYPE) &&
        !n->a.addrtaken && !(n->v & (SYM_FIELD | SYM_STRUCT)) && (n->v & ~SYM_FIELD) < SYM_FIRST_ANOM &&
        ((n->r & VT_VALMASK) == VT_LOCAL || ((n->r & VT_VALMASK) < VT_CONST && (n->r & VT_PARAM))))
      oc->nsym[i + 1 == oc->size_a ? 0 : 1] = n;
  }
  last_mul.on = 0;
  if (i < OC_MAXARGS)
  {
    oc->arg[i] = *vtop;
    oc->have[i] = 1;
  }
  oc->nargs = i + 1;
}

/* ------------------------------------------------------------------------- */
/* The checks */

static int arg_uconst(ObjCall *oc, int i, unsigned long long *v)
{
  SValue *sv;
  if (i < 0 || i >= OC_MAXARGS || !oc->have[i])
    return 0;
  sv = &oc->arg[i];
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST || !is_integer_btype(sv->type.t & VT_BTYPE))
    return 0;
  *v = (unsigned long long)sv->c.i;
  if (!(sv->type.t & VT_UNSIGNED) && (int64_t)sv->c.i < 0)
    *v = (unsigned long long)(uint32_t)(int32_t)sv->c.i; /* converted to a 32-bit size_t */
  else if ((sv->type.t & VT_BTYPE) != VT_LLONG)
    *v &= 0xffffffffull;
  return 1;
}

static int arg_sconst(ObjCall *oc, int i, int64_t *v)
{
  SValue *sv;
  if (i < 0 || i >= OC_MAXARGS || !oc->have[i])
    return 0;
  sv = &oc->arg[i];
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST || !is_integer_btype(sv->type.t & VT_BTYPE))
    return 0;
  *v = sv->c.i;
  if ((sv->type.t & VT_BTYPE) != VT_LLONG)
    *v = (sv->type.t & VT_UNSIGNED) ? (int64_t)(uint32_t)*v : (int64_t)(int32_t)*v;
  return 1;
}

static const char *arg_string(ObjCall *oc, int i, int *len)
{
  if (i < 0 || i >= OC_MAXARGS || !oc->have[i])
    return NULL;
  return try_get_constant_string(&oc->arg[i], len);
}

/* The shortest output of printf format FMT (FLEN bytes) with the arguments
   from FIRST (VA: a va_list, nothing known of them).  0 when it cannot say. */
static int fmt_min_length(ObjCall *oc, const char *fmt, int flen, int first, int va, unsigned long long *out)
{
  const char *p = fmt, *end = fmt + flen;
  unsigned long long n = 0;
  int argi = first;

  while (p < end)
  {
    char spec[48], flags[8];
    int nf = 0, width = 0, prec = -1, lm = 0, minus = 0, plus = 0, space = 0, alt = 0, zero = 0;
    long long len;
    char c;
    if (*p != '%')
    {
      n++, p++;
      continue;
    }
    p++;
    if (p < end && *p == '%')
    {
      n++, p++;
      continue;
    }
    /* %1$d: operands by number are not followed */
    {
      const char *q = p;
      while (q < end && is_digit_c(*q))
        q++;
      if (q < end && *q == '$')
        return 0;
    }
    while (p < end && strchr("-+ #0'", *p))
    {
      if (*p == '-')
        minus = 1;
      else if (*p == '+')
        plus = 1;
      else if (*p == ' ')
        space = 1;
      else if (*p == '#')
        alt = 1;
      else if (*p == '0')
        zero = 1;
      p++;
    }
    if (p < end && *p == '*')
    {
      int64_t w;
      p++;
      if (!va && arg_sconst(oc, argi, &w))
        width = w < 0 ? (minus = 1, (int)-w) : (int)w;
      argi++;
    }
    else
      while (p < end && is_digit_c(*p))
        width = width * 10 + (*p++ - '0');
    if (p < end && *p == '.')
    {
      p++;
      prec = 0;
      if (p < end && *p == '*')
      {
        int64_t w;
        p++;
        prec = -2; /* unknown */
        if (!va && arg_sconst(oc, argi, &w))
          prec = w < 0 ? -1 : (int)w;
        argi++;
      }
      else
        while (p < end && is_digit_c(*p))
          prec = prec * 10 + (*p++ - '0');
    }
    if (width > 4096 || prec > 4096)
      return 0;
    /* length modifiers: 1 hh, 2 h, 3 l, 4 ll / q / j, 5 L, 6 z / t */
    if (p < end && *p == 'h')
      lm = (p + 1 < end && p[1] == 'h') ? (p += 2, 1) : (p++, 2);
    else if (p < end && *p == 'l')
      lm = (p + 1 < end && p[1] == 'l') ? (p += 2, 4) : (p++, 3);
    else if (p < end && (*p == 'q' || *p == 'j'))
      lm = 4, p++;
    else if (p < end && *p == 'L')
      lm = 5, p++;
    else if (p < end && (*p == 'z' || *p == 't'))
      lm = 6, p++;
    if (p >= end)
      return 0;
    c = *p++;
    if (minus)
      flags[nf++] = '-';
    if (plus)
      flags[nf++] = '+';
    if (space)
      flags[nf++] = ' ';
    if (alt)
      flags[nf++] = '#';
    if (zero)
      flags[nf++] = '0';
    flags[nf] = 0;
    switch (c)
    {
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
    case 'o':
    {
      int64_t v;
      if (!va && arg_sconst(oc, argi, &v) && prec != -2)
      {
        int sign = c == 'd' || c == 'i';
        /* the value as the conversion reads it */
        switch (lm)
        {
        case 1:
          v = sign ? (int64_t)(signed char)v : (int64_t)(unsigned char)v;
          break;
        case 2:
          v = sign ? (int64_t)(short)v : (int64_t)(unsigned short)v;
          break;
        case 4:
          break;
        default:
          v = sign ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
          break;
        }
        if (prec >= 0)
          snprintf(spec, sizeof spec, "%%%s%d.%dll%c", flags, width, prec, c);
        else
          snprintf(spec, sizeof spec, "%%%s%dll%c", flags, width, c);
        len = sign ? snprintf(NULL, 0, spec, (long long)v) : snprintf(NULL, 0, spec, (unsigned long long)v);
      }
      else
      {
        len = prec == -2 ? 0 : prec >= 0 ? prec : 1;
        if ((c == 'd' || c == 'i') && (plus || space))
          len++;
        if (len < width)
          len = width;
      }
      argi++;
      break;
    }
    case 'c':
      len = width > 1 ? width : 1;
      argi++;
      break;
    case 's':
    {
      int sl;
      const char *s = va || lm ? NULL : arg_string(oc, argi, &sl);
      len = 0;
      if (s && prec != -2)
        len = prec >= 0 && prec < sl ? prec : sl;
      if (len < width)
        len = width;
      argi++;
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    {
      SValue *sv = !va && argi < OC_MAXARGS && oc->have[argi] ? &oc->arg[argi] : NULL;
      if (sv && (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && is_float(sv->type.t) && prec != -2)
      {
        if (prec >= 0)
          snprintf(spec, sizeof spec, "%%%s%d.%d%c", flags, width, prec, c);
        else
          snprintf(spec, sizeof spec, "%%%s%d%c", flags, width, c);
        len = snprintf(NULL, 0, spec, get_const_double(sv));
      }
      else
      {
        len = c == 'g' || c == 'G' ? 1 : 3; /* "0", "inf" */
        if (plus || space)
          len++;
        if (len < width)
          len = width;
      }
      argi++;
      break;
    }
    case 'a':
    case 'A':
    case 'p':
      len = width > 1 ? width : 1;
      argi++;
      break;
    case 'n':
      len = 0;
      argi++;
      break;
    case 'm':
      len = width;
      break;
    default:
      return 0;
    }
    if (len < 0)
      return 0;
    n += (unsigned long long)len;
  }
  *out = n;
  return 1;
}

/* the size of the destination for the check of KIND */
static int dst_size(ObjCall *oc, int closest, unsigned long long *size)
{
  ObjInfo *d = &oc->dst;
  int level = tcc_state->stringop_level ? tcc_state->stringop_level : 2;
  if (closest && level >= 2 ? !d->known1 : !d->known0)
    return 0;
  *size = closest && level >= 2 ? d->max1 : d->max0;
  return 1;
}

static void check_stringop(ObjCall *oc)
{
  unsigned long long size, n;
  int sl;
  const char *s;

  if (!tcc_state->warn_stringop_overflow && !tcc_state->nb_diag_snaps)
    return;
  switch (oc->kind)
  {
  case OF_MEM:
    if (dst_size(oc, 0, &size) && arg_uconst(oc, oc->fn->len, &n) && n > size)
      tcc_warning_once(warn_stringop_overflow, "'%s' writing %llu bytes into a region of size %llu overflows the destination",
                       oc->name, n, size);
    break;
  case OF_STRCPY:
  case OF_STRCAT:
    if (dst_size(oc, 1, &size) && (s = arg_string(oc, 1, &sl)) && (unsigned long long)sl + 1 > size)
    {
      tcc_warning_once(warn_stringop_overflow, "'%s' writing %llu bytes into a region of size %llu overflows the destination",
                       oc->name, (unsigned long long)sl + 1, size);
      objsize_bound_fixit(oc, 0); /* strcpy -> strlcpy */
    }
    break;
  case OF_STRN:
    if (dst_size(oc, 1, &size) && arg_uconst(oc, oc->fn->len, &n) && n > size)
      tcc_warning_once(warn_stringop_overflow, "'%s' writing %llu bytes into a region of size %llu overflows the destination",
                       oc->name, n, size);
    break;
  case OF_STRNCAT:
    if (dst_size(oc, 1, &size) && arg_uconst(oc, oc->fn->len, &n) && n >= size)
      tcc_warning_once(warn_stringop_overflow, "'%s' specified bound %llu %s destination size %llu", oc->name, n,
                       n == size ? "equals" : "exceeds", size);
    break;
  case OF_SNPRINTF:
    if (dst_size(oc, 1, &size) && arg_uconst(oc, oc->fn->len, &n) && n > size)
      tcc_warning_once(warn_stringop_overflow, "'%s' specified bound %llu exceeds destination size %llu", oc->name, n,
                       size);
    break;
  }
}

static void check_format(ObjCall *oc)
{
  unsigned long long size, out, bound;
  int flen;
  const char *fmt;
  const ObjFn *fn = oc->fn;

  if (oc->kind == OF_SPRINTF ? !tcc_warn_wanted(warn_format_overflow) : !tcc_warn_wanted(warn_format_truncation))
    return;
  /* the truncation check waits for the end of the call: whether its result
     is used is known there */
  if (oc->kind == OF_SNPRINTF && !oc->at_end)
  {
    oc->trunc_pending = 1;
    return;
  }
  fmt = arg_string(oc, fn->fmt, &flen);
  if (!fmt || !fmt_min_length(oc, fmt, flen, fn->fmt + 1, fn->va, &out))
    return;
  if (oc->kind == OF_SPRINTF)
  {
    if (dst_size(oc, 1, &size) && out + 1 > size)
    {
      tcc_warning_once(warn_format_overflow,
                       "'%s' writing at least %llu bytes into a region of size %llu overflows the destination", oc->name,
                       out + 1, size);
      objsize_bound_fixit(oc, 0); /* sprintf -> snprintf */
    }
    return;
  }
  /* level 1 (-Wall): only when nothing reads the result, as gcc */
  if (!arg_uconst(oc, fn->len, &bound) || !bound || out < bound)
    return;
  if (!oc->discarded && (tcc_state->format_truncation_level ? tcc_state->format_truncation_level : 1) < 2)
    return;
  tcc_warning_once(warn_format_truncation, "'%s' output truncated: at least %llu bytes into a destination of size %llu",
                   oc->name, out + 1, bound);
}

static void check_alloc(ObjCall *oc)
{
  unsigned long long a, b, limit = tcc_state->alloc_size_limit_set ? tcc_state->alloc_size_limit : 0x7fffffffull;

  if (tcc_warn_wanted(warn_alloc_size_larger_than) && oc->size_a && arg_uconst(oc, oc->size_a - 1, &a))
  {
    if (!oc->size_b && a > limit)
      tcc_warning_once(warn_alloc_size_larger_than, "argument %d value '%llu' exceeds maximum object size %llu",
                       oc->size_a, a, limit);
    else if (oc->size_b && arg_uconst(oc, oc->size_b - 1, &b) && b && (a > limit / b || a * b > limit))
      tcc_warning_once(warn_alloc_size_larger_than,
                       "product '%llu' of arguments %d and %d exceeds maximum object size %llu", a * b, oc->size_a,
                       oc->size_b, limit);
  }
  if (oc->mul_risky && oc->size_a && !oc->size_b && tcc_warn_wanted(warn_alloc_mul_overflow))
    tcc_warning_once(warn_alloc_mul_overflow, "the size argument of '%s' is a multiplication that may overflow: use %s",
                     oc->name, oc->dealloc_arg ? "'reallocarray'" : "'calloc' or 'reallocarray'");
}

static const char *strip_builtin(const char *n)
{
  return strncmp(n, "__builtin_", 10) ? n : n + 10;
}

/* the deallocator DEALLOC (a token, or DEALLOC_FREE) is OC's function: free
   and realloc release what the free family allocates */
static int dealloc_matches(int dealloc, ObjCall *oc)
{
  const char *callee = strip_builtin(oc->name);
  if (dealloc == DEALLOC_FREE || !strcmp(strip_builtin(get_tok_str(dealloc, NULL)), "free"))
    return !strcmp(callee, "free") || !strcmp(callee, "realloc") || !strcmp(callee, "reallocarray");
  return !strcmp(strip_builtin(get_tok_str(dealloc, NULL)), callee);
}

static void check_dealloc(ObjCall *oc)
{
  ObjInfo *p = &oc->freed;

  if (p->kind == OI_DECL && tcc_warn_wanted(warn_free_nonheap_object))
  {
    Sym *o = p->obj;
    if (o && (o->v & ~SYM_FIELD) < SYM_FIRST_ANOM)
      tcc_warning_once(warn_free_nonheap_object, "'%s' called on unallocated object '%s'", oc->name,
                       get_tok_str(o->v & ~SYM_FIELD, NULL));
    else
      tcc_warning_once(warn_free_nonheap_object, "'%s' called on a pointer to an unallocated object", oc->name);
    return;
  }
  if (p->kind != OI_HEAP || p->h.decl)
    return;
  if (!p->off_unknown && p->off && tcc_warn_wanted(warn_free_nonheap_object))
  {
    tcc_warning_once(warn_free_nonheap_object, "'%s' called on a pointer to an allocated object with nonzero offset %lld",
                     oc->name, p->off);
    return;
  }
  if (p->h.dealloc && p->h.dealloc != -1 && !dealloc_matches(p->h.dealloc, oc) &&
      tcc_warn_wanted(warn_mismatched_dealloc))
  {
    int shown;
    tcc_warning_once(warn_mismatched_dealloc, "'%s' called on pointer returned from a mismatched allocation function",
                     oc->name);
    shown = tcc_state->diag_last_shown;
    if (shown && p->h.alloc_tok && p->h.alloc_line)
      tcc_note("%i:returned from '%s'", p->h.alloc_line, get_tok_str(p->h.alloc_tok, NULL));
  }
}

/* sprintf -> snprintf and kin, after a diagnostic of OC (named at LINE)
 * just shown: when the destination's size is exact (a declared array, a
 * member, a constant allocation, at a known offset), the note "use
 * snprintf(buf, sizeof buf, ...)" and, when the call's name and arguments up
 * to the one the size follows are on that line, the fix-its: the new name
 * and `, sizeof buf` after the first argument (strlcpy, strlcat: after the
 * second).  `sizeof buf` when the destination is written as the name of a
 * char array, else the size as a constant.  Once per call; 1 when the note
 * was given. */
int objsize_bound_fixit(ObjCall *oc, int line)
{
  static const struct
  {
    const char *from, *to, *use;
    unsigned char after; /* the argument the size follows */
  } bf[] = {
      {"sprintf", "snprintf", "%s(%s, %s, ...), which bounds the output", 0},
      {"vsprintf", "vsnprintf", "%s(%s, %s, fmt, ap), which bounds the output", 0},
      {"strcpy", "strlcpy", "%s(%s, src, %s), which bounds the copy", 1},
      {"strcat", "strlcat", "%s(%s, src, %s), which bounds the copy", 1},
  };
  const ObjInfo *d = &oc->dst;
  char dst[64], size[80], append[96];
  int i, found, n = (int)countof(bf);
  Sym *o = d->obj;

  if (oc->fixit_done || !tcc_state->diag_last_shown)
    return 0;
  for (i = 0; i < n && strcmp(oc->name, bf[i].from); i++)
    ;
  if (i == n || !d->known1 || d->min1 != d->max1 || !d->max1)
    return 0;
  oc->fixit_done = 1;
  found = tcc_fixit_call(line ? line : oc->line, bf[i].from, bf[i].after, dst, sizeof dst);
  snprintf(size, sizeof size, "%llu", d->max1);
  if (found && d->kind == OI_DECL && o && !d->off && !d->off_unknown && (o->type.t & VT_ARRAY) &&
      !(o->type.t & VT_VLA) && o->type.ref && btype_size(o->type.ref->type.t & VT_BTYPE) == 1 &&
      !strcmp(dst, get_tok_str(o->v & ~SYM_FIELD, NULL)))
  {
    int align;
    if ((unsigned long long)type_size(&o->type, &align) == d->max1)
      snprintf(size, sizeof size, "sizeof %s", dst);
  }
  if (!found)
    strcpy(dst, "dst");
  {
    char use[160];
    snprintf(use, sizeof use, bf[i].use, bf[i].to, dst, size);
    tcc_note("%i:use %s", line ? line : oc->line, use);
  }
  if (found)
  {
    snprintf(append, sizeof append, ", %s", size);
    tcc_fixit_call_emit(bf[i].to, append);
  }
  return 1;
}

/* the arguments are parsed; DISCARDED: nothing reads the call's result */
void objsize_call_args_end(ObjCall *oc)
{
  if (oc->dep)
  {
    attr_dep_call_report(oc->dep, oc->dep_line, oc);
    oc->dep = NULL;
  }
  if (!fe_eval_checks_wanted() || tcc_state->fe_speculative)
    return;
  if (oc->kind && oc->kind < OF_ALLOC)
  {
    check_stringop(oc);
    if (oc->kind == OF_SPRINTF || oc->kind == OF_SNPRINTF)
      check_format(oc);
  }
  if (oc->kind == OF_ALLOC)
    check_alloc(oc);
  if (oc->dealloc_arg)
    check_dealloc(oc);
}

/* the call is done: vtop is its result */
void objsize_call_end(ObjCall *oc, int discarded)
{
  if (oc->trunc_pending)
  {
    oc->at_end = 1;
    oc->discarded = discarded;
    check_format(oc);
  }
  if (oc->in_ctx)
    objsize_leave(&oc->saved);
  if (oc->kind == OF_ALLOC && (vtop->type.t & VT_BTYPE) == VT_PTR)
  {
    ObjHeap h;
    unsigned long long a, b = 1;
    memset(&h, 0, sizeof h);
    if (oc->size_a && arg_uconst(oc, oc->size_a - 1, &a) && (!oc->size_b || arg_uconst(oc, oc->size_b - 1, &b)) &&
        (!b || a <= 0xffffffffull / b))
    {
      h.known = 1;
      h.lo = h.hi = (unsigned)(a * b);
    }
    else if (oc->size_a && oc->nsym[0] && (!oc->size_b || arg_uconst(oc, oc->size_b - 1, &b)) && b < 0x10000)
    {
      /* malloc(n), calloc(n, k): n is followed for the dynamic size */
      SymLocalFacts *nf = sym_facts(oc->nsym[0]);
      nf->os_watch = 1;
      objsize_nb_watch = 1;
      h.nsym = oc->nsym[0];
      h.nstores = nf->os_stores;
      h.factor = (unsigned)b;
    }
    else if (oc->size_b && oc->nsym[1] && arg_uconst(oc, oc->size_a - 1, &a) && a < 0x10000)
    {
      SymLocalFacts *nf = sym_facts(oc->nsym[1]);
      nf->os_watch = 1;
      objsize_nb_watch = 1;
      h.nsym = oc->nsym[1];
      h.nstores = nf->os_stores;
      h.factor = (unsigned)a;
    }
    h.dealloc = oc->heap ? (oc->dealloc_tok ? oc->dealloc_tok : -1) : -1;
    h.alloc_tok = oc->fsym->v;
    h.alloc_line = oc->line;
    if (objsize_ctx)
      rec_from_alloc(&h);
    if (!NOEVAL_WANTED)
    {
      last_alloc.sv = *vtop;
      last_alloc.h = h;
      last_alloc.on = 1;
    }
  }
  tcc_free(oc);
}

/* -------------------------------------------------------------------------
 * __builtin___memcpy_chk and kin (chk.c): a constant length over a known
 * object size is a sure overflow */
void objsize_chk_overflow(const char *name, unsigned long long n, unsigned long long size)
{
  if (fe_eval_checks_wanted() && !tcc_state->fe_speculative && tcc_warn_wanted(warn_stringop_overflow))
    tcc_warning_once(warn_stringop_overflow, "'%s' writing %llu bytes into a region of size %llu overflows the destination",
                     name, n, size);
}

/* -------------------------------------------------------------------------
 * -Warray-bounds in loops: `for (i = C0; i < N; i++)` (or <=, or N > i, ...)
 * with i a local integer variable and C0 <= the last value, and a[i] or
 * a[i + k] in the body where the array's size is known: the last trip's
 * index is checked against it.  Only an access no branch comes before in the
 * body (if, switch, loops, break, continue, goto, return, ?:, && and || at
 * the loop's own level -- an inline expansion's branches are its own), and
 * only when the body stores nothing to i and its address is not taken.
 * Decided by the parser, so the same at every -O; a value-range analysis on
 * SSA (while loops, other strides, conditions in the body) is not done. */

enum
{
  AB_INIT = 1,
  AB_COND,
  AB_STEP,
  AB_BODY
};

typedef struct AbPending
{
  int line;
  long long idx;
  char type[64];
} AbPending;

typedef struct AbLoop
{
  struct AbLoop *prev;
  unsigned char phase, ok, branchy, step_ok;
  int depth;          /* the inline expansion it is parsed in */
  int ivr;            /* i's vreg */
  Sym *isym;
  long long max;      /* the last value i takes */
  int nstores;        /* stores to i in the body */
  int ninit;
  int init_vr[4];
  long long init_val[4];
  unsigned char init_const[4];
  SValue cmp_res;     /* the condition's comparison */
  SValue cmp_a, cmp_b;
  int cmp_op;
  unsigned char have_cmp;
  int npend;
  AbPending pend[4];
} AbLoop;

int objsize_ab_watch;   /* AbLoops are open */
static AbLoop *ab_top;
/* the last '+' parsed while a loop is watched: for the step and a[i + k] */
static struct
{
  SValue res, a, b;
  unsigned char on;
} ab_add;

static int ab_int_local(const SValue *sv)
{
  Sym *s = sv->sym;
  return s && (sv->r & VT_LVAL) && sv->vr >= 0 && sv->vr == s->vreg && is_integer_btype(s->type.t & VT_BTYPE) &&
         !(s->type.t & (VT_ARRAY | VT_BITFIELD)) && (s->r & VT_VALMASK) == VT_LOCAL &&
         !(s->v & (SYM_FIELD | SYM_STRUCT)) && !(sv->r & VT_SYM);
}

static int ab_const(const SValue *sv, long long *v)
{
  int big;
  if (!fe_int_const(sv))
    return 0;
  *v = fe_const_value(sv, &big);
  return !big;
}

void objsize_for_begin(void)
{
  AbLoop *l;
  if (!tcc_warn_wanted(warn_array_bounds) || NOEVAL_WANTED || pp_expr)
    return;
  l = tcc_mallocz(sizeof *l);
  l->prev = ab_top;
  l->phase = AB_INIT;
  l->depth = tcc_state->in_inline_expansion;
  ab_top = l;
  objsize_ab_watch++;
}

/* the for statement's next part begins (AB_COND, AB_STEP, AB_BODY) */
void objsize_for_phase(int phase)
{
  AbLoop *l = ab_top;
  long long n, c0;
  int i;
  if (!objsize_ab_watch || !l)
    return;
  if (phase == AB_STEP)
  {
    /* the condition: one comparison of i against a constant */
    SValue *iv = NULL;
    int op = l->cmp_op;
    l->ok = 0;
    if (!l->have_cmp || !sv_same(vtop, &l->cmp_res))
      goto out;
    if (ab_int_local(&l->cmp_a) && ab_const(&l->cmp_b, &n))
      iv = &l->cmp_a;
    else if (ab_int_local(&l->cmp_b) && ab_const(&l->cmp_a, &n))
    {
      iv = &l->cmp_b;
      /* N > i is i < N */
      op = op == TOK_GT ? TOK_LT : op == TOK_GE ? TOK_LE : op == TOK_UGT ? TOK_ULT : op == TOK_UGE ? TOK_ULE : 0;
    }
    if (!iv)
      goto out;
    if (op == TOK_LT || op == TOK_ULT)
      l->max = n - 1;
    else if (op == TOK_LE || op == TOK_ULE)
      l->max = n;
    else
      goto out;
    l->ivr = iv->vr;
    l->isym = iv->sym;
    /* the start: a constant stored to i in the init, at most the last value */
    for (i = l->ninit - 1; i >= 0; i--)
      if (l->init_vr[i] == l->ivr)
        break;
    if (i < 0 || !l->init_const[i] || (c0 = l->init_val[i]) > l->max || l->max > 0x7fffffff || c0 < -0x7fffffff)
      goto out;
    l->ok = 1;
  }
  else if (phase == AB_BODY && l->ok && !l->step_ok)
    l->ok = 0;
out:
  l->phase = phase;
  ab_add.on = 0;
}

/* the for statement ends */
void objsize_for_end(void)
{
  AbLoop *l = ab_top;
  if (!objsize_ab_watch || !l)
    return;
  if (l->ok && !l->nstores && l->isym && !l->isym->a.addrtaken)
    for (int i = 0; i < l->npend; i++)
    {
      tcc_warning_once_at(warn_array_bounds, l->pend[i].line, "array subscript %lld is above array bounds of '%s'",
                          l->pend[i].idx, l->pend[i].type);
      /* -Warray-bounds=2 finds it too: said once */
      if (tcc_state->ir && tcc_state->ir->ab_on)
        tcc_ir_ab_said(tcc_state->ir, l->pend[i].line, l->pend[i].idx);
    }
  ab_top = l->prev;
  objsize_ab_watch--;
  tcc_free(l);
}

/* a statement or an operator that branches: in a loop body, what follows
   it may not run on the last trip */
void objsize_branch(void)
{
  for (AbLoop *l = ab_top; l; l = l->prev)
    if (l->phase == AB_BODY && l->depth == tcc_state->in_inline_expansion)
      l->branchy = 1;
}

void objsize_branch_stmt(int t)
{
  if (t == TOK_IF || t == TOK_SWITCH || t == TOK_WHILE || t == TOK_DO || t == TOK_FOR || t == TOK_BREAK ||
      t == TOK_CONTINUE || t == TOK_GOTO || t == TOK_RETURN)
    objsize_branch();
}

/* gen_op is about to apply OP to vtop[-1], vtop (a loop is watched) */
void objsize_ab_op(int op, int after)
{
  AbLoop *l = ab_top;
  static SValue a, b;
  static int pending_op;
  if (!l)
    return;
  if (!after)
  {
    pending_op = 0;
    if ((l->phase == AB_COND && TOK_ISCOND(op)) || op == '+')
    {
      a = vtop[-1], b = vtop[0];
      pending_op = op;
    }
    return;
  }
  if (!pending_op || pending_op != op)
    return;
  pending_op = 0;
  if (op == '+')
  {
    ab_add.res = *vtop, ab_add.a = a, ab_add.b = b, ab_add.on = 1;
    return;
  }
  l->cmp_res = *vtop, l->cmp_a = a, l->cmp_b = b, l->cmp_op = op, l->have_cmp = 1;
}

/* vstore: DST was given SRC (a loop is watched) */
void objsize_ab_store(SValue *dst, SValue *src)
{
  AbLoop *l;
  long long v = 0;
  if (dst->vr < 0)
    return;
  for (l = ab_top; l; l = l->prev)
  {
    if (l->phase == AB_INIT && l == ab_top && l->ninit < 4)
    {
      l->init_vr[l->ninit] = dst->vr;
      l->init_const[l->ninit] = ab_const(src, &v);
      l->init_val[l->ninit] = v;
      l->ninit++;
    }
    else if (l->phase == AB_STEP && l == ab_top && dst->vr == l->ivr)
    {
      /* i++, ++i, i += 1, i = i + 1 */
      const SValue *o = NULL;
      long long k = 0;
      if (ab_add.on && sv_same(src, &ab_add.res))
      {
        if (ab_const(&ab_add.b, &k))
          o = &ab_add.a;
        else if (ab_const(&ab_add.a, &k))
          o = &ab_add.b;
      }
      l->step_ok = !l->step_ok && o && k == 1 &&
                   (o->vr == l->ivr || TCCIR_DECODE_VREG_TYPE(o->vr) == TCCIR_VREG_TYPE_TEMP);
    }
    else if (l->phase == AB_BODY && dst->vr == l->ivr)
      l->nstores++;
  }
}

/* base[index] is on the value stack (a loop is watched): IX gets what the
   check needs at the subscript's end */
int objsize_ab_subscript_begin(AbSub *ix, int flex)
{
  SValue *base = vtop - 1, *idx = vtop;
  long long k = 0;
  int n;
  if (flex || !(base->type.t & VT_ARRAY) || (base->type.t & VT_VLA) || !base->type.ref || base->type.ref->c < 0 ||
      base->union_member)
    return 0;
  n = base->type.ref->c;
  if (!ab_int_local(idx))
  {
    if (!ab_add.on || !sv_same(idx, &ab_add.res))
      return 0;
    if (ab_int_local(&ab_add.a) && ab_const(&ab_add.b, &k))
      idx = &ab_add.a;
    else if (ab_int_local(&ab_add.b) && ab_const(&ab_add.a, &k))
      idx = &ab_add.b;
    else
      return 0;
  }
  for (AbLoop *l = ab_top; l; l = l->prev)
    if (l->ok && l->phase == AB_BODY && !l->branchy && l->ivr == idx->vr && l->depth == tcc_state->in_inline_expansion)
    {
      CType ct = base->type;
      ix->loop = l;
      ix->idx = l->max + k;
      ix->n = n;
      ix->line = file ? file->line_num : 0;
      ct.t &= ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE);
      type_to_str(ix->type, sizeof ix->type, &ct, NULL);
      for (char *p = ix->type; (p = strstr(p, " [")) != NULL;)
        memmove(p, p + 1, strlen(p));
      return ix->idx >= n;
    }
  return 0;
}

/* ... and ended: ONE_PAST_OK when &a[n] is all it was */
void objsize_ab_subscript_end(AbSub *ix, int one_past_ok)
{
  AbLoop *l = ix->loop, *p;
  if (ix->idx == ix->n && one_past_ok)
    return;
  for (p = ab_top; p && p != l; p = p->prev)
    ;
  if (!p || l->npend >= 4)
    return;
  l->pend[l->npend].line = ix->line;
  l->pend[l->npend].idx = ix->idx;
  pstrcpy(l->pend[l->npend].type, sizeof l->pend[l->npend].type, ix->type);
  l->npend++;
}
