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
    if ((dq & YASC_OWNED) && !(sq & YASC_OWNED) && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
      tcc_warning_once_always("an int that owns nothing is stored in an owned handle");
    else if (!(dq & YASC_OWNED) && (sq & YASC_FRESH) && !(vtop->r & VT_LVAL))
      tcc_warning_once_always("an owned result is stored in an int that owns nothing: it is never released");
    yasc_verify_move(dq, sq);
    return;
  }
  if ((dt->t & VT_BTYPE) != VT_PTR || (vtop->type.t & VT_BTYPE) != VT_PTR)
    return;
  /* regular C on either side: it says nothing about ownership, so nothing is
     checked here -- checks start at the yasc variable the pointer lands in */
  if ((dq | sq) & YASC_UNCHECKED)
    return;
  if ((dq & YASC_OWNED) && !(sq & YASC_OWNED) && !is_null_pointer(vtop))
    tcc_warning_once_always("a pointer that owns nothing is stored in an owned pointer");
  else if (!(dq & YASC_OWNED) && (sq & YASC_FRESH) && !(vtop->r & VT_LVAL))
    tcc_warning_once_always("an owned result is stored in a pointer that owns nothing: it is never freed");
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
    tcc_warning_once_always("%s", type_is_handle(&vtop->type) ? "an owned result is discarded: it is never released"
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

/* -Wcast-align: the cast raises the alignment the target needs -- from a
   char buffer or a packed struct to a word, which an unaligned address
   faults on (ARMv8-M Baseline on every access, Mainline on LDRD/LDM).  Not
   from void * (as gcc: what it points to is unknown), not to or from an
   incomplete type. */
static void cast_align_check(CType *dt, CType *st)
{
  CType *t1 = pointed_type(dt), *t2 = pointed_type(st);
  int a1, a2, b1 = t1->t & VT_BTYPE, b2 = t2->t & VT_BTYPE;
  if (b1 == VT_VOID || b1 == VT_FUNC || b2 == VT_VOID || b2 == VT_FUNC)
    return;
  if (type_size(t1, &a1) < 0 || type_size(t2, &a2) < 0)
    return;
  if (a1 > a2)
    tcc_warning_once(warn_cast_align, "cast increases required alignment of target type");
}

/* The checks of an explicit cast of vtop to the pointer type DT. */
void explicit_cast_checks(CType *dt)
{
  CType *st = &vtop->type;
  if ((st->t & VT_BTYPE) == VT_PTR)
  {
    if (tcc_warn_wanted(warn_cast_qual))
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

static void fe_check_conversion(CType *dt, const SValue *sv)
{
  TCCState *s1 = tcc_state;
  int dtt = dt->t, stt = sv->type.t, dbt = dtt & VT_BTYPE, sbt = stt & VT_BTYPE;
  int dint = is_integer_btype(dbt) && dbt != VT_BOOL, sint = is_integer_btype(sbt);
  char ds[64], ss[64];
  if (((dtt | stt) & (VT_COMPLEX | VT_VECTOR)) || (!dint && !is_float(dbt)) || (!sint && !is_float(sbt)))
    return;
  fe_conv_type_str(ds, sizeof ds, dtt);
  fe_conv_type_str(ss, sizeof ss, stt);
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
        tcc_fe_warning_c(warn_overflow, "%s conversion from '%s' to '%s' changes value from '%s' to '%s'",
                         du ? "unsigned" : "overflow in", ss, ds, from, to);
    }
    else if (du && v < 0 && !big)
    {
      if (s1->warn_sign_conversion)
        tcc_fe_warning_c(warn_sign_conversion, "unsigned conversion from '%s' to '%s' changes value from '%s' to '%s'",
                         ss, ds, from, to);
    }
    else if (!du && (big || (n < 64 && v > (int64_t)(mask >> 1))))
    {
      if (fe_int_unsigned(stt) && fe_int_bits(stt) == n)
      {
        if (s1->warn_sign_conversion)
          tcc_fe_warning_c(warn_sign_conversion, "signed conversion from '%s' to '%s' changes value from '%s' to '%s'",
                           ss, ds, from, to);
      }
      else if (s1->warn_conversion)
        tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' changes value from '%s' to '%s'", ss, ds, from,
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
        tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' changes value from '%.17g' to '%lld'", ss, ds,
                         v, (long long)(v < lo || v >= hi ? 0 : (int64_t)v));
    }
    else if (dbt == VT_FLOAT && (float)v != v)
      tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' changes value from '%.17g' to '%.9g'", ss, ds, v,
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
        tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
    }
    else if (s1->warn_sign_conversion)
      tcc_fe_warning_c(warn_sign_conversion, "conversion to '%s' from '%s' may change the sign of the result", ds, ss);
    return;
  }
  if (!s1->warn_conversion)
    return;
  if (is_float(sbt) && dint)
    tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
  else if (sint && is_float(dbt) && fe_value_bits(stt) > fe_mantissa_bits(dbt))
    tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
  else if (is_float(sbt) && sbt != VT_FLOAT && dbt == VT_FLOAT) /* double (and long double, also 8 bytes here) to float */
    tcc_fe_warning_c(warn_conversion, "conversion from '%s' to '%s' may change value", ss, ds);
}

/* verify type compatibility to store vtop in 'dt' type */
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
    yasc_verify_errors_assign(dt);
    yasc_verify_assign(dt);
  }
  st = &vtop->type; /* source type */
  if (dt->t & VT_RODATA_REL)
    rodata_rel_store_error();
  dbt = dt->t & VT_BTYPE;
  sbt = st->t & VT_BTYPE;
  if (dt->t & VT_CONSTANT)
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
    /* '0' can also be a pointer */
    if (is_null_pointer(vtop))
      break;
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
        tcc_warning_once(warn_incompatible_pointer_types, "assignment from incompatible pointer type");
        break;
      }
    }
    if (qualwarn)
      tcc_warning_once(warn_discarded_qualifiers, "assignment discards qualifiers from pointer target type");
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
