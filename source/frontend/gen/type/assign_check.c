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

/* assign_check.c -- Assignment-cast checking and pointer-compatibility diagnostics.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Recursively mark value (non-padding) bytes of TYPE at BASE offset into MAP.
 * MAP is a byte array of MAP_SIZE bytes; map[i]=1 means "value byte", 0 means
 * "padding".  Caller pre-zeroes MAP.  Returns 0 on success, -1 if the type
 * contains a VLA member or other structure we can't statically analyze (in
 * which case the caller should not emit any clear-padding stores). */
int mark_value_bytes(CType *type, int base, unsigned char *map, int map_size)
{
  int align, size;
  int bt = type->t & VT_BTYPE;

  /* VLA member inside a struct: layout can't be statically determined.
   * The top-level VLA pointer case is handled by the caller (size<=0). */
  if (type->t & VT_VLA)
    return -1;

  /* Bitfield: mark the storage-unit bytes the field touches as value bytes.
   * Bits not used by this bitfield but inside the same byte may be used by
   * an adjacent bitfield, so we conservatively never clear those bytes. */
  if (type->t & VT_BITFIELD)
  {
    int bpos = BIT_POS(type->t);
    int bsize_bits = BIT_SIZE(type->t);
    int span = (bpos + bsize_bits + 7) / 8;
    for (int i = 0; i < span; i++)
    {
      int idx = base + i;
      if (idx >= 0 && idx < map_size)
        map[idx] = 1;
    }
    return 0;
  }

  size = type_size(type, &align);
  if (size <= 0)
    return 0;

  if (bt == VT_STRUCT)
  {
    Sym *sref = type->ref;
    if (!sref)
      return -1;
    for (Sym *f = sym_first(sref); f; f = f->next)
    {
      int rc = mark_value_bytes(&f->type, base + f->c, map, map_size);
      if (rc < 0)
        return rc;
    }
    return 0;
  }

  if ((type->t & VT_ARRAY) && bt == VT_PTR)
  {
    Sym *sref = type->ref;
    int nelem = sref->c;
    if (nelem <= 0)
      return 0;
    int eal;
    int esize = type_size(&sref->type, &eal);
    if (esize <= 0)
      return 0;
    for (int i = 0; i < nelem; i++)
    {
      int rc = mark_value_bytes(&sref->type, base + i * esize, map, map_size);
      if (rc < 0)
        return rc;
    }
    return 0;
  }

  /* Scalar, pointer, function pointer, etc.: every byte is a value byte. */
  for (int i = 0; i < size; i++)
  {
    int idx = base + i;
    if (idx >= 0 && idx < map_size)
      map[idx] = 1;
  }
  return 0;
}

/* modify type so that its it is a pointer to type.  The pointer's Sym is
   shared by every pointer to the same type (sym_pointer_node): it must not
   be written. */
ST_FUNC void mk_pointer(CType *type)
{
  type->ref = sym_pointer_node(type);
  type->t = VT_PTR | (type->t & VT_STORAGE);
}

/* mk_pointer for a pointer with yasc qualifiers (YASC_*): its own interned
   node, so `T * owned` and `T *` differ only there. */
ST_FUNC void mk_pointer_yasc(CType *type, int yasc)
{
  type->ref = sym_pointer_node_q(type, yasc);
  type->t = VT_PTR | (type->t & VT_STORAGE);
}

/* The same pointer type without yasc qualifiers: what `p + 1` is, a pointer
   no one frees. */
ST_FUNC void type_yasc_strip(CType *type)
{
  if (type_is_handle(type))
    type->ref = NULL; /* fd + 1: an int */
  else if (type_yasc(type))
    type->ref = sym_pointer_node_q(&type->ref->type, 0);
}

/* `p + n`: points into what p points to and owns nothing itself.  What p
   is to the checks stays: a view stays a view (it may be shared), a restrict
   pointer restrict (based on it), a regular-C pointer unchecked; ownership
   and the rest go. */
ST_FUNC void type_yasc_derive(CType *type)
{
  int q = type_yasc(type);
  if (type_is_handle(type))
    type->ref = NULL; /* fd + 1: an int */
  else if (q)
    type->ref = sym_pointer_node_q(&type->ref->type, q & (YASC_VIEW | YASC_RESTRICT | YASC_UNCHECKED));
}

/* The same pointer or int handle with qualifiers YASC, keeping what its
   node says beside them (borrows(...), the deallocator). */
ST_FUNC void yasc_requalify(CType *type, int yasc)
{
  if (type_is_handle(type))
    type->ref = sym_handle_node(yasc, type->ref->vreg > 0 ? type->ref->vreg : 0);
  else
    type->ref = sym_pointer_node_b(&type->ref->type, yasc, type->ref->vreg);
}

/* mk_pointer with a Sym of its own, for a pointer whose target is still to
   be rewritten in place (type_decl). */
ST_FUNC void mk_private_pointer(CType *type)
{
  Sym *s;
  s = sym_push(SYM_FIELD, type, 0, -1);
  type->t = VT_PTR | (type->t & VT_STORAGE);
  type->ref = s;
}

/* return true if type1 and type2 are exactly the same (including
   qualifiers).
*/
int is_compatible_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 0);
}

/* return true if type1 and type2 are the same (ignoring qualifiers).
 */
int is_compatible_unqualified_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 1);
}

void cast_error(CType *st, CType *dt)
{
  type_incompatibility_error(st, dt, "cannot convert '%s' to '%s'");
}

/* gen_assign_cast is converting a return statement's value, an argument */
int yasc_returning, yasc_argument;
int yasc_argument_c; /* yasc_argument: the parameter is regular C's (YASC_PARAM_C) */
int yasc_stmt_bol, yasc_unsafe_expr_line, yasc_rbrace_line;

static BufferedFile *yasc_unsafe_expr_file;
/* the value unsafe(expr) left on the stack, until the next statement: the
   plain-pointer inference (plain_param.c) learns nothing from it */
SValue yasc_unsafe_value;
const SValue *yasc_unsafe_value_at;

/* a statement begins at LINE (block.c): on the line of the last unsafe(expr)
   of this file it is an error; past it the line is forgotten */
__attribute__((noinline)) void yasc_unsafe_line_check(int line)
{
  int same = file == yasc_unsafe_expr_file && line == yasc_unsafe_expr_line;
  yasc_unsafe_expr_line = 0;
  yasc_unsafe_value_at = NULL;
  if (same)
    tcc_error_noabort("unsafe(...) takes its whole line: put this statement on a line of its own");
}

/* The variable vtop is, as a whole -- not a member of it, not something
   reached through it: primary.c leaves the identifier's Sym in vtop->sym,
   which a member access or a dereference keeps while the type changes. */
Sym *yasc_vtop_var(void)
{
  Sym *s = vtop->sym;
  if (!(vtop->r & VT_LVAL) || !s || (s->v & ~SYM_STRUCT) >= SYM_FIRST_ANOM ||
      (((vtop->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || (s->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR) &&
       !(type_is_handle(&vtop->type) && type_is_handle(&s->type))) ||
      s->type.ref != vtop->type.ref)
    return NULL;
  return s;
}

/* yasc: ownership that leaves an owned variable says so with move(p)
   (docs/static_analysis_todo.md, "Explicit move(p) and borrow(p)").  Not
   needed for a `drop` parameter (the deallocator) or for returning a whole
   local or parameter, which ends there; needed for an owned parameter that
   is not drop, an owned variable or member, and a member or global
   returned, which lives on. */
static void yasc_verify_move(int dq, int sq)
{
  Sym *s;
  if (!(dq & YASC_OWNED) || !(sq & YASC_OWNED) || (sq & YASC_FRESH) || ((dq & YASC_DROP) && yasc_argument) ||
      !(vtop->r & VT_LVAL))
    return;
  s = yasc_vtop_var();
  if (s && yasc_returning && (s->r & VT_VALMASK) != VT_CONST)
    return;
  if (s)
  {
    const char *name = get_tok_str(s->v, NULL);
    tcc_warning_once(warn_implicit_move, "ownership of '%s' moves here: write move(%s)", name, name);
    tcc_fixit(0, name, FIXIT_BEFORE, "move(");
    tcc_fixit(0, name, FIXIT_AFTER, ")");
  }
  else
    tcc_warning_once(warn_implicit_move, "ownership moves out of a member or through a pointer here: write move(...)");
}

/* yasc: an owned value given to a regular-C parameter (yasc_verify_assign).
   The C side says nothing about ownership, so the code says it: borrow(p)
   lends it, move(p) hands it over (the flow check follows p as moved where
   the call takes it); a plain owned variable is -Wimplicit-move's. */
static void yasc_owned_to_unchecked(int sq)
{
  Sym *s;
  if (sq & YASC_FRESH)
  {
    if (!(vtop->r & VT_LVAL) && tcc_state->ir && vtop->vr >= 0)
      tcc_ir_lt_note_moved_temp(tcc_state->ir, vtop->vr);
    return;
  }
  if ((s = yasc_vtop_var()))
  {
    const char *name = get_tok_str(s->v, NULL);
    tcc_warning_once(warn_implicit_move,
                     "'%s' is owned and goes to a regular-C parameter: write borrow(%s) or move(%s)", name, name, name);
    tcc_fixit(0, name, FIXIT_BEFORE, "borrow(");
    tcc_fixit(0, name, FIXIT_AFTER, ")");
  }
}

/* yasc unsafe: the region's end (tcc_unsafe_end), the first line after it:
   the line of the token read after it, or the line after its first one when
   that token is on it (`unsafe(f(p));`: the whole line) */
static int yasc_unsafe_end_line(int begin)
{
  int l = file ? file->line_num : 0;
  return l > begin ? l : begin + 1;
}

static const char *yasc_file_now(void)
{
  BufferedFile *f;
  return tcc_diag_file(&f);
}

/* `unsafe { ... }` (block.c, after `unsafe`, at the '{'): the statements
   inside pass strict lifetimes.  Out of line: block() is recursive. */
__attribute__((noinline)) void yasc_unsafe_block(void)
{
  const char *fn = yasc_file_now();
  const int begin = file ? file->line_num : 0;
  /* unsafe works by lines: the region has its lines to itself */
  if (!yasc_stmt_bol)
    tcc_error_noabort("unsafe { } must begin its line: put what comes before it on the line above");
  tcc_unsafe_begin(tcc_state, fn, begin);
  block(0);
  /* it ends with its '}', which must end its line */
  const int rb = yasc_rbrace_line >= begin ? yasc_rbrace_line : begin;
  int end = rb + 1;
  if (file && file->line_num == rb && tok != TOK_EOF)
    tcc_error_noabort("unsafe { } must end its line: put what follows its '}' on the next line");
  tcc_unsafe_end(tcc_state, fn, end);
}

/* `unsafe(expr)`: one expression passes strict lifetimes.  0 when tok
   names a variable or function instead. */
__attribute__((noinline)) int yasc_unsafe_expr(void)
{
  const char *fn;
  int begin;
  if (sym_find(TOK_unsafe))
    return 0;
  next();
  if (tok != '(')
  {
    unget_tok(TOK_unsafe);
    return 0;
  }
  fn = yasc_file_now();
  begin = file ? file->line_num : 0;
  /* unsafe(expr) takes its whole line: its statement begins it */
  if (!yasc_stmt_bol)
    tcc_error_noabort("unsafe(...) takes its whole line: put what comes before its statement on the line above");
  tcc_unsafe_begin(tcc_state, fn, begin);
  next();
  expr_eq();
  skip(')');
  yasc_unsafe_value = *vtop;
  yasc_unsafe_value_at = vtop;
  tcc_unsafe_end(tcc_state, fn, yasc_unsafe_end_line(begin));
  /* ... and no statement follows it there (block.c) */
  yasc_unsafe_expr_line = yasc_unsafe_end_line(begin) - 1;
  yasc_unsafe_expr_file = file;
  return 1;
}

/* yasc: move(p) hands p's ownership on -- an owned result, as a call's is;
   borrow(p) lends it -- a view of the same memory.  Both take an owned
   pointer.  Returns 0 when tok names a variable or function instead. */
int yasc_move_borrow(void)
{
  int kw = tok, q;
  CType t;

  if (sym_find(kw))
    return 0;
  next();
  if (tok != '(')
  {
    unget_tok(kw);
    return 0;
  }
  next();
  expr_eq();
  skip(')');
  q = type_yasc(&vtop->type);
  if (!(q & YASC_OWNED))
    tcc_error("%s of a pointer that owns nothing", get_tok_str(kw, NULL));
  if (vtop->r & VT_LVAL)
    gv(RC_INT);
  t = vtop->type;
  if (kw == TOK_move)
    yasc_requalify(&t, q | YASC_FRESH);
  else
  {
    if (type_is_handle(&t))
      t.ref = sym_handle_node((q & ~(YASC_OWNED | YASC_FRESH | YASC_DROP | YASC_TAKEN)) | YASC_VIEW, 0);
    else
      t.ref = sym_pointer_node_q(&t.ref->type, (q & ~(YASC_OWNED | YASC_FRESH | YASC_DROP | YASC_TAKEN)) | YASC_VIEW);
  }
  vtop->type = t;
  return 1;
}

/* yasc `restrict` (S3): an exclusive borrow is made from what nothing else
   reaches -- an owned pointer, another restrict pointer, a local's address or
   a local array.  A view may be shared (as Rust's &T never becomes &mut T);
   a global's address is reachable by name from anywhere.  -Wlifetime. */
static __attribute__((noinline)) void yasc_verify_restrict(int sq)
{
  if (is_null_pointer(vtop) || ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0) ||
      (sq & (YASC_RESTRICT | YASC_OWNED)))
    return;
  if ((vtop->r & VT_SYM) && !(vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) == VT_CONST)
    tcc_warning_once(warn_lifetime,
                     "a restrict pointer to a global or static: the code can reach it by name too: write unsafe(...)");
  else if (sq & YASC_VIEW)
    tcc_warning_once(warn_lifetime, "a view may be shared: a restrict pointer needs an owned pointer, a local's "
                                    "address or another restrict pointer");
}

/* yasc: what the types alone say about storing vtop in 'dt' -- an
   assignment, an initializer, an argument or a return.  A variable handed
   to an owned pointer moves (checked by the flow analysis); a call's owned
   result that lands in a pointer owning nothing is never freed. */
static void yasc_verify_assign(CType *dt)
{
  int dq = type_yasc(dt), sq = type_yasc(&vtop->type);

  if (type_is_handle(dt) || type_is_handle(&vtop->type))
  {
    /* int handles: a constant (-1, ...) owns nothing and is fine anywhere */
    if ((dq | sq) & YASC_UNCHECKED)
      return;
    /* A handle into a plain int: an int is a number, it points at nothing, so
       where a handle goes says what it is -- `int view fd` borrows it, `int
       owned(close) fd` takes it.  A regular-C parameter cannot say it: an
       owned handle goes there as borrow(fd) or move(fd), a view as it is.
       `cast<as(int)>(fd)` is the number, on purpose: a copy that owns nothing. */
    if (!type_is_handle(dt))
    {
      if (yasc_argument && yasc_argument_c)
      {
        if (sq & YASC_OWNED)
          yasc_owned_to_unchecked(sq);
        return;
      }
      if (!(sq & YASC_FRESH) || (vtop->r & VT_LVAL))
      {
        Sym *v = yasc_vtop_var();
        const char *name = v ? get_tok_str(v->v, NULL) : "...";
        tcc_yasc_rule("a handle is stored in a plain int: where it goes says 'int view' or 'int owned', or "
                      "cast<as(int)>(%s) takes its number", name);
        return;
      }
    }
    if ((dq & YASC_OWNED) && !(sq & YASC_OWNED) && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
      tcc_yasc_rule("an int that owns nothing is stored in an owned handle");
    else if (!(dq & YASC_OWNED) && (sq & YASC_FRESH) && !(vtop->r & VT_LVAL))
      tcc_yasc_rule("an owned result is stored in an int that owns nothing: it is never released");
    yasc_verify_move(dq, sq);
    return;
  }
  if ((dt->t & VT_BTYPE) != VT_PTR || (vtop->type.t & VT_BTYPE) != VT_PTR)
    return;
  /* an owned pointer handed to a regular-C parameter: borrow(p) or move(p) */
  if (yasc_argument && (dq & YASC_UNCHECKED) && !(sq & YASC_UNCHECKED) && (sq & YASC_OWNED))
    yasc_owned_to_unchecked(sq);
  /* move(p) stored in a regular-C place (a member of a context the kernel
     takes): p moves there, as into a regular-C parameter */
  else if (!yasc_argument && (dq & YASC_UNCHECKED) && (sq & YASC_FRESH) && !(vtop->r & VT_LVAL) && tcc_state->ir &&
           vtop->vr >= 0)
    tcc_ir_lt_note_moved_temp(tcc_state->ir, vtop->vr);
  /* a regular-C pointer becomes an owned / view / borrows one: the code
     vouches for it with unsafe(...) (S2; -Wunchecked, an error in the safe
     default, off under -ccompat).  Null (C's NULL too) is fine. */
  if ((sq & YASC_UNCHECKED) && !(dq & YASC_UNCHECKED) && (dq & (YASC_OWNED | YASC_VIEW)) && !is_null_pointer(vtop) &&
      !((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0)) /* `(T *)0` */
    tcc_warning_once(warn_unchecked, "a pointer from regular C becomes %s here: write unsafe(...)",
                     (dq & YASC_OWNED) ? "owned" : type_borrows(dt) ? "a borrow" : "a view");
  /* regular C on either side: it says nothing about ownership, so nothing
     more is checked here -- checks start at the yasc variable it lands in */
  if ((dq | sq) & YASC_UNCHECKED)
    return;
  if (dq & YASC_RESTRICT)
    yasc_verify_restrict(sq);
  if ((dq & YASC_OWNED) && !(sq & YASC_OWNED) && !is_null_pointer(vtop))
  {
    Sym *v = (vtop->r & VT_LVAL) ? yasc_vtop_var() : NULL;
    /* a parameter is a view unless it says owned: free(p) of `char *p` (one
       diagnostic: a note is not replayed with a body parsed again) */
    if (v && (v->yasc & YASC_BODY_PARAM) && (sq & YASC_VIEW) && !type_borrows(&vtop->type))
    {
      const char *n = get_tok_str(v->v, NULL);
      tcc_yasc_rule("the parameter '%s' owns nothing (a view unless it says owned): write '*owned %s' when the "
                    "function takes it over, 'drop' before the type too when it frees it", n, n);
    }
    else
      tcc_yasc_rule("a pointer that owns nothing is stored in an owned pointer");
  }
  else if (!(dq & YASC_OWNED) && (sq & YASC_FRESH) && !(vtop->r & VT_LVAL))
    tcc_yasc_rule("an owned result is stored in a pointer that owns nothing: it is never freed");
  yasc_verify_move(dq, sq);
}

ST_FUNC void yasc_keyword_as_name(int kw)
{
  tcc_error("'%s' is a yasc keyword: rename it, or compile with -ccompat", get_tok_str(kw, NULL));
}

/* yasc: an expression statement that drops an owned result leaks it */
ST_FUNC void yasc_verify_discard(void)
{
  if (type_is_errunion(&vtop->type))
    yasc_errunion_discarded();
  if ((type_yasc(&vtop->type) & YASC_FRESH) && !(vtop->r & VT_LVAL))
    tcc_yasc_rule("%s", type_is_handle(&vtop->type) ? "an owned result is discarded: it is never released"
                                                              : "an owned result is discarded: it is never freed");
}

/* -Waddress-of-packed-member: vtop, converted to the pointer type DT (an
   assignment, an argument, a return, a cast), is the address of a member a
   packed struct places below its type's alignment (or such an array member),
   and DT's target needs more alignment than that place guarantees.  A
   pointer to char takes it as it is, and so does void * (the memcpy idiom)
   -- except an explicit (void *) cast, which passes the address on to the
   conversion after it, as gcc's check looks through it.  A pointer to a
   packed struct converted to an over-aligned one is -Wcast-align's (gcc 15
   no longer reports it here). */
void packed_conversion_check(CType *dt)
{
  CType *st = &vtop->type, *t1;
  int a1;
  char b1[256];

  if ((dt->t & VT_BTYPE) != VT_PTR || (st->t & VT_BTYPE) != VT_PTR)
    goto done;
  t1 = pointed_type(dt);
  if ((t1->t & VT_BTYPE) == VT_VOID || (t1->t & VT_BTYPE) == VT_FUNC || type_size(t1, &a1) < 0)
    goto done;
  if (vtop->packed_addr || ((st->t & VT_ARRAY) && packed_member_last.set && packed_member_is_vtop()))
  {
    PackedMember *pm = vtop->packed_addr ? &packed_member_addr : &packed_member_last;
    if (a1 > pm->align)
    {
      type_to_str(b1, sizeof b1, &pm->st, NULL);
      tcc_warning_once(warn_address_of_packed_member,
                       "taking address of packed member of '%s' may result in an unaligned pointer value", b1);
    }
  }
done:
  vtop->packed_addr = 0; /* checked: the converted value is a pointer like any other */
}

static const char *qualifier_names(int q)
{
  return (q & VT_CONSTANT) && (q & VT_VOLATILE) ? "const volatile" : (q & VT_CONSTANT) ? "const" : "volatile";
}

/* -Wcast-qual: the cast drops const/volatile from what the pointer points to
   (at any level), or adds one below a level that is not const -- `char **`
   to `const char **` lets a `const char *` be stored where a `char *` is
   read. */
static void cast_qual_check(CType *dt, CType *st)
{
  CType *t1 = pointed_type(dt), *t2 = pointed_type(st);
  int all_const = 1;
  char b1[256], b2[256];
  for (int lvl = 0;; lvl++)
  {
    int drop = t2->t & ~t1->t & (VT_CONSTANT | VT_VOLATILE);
    if (drop)
    {
      tcc_warning_once(warn_cast_qual, "cast discards '%s' qualifier from pointer target type", qualifier_names(drop));
      return;
    }
    if (lvl && !all_const && (t1->t & ~t2->t & (VT_CONSTANT | VT_VOLATILE)))
    {
      type_to_str(b1, sizeof b1, st, NULL);
      type_to_str(b2, sizeof b2, dt, NULL);
      tcc_warning_once(warn_cast_qual,
                       "to be safe all intermediate pointers in cast from '%s' to '%s' must be 'const' qualified", b1,
                       b2);
      return;
    }
    if (!(t1->t & VT_CONSTANT))
      all_const = 0;
    if ((t1->t & VT_BTYPE) != VT_PTR || (t2->t & VT_BTYPE) != VT_PTR)
      return;
    t1 = pointed_type(t1);
    t2 = pointed_type(t2);
  }
}

/* What the place of the array vtop is guarantees, past its element type:
   an array variable's _Alignas, an array member's offset in an aligned
   struct.  0 when unknown (a pointer, `buf + n`, an element of an array of
   arrays). */
int cast_source_place_align(CType *st)
{
  const SValue *a = &array_member_last.sv;
  Sym *sym = vtop->sym;
  int align;
  if (!(st->t & VT_ARRAY) || (st->t & VT_VLA) || (vtop->r & VT_VALMASK) == VT_CMP)
    return 0;
  if (array_member_last.set && a->r == vtop->r && a->vr == vtop->vr && a->type.t == vtop->type.t &&
      a->type.ref == vtop->type.ref && a->c.i == vtop->c.i && (!(a->r & VT_SYM) || a->sym == vtop->sym))
    return array_member_last.align;
  /* the array variable itself */
  if (!sym || sym->type.ref != st->ref || !(sym->type.t & VT_ARRAY) || (vtop->r & VT_LVAL))
    return 0;
  if ((vtop->r & VT_VALMASK) == VT_LOCAL ? vtop->c.i != sym->c
                                          : (vtop->r & (VT_VALMASK | VT_SYM)) != (VT_CONST | VT_SYM) || vtop->c.i)
    return 0;
  type_size(&sym->type, &align);
  if (sym->a.aligned && (1 << (sym->a.aligned - 1)) > align)
    align = 1 << (sym->a.aligned - 1);
  return sym->a.packed && !sym->a.aligned ? 1 : align;
}

/* -Wcast-align: the cast raises the alignment the target needs -- from a
   char buffer or a packed struct to a word, which an unaligned address
   faults on (ARMv8-M Baseline on every access, Mainline on LDRD/LDM).  Not
   from void * (as gcc: what it points to is unknown), not to or from an
   incomplete type.  An array whose place is aligned enough (_Alignas, a
   member at an aligned offset) is not reported, as clang does.  The target
   type is named first: the caret goes under it in the cast. */
static void cast_align_check(CType *dt, CType *st)
{
  CType *t1 = pointed_type(dt), *t2 = pointed_type(st);
  int a1, a2, pa, b1 = t1->t & VT_BTYPE, b2 = t2->t & VT_BTYPE;
  char n1[256], n2[256];
  if (b1 == VT_VOID || b1 == VT_FUNC || b2 == VT_VOID || b2 == VT_FUNC)
    return;
  if (type_size(t1, &a1) < 0 || type_size(t2, &a2) < 0)
    return;
  if (a1 <= a2 || (pa = cast_source_place_align(st)) >= a1)
    return;
  {
    CType from = *st;
    from.t &= ~(VT_ARRAY | VT_STORAGE); /* `char [1024]` is `char *` here, `static` is not a type */
    type_to_str(n1, sizeof n1, dt, NULL);
    type_to_str(n2, sizeof n2, &from, NULL);
  }
  tcc_warning_once(warn_cast_align, "cast to '%s' from '%s' increases required alignment from %d to %d", n1, n2,
                   pa > a2 ? pa : a2, a1);
}

/* yasc: `*p`, `p->m`, `p[i]` with p a regular-C (unchecked) pointer: what
   it points to is vouched for with unsafe (S2, -Wunchecked).  Calls through
   a function pointer are not memory. */
__attribute__((noinline)) void yasc_unchecked_deref(void)
{
  if ((pointed_type(&vtop->type)->t & VT_BTYPE) == VT_FUNC)
    return;
  tcc_warning_once(warn_unchecked, "a pointer from regular C is dereferenced: write unsafe { } around it");
}

/* yasc: whether T holds an owned / view / borrows pointer or int handle --
   itself, in a member or in an element (not behind a pointer).  OWNED_ONLY:
   an owned one. */
int yasc_type_holds_annotated(CType *t, int owned_only, int depth)
{
  const int want = owned_only ? YASC_OWNED : (YASC_OWNED | YASC_VIEW);
  int q;
  if (depth > 64)
    return 0;
  if (t->t & VT_ARRAY)
    return t->ref && yasc_type_holds_annotated(&t->ref->type, owned_only, depth + 1);
  q = type_yasc(t);
  if ((q & want) && !(q & YASC_UNCHECKED))
    return 1;
  if ((t->t & VT_BTYPE) == VT_STRUCT && t->ref)
    for (Sym *f = sym_first(t->ref); f; f = f->next)
      if (yasc_type_holds_annotated(&f->type, owned_only, depth + 1))
        return 1;
  return 0;
}

/* yasc (S2, U2): the member (or element) on vtop was reached through a
   union -- `u.s`, `x.u.st.p`, `u.arr[i]` -- so it may have been written as
   another member, and what it holds is not known.  tok is the token after
   it.  An owned / view / borrows pointer (or handle) member read reads as
   regular C: held, compared and passed to C freely, used as owned / view /
   memory only in unsafe -- the -Wunchecked rules.  A store to it (`u.s = p`)
   is checked as its type says.  Its address, or an aggregate member that
   holds such pointers used whole (copied, passed, returned, its address
   taken), would hand the annotated type on unchecked: -Wunchecked too.
   Member and element accesses below an aggregate come here themselves. */
void yasc_union_member_end(int addr_operand)
{
  CType *t = &vtop->type;
  int q = type_yasc(t);
  if ((q & (YASC_OWNED | YASC_VIEW)) && !(q & YASC_UNCHECKED))
  {
    if (addr_operand && tok != '[' && tok != TOK_ARROW)
    {
      if (tcc_warn_wanted(warn_unchecked))
        tcc_warning_once(warn_unchecked,
                         "the address of a union member that holds a yasc pointer is taken: write unsafe(...)");
      return;
    }
    if (tok == '=')
      return;
    if (type_is_handle(t))
      t->ref = NULL; /* an int: the handle's qualifiers are not known either */
    else
      t->ref = sym_pointer_node_q(&t->ref->type, YASC_UNCHECKED);
    return;
  }
  if (tok == '.' || tok == '[' || (tok == '=' && !addr_operand) || nocode_wanted ||
      !yasc_type_holds_annotated(t, 0, 0) || !tcc_warn_wanted(warn_unchecked))
    return;
  tcc_warning_once(warn_unchecked, "a union member that holds yasc pointers is %s: write unsafe(...)",
                   addr_operand ? "addressed" : "used whole");
}

/* yasc: an initializer's null (nullptr, 0) for a pointer that is not '?':
   a member (`struct s y = {.p = nullptr}`; `struct s { char *p; }`'s p is a
   view, never null) or a static pointer.  What flows in later is the
   lifetime check's (a store through `->` / `.`); a member left out of the
   braces is not said. */
__attribute__((noinline)) void yasc_init_null(CType *type, int is_static, int in_aggregate)
{
  const int q = type_yasc(type);
  /* (file scope runs with nocode_wanted set: a static object is data) */
  /* (an inlined body: the function's own said it) */
  if (!(q & (YASC_OWNED | YASC_VIEW)) || (q & (YASC_MAYBE | YASC_UNCHECKED)) || type_is_handle(type) ||
      (nocode_wanted && !is_static) || tcc_state->in_inline_expansion)
    return;
  if (!is_null_pointer(vtop) && !((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0 &&
                                  is_integer_btype(vtop->type.t & VT_BTYPE)))
    return;
  /* `char *a[2] = {nullptr}`: the plain array is the error */
  if (!is_static && in_aggregate && !yasc_init_field && yasc_pp_held_array())
    return;
  if (yasc_init_field && (yasc_init_field->v & ~SYM_FIELD) < SYM_FIRST_ANOM)
  {
    tcc_warning_c(warn_lifetime)("a null pointer initializes '%s', which is not '?'",
                                 get_tok_str(yasc_init_field->v & ~SYM_FIELD, NULL));
    tcc_note("a member's pointer is never null unless it says '?': write '*?' in its declaration");
  }
  else if (in_aggregate)
  {
    tcc_warning_c(warn_lifetime)("a null pointer initializes an element that is not '?'");
    tcc_note("an element's pointer is never null unless it says '?': write '*?' in the element type");
  }
  else
    tcc_warning_c(warn_lifetime)("a null pointer initializes a pointer that is not '?'");
}

/* yasc: an automatic object's initializer value for a member (or array
   element) pointer that is not '?', other than a null constant
   (yasc_init_null's): the lifetime check says whether it may be null
   (tcc_ir_lt_note_init_store).  The member's name, 0 for an element; -1:
   nothing to check. */
__attribute__((noinline)) int yasc_init_flow_tok(CType *type)
{
  const int q = type_yasc(type);
  if (!tcc_state->ir || !tcc_state->ir->lt_on || nocode_wanted || tcc_state->in_inline_expansion ||
      !(q & (YASC_OWNED | YASC_VIEW)) || (q & (YASC_MAYBE | YASC_UNCHECKED)) || type_is_handle(type) ||
      is_null_pointer(vtop) || ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0))
    return -1;
  if (yasc_init_field && (yasc_init_field->v & ~SYM_FIELD) < SYM_FIRST_ANOM)
    return yasc_init_field->v & ~SYM_FIELD;
  return yasc_init_field || yasc_pp_held_array() ? -1 : 0;
}

/* yasc: `a[i] = v` / `*p = v` into an element or a target pointer that is
   not '?' (a view or owned one): the lifetime check says whether V may be
   null (tcc_ir_lt_note_assign_store, infix.c).  A variable and a member
   are followed by the check itself, a global's own pointer too. */
__attribute__((noinline)) int yasc_store_flow_wanted(CType *type)
{
  int q;
  if (!tcc_state->yasc || !tcc_state->ir || !tcc_state->ir->lt_on || nocode_wanted ||
      tcc_state->in_inline_expansion || (type->t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !type->ref)
    return 0;
  q = type_yasc(type);
  return (q & (YASC_OWNED | YASC_VIEW)) && !(q & (YASC_MAYBE | YASC_UNCHECKED)) && !type_is_handle(type);
}

/* yasc: an explicit cast to an owned / view / borrows pointer says what
   the types cannot check, so outside unsafe it is -Wunchecked's (strict):
   from regular C, from an integer (MMIO, flash offsets), dropping maybe,
   making a borrowed pointer owned (or an owned one a view), dropping const. */
static __attribute__((noinline)) void yasc_cast_check(CType *dt)
{
  CType *st = &vtop->type;
  int dq = type_yasc(dt), sq, null = is_null_pointer(vtop) ||
                                      ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0);
  const char *what;
  if (!(dq & (YASC_OWNED | YASC_VIEW)) || (dq & YASC_UNCHECKED) || null || type_is_handle(dt))
    return;
  if ((st->t & VT_BTYPE) != VT_PTR)
  {
    if (is_integer_btype(st->t & VT_BTYPE))
      tcc_warning_once(warn_unchecked, "a cast makes an integer a pointer: write unsafe(...)");
    return;
  }
  sq = type_yasc(st);
  what = (dq & YASC_OWNED) ? "owned" : type_borrows(dt) ? "a borrow" : "a view";
  if (sq & YASC_UNCHECKED)
    tcc_warning_once(warn_unchecked, "a pointer from regular C becomes %s here: write unsafe(...)", what);
  else if ((sq & YASC_MAYBE) && !(dq & YASC_MAYBE))
    tcc_warning_once(warn_unchecked, "a cast drops '?': test the pointer, or write unsafe(...)");
  else if ((dq & YASC_OWNED) && !(sq & YASC_OWNED))
    tcc_warning_once(warn_unchecked, "a cast makes a pointer that owns nothing owned: write unsafe(...)");
  else if (!(dq & YASC_OWNED) && (sq & YASC_OWNED))
    tcc_warning_once(warn_unchecked, "a cast makes an owned pointer a view: write borrow(...)");
  else if ((pointed_type(st)->t & VT_CONSTANT) && !(pointed_type(dt)->t & VT_CONSTANT))
    tcc_warning_once(warn_unchecked, "a cast drops const: write unsafe(...)");
}

/* The checks of an explicit cast of vtop to the pointer type DT. */
void explicit_cast_checks(CType *dt)
{
  CType *st = &vtop->type;
  yasc_null_spelling(vtop); /* (T *)0 */
  if (tcc_state->yasc && tcc_warn_wanted(warn_unchecked))
    yasc_cast_check(dt);
  if ((st->t & VT_BTYPE) == VT_PTR)
  {
    /* yasc: dropping const needs unsafe(...), which says it was meant */
    if (tcc_warn_wanted(warn_cast_qual) && !(tcc_state->yasc && tcc_state->unsafe_depth))
      cast_qual_check(dt, st);
    if (tcc_warn_wanted(warn_cast_align))
      cast_align_check(dt, st);
  }
  if ((pointed_type(dt)->t & VT_BTYPE) == VT_VOID)
    return; /* (void *)&packed->m: checked where it lands */
  if (tcc_warn_wanted(warn_address_of_packed_member))
    packed_conversion_check(dt);
  else
    vtop->packed_addr = 0;
}

/* ---------------------------------------------------------------------
 * Implicit conversions that change a value (assignment, initialization,
 * argument, return): -Woverflow for a constant out of the destination's
 * range, -Wconversion / -Wsign-conversion for the rest, as gcc words them.
 * A compound assignment `c += x` converts its result, an int: what is
 * checked is the operand x against c (expr_eq records it), so `c += 1`
 * stays quiet as in gcc.
 * --------------------------------------------------------------------- */
SValue fe_compound_rhs;
int fe_compound_pending;
int fe_conv_quiet; /* stores the compiler makes itself (a bit-field's unit) */

static void fe_conv_type_str(char *buf, int size, int t)
{
  CType ct;
  ct.t = t & ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE | VT_STRUCT_MASK);
  ct.ref = NULL;
  type_to_str(buf, size, &ct, NULL);
  if (t & VT_BITFIELD)
    snprintf(buf + strlen(buf), size - strlen(buf), ":%d", BIT_SIZE(t));
}

static int fe_float_const(const SValue *sv)
{
  int bt = sv->type.t & VT_BTYPE;
  return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && (bt == VT_FLOAT || bt == VT_DOUBLE) &&
         !(sv->type.t & (VT_COMPLEX | VT_VECTOR));
}

static double fe_float_value(const SValue *sv)
{
  return (sv->type.t & VT_BTYPE) == VT_FLOAT ? sv->c.f : sv->c.d;
}

/* the value bits of an integer type a float mantissa must hold */
static int fe_value_bits(int t)
{
  return fe_int_bits(t) - !fe_int_unsigned(t);
}

static int fe_mantissa_bits(int bt)
{
  return bt == VT_FLOAT ? 24 : 53;
}

/* a conversion of a call argument (gfunc_param_typed): which one, of what */
int fe_arg_index;    /* 1-based; 0 = not an argument */
Sym *fe_arg_callee; /* the called function's symbol, or NULL (a call through a pointer) */

static void fe_arg_where(char *buf, int size)
{
  buf[0] = 0;
  if (!fe_arg_index)
    return;
  if (fe_arg_callee && (fe_arg_callee->type.t & VT_BTYPE) == VT_FUNC)
    snprintf(buf, size, " (argument %d of '%s')", fe_arg_index, get_tok_str(fe_arg_callee->v & ~SYM_FIELD, NULL));
  else
    snprintf(buf, size, " (argument %d)", fe_arg_index);
}

/* every conversion warning names the argument it is about */
#define FE_CONV_WARN(sw, fmt, ...) tcc_fe_warning_c(sw, fmt "%s", __VA_ARGS__, where)

static void fe_check_conversion(CType *dt, const SValue *sv)
{
  TCCState *s1 = tcc_state;
  int dtt = dt->t, stt = sv->type.t, dbt = dtt & VT_BTYPE, sbt = stt & VT_BTYPE;
  int dint = is_integer_btype(dbt) && dbt != VT_BOOL, sint = is_integer_btype(sbt);
  char ds[64], ss[64], where[128];
  if (((dtt | stt) & (VT_COMPLEX | VT_VECTOR)) || (!dint && !is_float(dbt)) || (!sint && !is_float(sbt)))
    return;
  fe_conv_type_str(ds, sizeof ds, dtt);
  fe_conv_type_str(ss, sizeof ss, stt);
  fe_arg_where(where, sizeof where);
  if (dint && fe_int_const(sv))
  {
    int big, n = fe_int_bits(dtt), du = fe_int_unsigned(dtt);
    int64_t v = fe_const_value(sv, &big);
    uint64_t mask = n >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << n) - 1;
    uint64_t nb = (uint64_t)v & mask;
    int64_t nv = du || n >= 64 ? (int64_t)nb : (int64_t)(nb ^ ((uint64_t)1 << (n - 1))) - ((int64_t)1 << (n - 1));
    char from[32], to[32];
    snprintf(from, sizeof from, big ? "%llu" : "%lld", (unsigned long long)v);
    snprintf(to, sizeof to, du ? "%llu" : "%lld", (unsigned long long)nv);
    if (n < 64 && (big || v < -((int64_t)1 << (n - 1)) || v > (int64_t)mask))
    {
      if (s1->warn_overflow)
        FE_CONV_WARN(warn_overflow, "%s conversion from '%s' to '%s' changes value from '%s' to '%s'",
                         du ? "unsigned" : "overflow in", ss, ds, from, to);
    }
    else if (du && v < 0 && !big)
    {
      if (s1->warn_sign_conversion)
        FE_CONV_WARN(warn_sign_conversion, "unsigned conversion from '%s' to '%s' changes value from '%s' to '%s'",
                         ss, ds, from, to);
    }
    else if (!du && (big || (n < 64 && v > (int64_t)(mask >> 1))))
    {
      if (fe_int_unsigned(stt) && fe_int_bits(stt) == n)
      {
        if (s1->warn_sign_conversion)
          FE_CONV_WARN(warn_sign_conversion, "signed conversion from '%s' to '%s' changes value from '%s' to '%s'",
                           ss, ds, from, to);
      }
      else if (s1->warn_conversion)
        FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' changes value from '%s' to '%s'", ss, ds, from,
                         to);
    }
    return;
  }
  if (!s1->warn_conversion && !s1->warn_sign_conversion)
    return;
  if (fe_float_const(sv) || (fe_int_const(sv) && is_float(dbt)))
  {
    /* a constant: only when the value changes */
    int big;
    double v = fe_float_const(sv) ? fe_float_value(sv) : (double)fe_const_value(sv, &big);
    if (fe_int_const(sv) && big)
      v = (double)(uint64_t)fe_const_value(sv, &big);
    if (dint)
    {
      int n = fe_int_bits(dtt), du = fe_int_unsigned(dtt);
      double lo = du ? 0.0 : -ldexp(1.0, n - 1), hi = ldexp(1.0, n - du);
      if (v != (double)(int64_t)v || v < lo || v >= hi)
        FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' changes value from '%.17g' to '%lld'", ss, ds,
                         v, (long long)(v < lo || v >= hi ? 0 : (int64_t)v));
    }
    else if (dbt == VT_FLOAT && (float)v != v)
      FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' changes value from '%.17g' to '%.9g'", ss, ds, v,
                       (double)(float)v);
    return;
  }
  if (fe_int_const(sv))
    return; /* an integer constant to an integer: done above */
  {
    int v = sv->r & VT_VALMASK;
    if (v == VT_CMP || v == VT_JMP || v == VT_JMPI)
      return; /* 0 or 1 */
  }
  if (dint && sint && fe_narrow_fits(sv, dtt))
    return; /* its range fits, as gcc's shortening sees it */
  if (dint && sint)
  {
    int n = fe_int_bits(dtt), du = fe_int_unsigned(dtt);
    int sb = fe_int_bits(stt), su = fe_int_unsigned(stt);
    int narrows = du == su ? sb > n : su ? sb >= n : sb > n;
    if (du == su ? sb <= n : su ? sb < n : 0)
      return; /* every value fits */
    if (!du && su && sb == n)
      narrows = 0; /* same width: only the sign */
    if (narrows)
    {
      if (s1->warn_conversion)
        FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
    }
    else if (s1->warn_sign_conversion)
      FE_CONV_WARN(warn_sign_conversion, "conversion to '%s' from '%s' may change the sign of the result", ds, ss);
    return;
  }
  if (!s1->warn_conversion)
    return;
  if (is_float(sbt) && dint)
    FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
  else if (sint && is_float(dbt) && fe_value_bits(stt) > fe_mantissa_bits(dbt))
    FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
  else if (is_float(sbt) && sbt != VT_FLOAT && dbt == VT_FLOAT) /* double (and long double, also 8 bytes here) to float */
    FE_CONV_WARN(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
}

#undef FE_CONV_WARN

/* verify type compatibility to store vtop in 'dt' type */
/* vstore: the destination is a plain yasc local (yasc_ro_local_ok), and the
   value stored was read-only (yasc_ro_dropped) */
int yasc_ro_local_ok, yasc_ro_dropped;

void verify_assign_cast(CType *dt)
{
  CType *st, *type1, *type2;
  int dbt, sbt, qualwarn, lvl;

  if (fe_compound_pending)
  {
    /* `x op= y`: y against x, not the int result (shifts: never) */
    int shift = fe_compound_pending == 2;
    fe_compound_pending = 0;
    if (!shift && (tcc_state->warn_conversion || tcc_state->warn_sign_conversion) && fe_eval_checks_wanted())
    {
      const SValue *y = &fe_compound_rhs;
      if (!fe_int_const(y))
        fe_check_conversion(dt, y);
      else if (is_integer_btype(dt->t & VT_BTYPE))
      {
        /* a constant that fits: `c += 1` */
        int big, n = fe_int_bits(dt->t);
        int64_t v = fe_const_value(y, &big);
        if (big || (n < 64 && (v < -((int64_t)1 << (n - 1)) || v >= ((int64_t)1 << n))))
          fe_check_conversion(dt, y);
      }
    }
  }
  else if ((tcc_state->warn_overflow || tcc_state->warn_conversion || tcc_state->warn_sign_conversion) &&
           fe_eval_checks_wanted() && !fe_conv_quiet)
    fe_check_conversion(dt, vtop);

  if (tcc_state->yasc)
  {
    yasc_slice_assign(dt);
    yasc_verify_errors_assign(dt);
    yasc_verify_assign(dt);
  }
  st = &vtop->type; /* source type */
  if (dt->t & VT_RODATA_REL)
    rodata_rel_store_error();
  dbt = dt->t & VT_BTYPE;
  sbt = st->t & VT_BTYPE;
  /* yasc: a view parameter's target is read-only unless it says inout or
     out -- writing it anyway is the contract broken, not a style matter */
  if ((dt->t & VT_CONSTANT) && tcc_state->yasc && !pp_in_c())
    tcc_error("assignment of read-only location (a view parameter's target is written only when it says inout or out)");
  else if (dt->t & VT_CONSTANT)
    tcc_warning_once_always("assignment of read-only location");
  switch (dbt)
  {
  case VT_VOID:
    if (sbt != dbt)
      tcc_error("assignment to void expression");
    break;
  case VT_PTR:
    if (tcc_warn_wanted(warn_address_of_packed_member))
      packed_conversion_check(dt);
    /* special cases for pointers */
    /* '0' can also be a pointer -- yasc writes nullptr */
    if (is_null_pointer(vtop))
    {
      yasc_null_spelling(vtop);
      break;
    }
    /* accept implicit pointer to integer cast with warning */
    if (is_integer_btype(sbt))
    {
      tcc_warning_once(warn_int_conversion, "assignment makes pointer from integer without a cast");
      break;
    }
    type1 = pointed_type(dt);
    if (sbt == VT_PTR)
      type2 = pointed_type(st);
    else if (sbt == VT_FUNC)
      type2 = st; /* a function is implicitly a function pointer */
    else
      goto error;
    rodata_rel_check_targets(type1, type2, st, dt);
    if (is_compatible_types(type1, type2))
      break;
    /* a function whose yasc declaration says c_api(...): its C type too */
    if (yasc_capi_fnptr_ok(type1, type2))
      break;
    for (qualwarn = lvl = 0;; ++lvl)
    {
      qualwarn |= type2->t & ~type1->t & (VT_CONSTANT | VT_VOLATILE);
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
        tcc_warning_once(warn_incompatible_pointer_types, "assignment from incompatible pointer type");
        break;
      }
    }
    /* yasc: a plain local takes a read-only value -- what flows into it is
       followed (lifetime.c: written through, handed where it may be written,
       it is an error), as for a value that may be null */
    if (qualwarn == VT_CONSTANT && yasc_ro_local_ok)
    {
      yasc_ro_dropped = 1;
      break;
    }
    if (qualwarn)
    {
      /* name what is lost and where, as clang does: `const char *view` to
         `char *view` drops the target's const, which the bare message hid */
      char b1[256], b2[256];
      CType t1 = *dt, t2 = *st;
      t1.t &= ~VT_STORAGE;
      t2.t &= ~VT_STORAGE;
      type_to_str(b1, sizeof b1, &t1, NULL);
      type_to_str(b2, sizeof b2, &t2, NULL);
      tcc_warning_once(warn_discarded_qualifiers,
                       "assignment to '%s' from '%s' discards '%s' qualifier from pointer target type", b1, b2,
                       qualifier_names(qualwarn));
    }
    break;
  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
    if (sbt == VT_PTR || sbt == VT_FUNC)
    {
      tcc_warning_once(warn_int_conversion, "assignment makes integer from pointer without a cast");
    }
    else if (sbt == VT_STRUCT)
    {
      goto case_VT_STRUCT;
    }
    /* XXX: more tests */
    break;
  case VT_STRUCT:
  case_VT_STRUCT:
    if (is_transparent_union_type(dt) && find_assignable_transparent_union_member(dt))
      break;
    /* Allow reinterpret assignment/cast between GCC vector types of the
     * same total byte size (e.g. v4si <-> v4ui, v8hi <-> v4si). */
    if ((dt->t & VT_VECTOR) && (st->t & VT_BTYPE) == VT_STRUCT && (st->t & VT_VECTOR) && dt->ref->c == st->ref->c)
      break;
    if (!is_compatible_unqualified_types(dt, st))
    {
    error:
      cast_error(st, dt);
    }
    break;
  }
}

void gen_assign_cast(CType *dt)
{
  verify_assign_cast(dt);
  gen_cast(dt);
}
