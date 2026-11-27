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

/* compare.c -- Type comparison, compatibility, transparent unions and type-to-string.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* print a type. If 'varstr' is not NULL, then the variable is also
   printed in the type */
/* XXX: union */
/* XXX: add array and function pointers */
void type_to_str(char *buf, int buf_size, CType *type, const char *varstr)
{
  int bt, v, t;
  Sym *s, *sa;
  char buf1[256];
  const char *tstr;

  t = type->t;
  bt = t & VT_BTYPE;
  buf[0] = '\0';

  /* yasc: `[]T`, `[:0]T` */
  if (MAYBE_ERRUNION(type) && yasc_slice_to_str(buf, buf_size, type, varstr))
    return;
  /* yasc: `!T`, `error NAME`, `error` */
  if (MAYBE_ERRUNION(type) && type_is_errunion(type))
  {
    pstrcpy(buf, buf_size, "!");
    type_to_str(buf + 1, buf_size - 1, &type->ref->type, varstr);
    return;
  }
  if (type_is_errset(type))
  {
    pstrcpy(buf, buf_size, yasc_errset_name(type));
    if (varstr)
    {
      pstrcat(buf, buf_size, " ");
      pstrcat(buf, buf_size, varstr);
    }
    return;
  }

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
      sa = sym_first(s);
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
      if (t & VT_RODATA_REL)
        pstrcat(buf1, buf_size, "__rodata_relative ");
      /* yasc: as written, `char *?owned(fclose)`, `T *view`, `T *@(const, view)` */
      if ((s->yasc & (YASC_OWNED | YASC_VIEW | YASC_MAYBE)) && !(s->yasc & YASC_UNCHECKED))
      {
        char words[200];
        int nw = 0;
        words[0] = 0;
#define YW_ADD(w)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if (nw++)                                                                                                          \
      pstrcat(words, sizeof(words), ", ");                                                                             \
    pstrcat(words, sizeof(words), (w));                                                                                \
  } while (0)
        if (t & VT_CONSTANT)
          YW_ADD("const");
        if (t & VT_VOLATILE)
          YW_ADD("volatile");
        if (t & VT_RODATA_REL)
          YW_ADD("__rodata_relative");
        if (s->yasc & YASC_OWNED)
        {
          char w[80];
          if (s->vreg > 0 && s->vreg < YASC_OBJ_MARK)
            snprintf(w, sizeof w, "owned(%s)", get_tok_str(s->vreg, NULL));
          else
            pstrcpy(w, sizeof w, "owned");
          YW_ADD(w);
        }
        else if ((s->yasc & YASC_VIEW) && s->vreg == YASC_BORROWS_STATIC)
          YW_ADD("borrows(static)");
        else if ((s->yasc & YASC_VIEW) && s->vreg <= -2 && yasc_field_borrows(-2 - s->vreg))
        {
          /* a member's `borrows(buf)` */
          YascFieldBorrows *e = yasc_field_borrows(-2 - s->vreg);
          char w[160];
          pstrcpy(w, sizeof w, "borrows(");
          for (int k = 0; k < e->nsrc; k++)
          {
            if (k)
              pstrcat(w, sizeof w, ", ");
            pstrcat(w, sizeof w, get_tok_str(e->src_tok[k], NULL));
          }
          if (e->is_static)
            pstrcat(w, sizeof w, e->nsrc ? ", static" : "static");
          pstrcat(w, sizeof w, ")");
          YW_ADD(w);
        }
        else if (s->yasc & YASC_VIEW)
          YW_ADD("view"); /* or borrows(params): their names are not here */
#undef YW_ADD
        pstrcpy(buf1, sizeof(buf1), (s->yasc & YASC_MAYBE) ? "*?" : "*");
        if (nw > 1)
          pstrcat(buf1, sizeof(buf1), "@(");
        pstrcat(buf1, sizeof(buf1), words);
        if (nw > 1)
          pstrcat(buf1, sizeof(buf1), ")");
        if (varstr)
          pstrcat(buf1, sizeof(buf1), " ");
      }
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

void type_incompatibility_error(CType *st, CType *dt, const char *fmt)
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
  tcc_warning_once_always(fmt, buf1, buf2);
}

int pointed_size(CType *type)
{
  int align;
  return type_size(pointed_type(type), &align);
}

int is_null_pointer(SValue *p)
{
  if ((p->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
    return 0;
  return ((p->type.t & VT_BTYPE) == VT_INT && (uint32_t)p->c.i == 0) ||
         ((p->type.t & VT_BTYPE) == VT_LLONG && p->c.i == 0) ||
         ((p->type.t & VT_BTYPE) == VT_PTR && (PTR_SIZE == 4 ? (uint32_t)p->c.i == 0 : p->c.i == 0) &&
          ((pointed_type(&p->type)->t & VT_BTYPE) == VT_VOID) &&
          0 == (pointed_type(&p->type)->t & (VT_CONSTANT | VT_VOLATILE)));
}

/* yasc writes the null pointer one way, nullptr: an integer constant zero
   (`0`, `'\0'`, `1 - 1`) used as one -- stored, passed, returned, compared
   or cast to a pointer -- says so (-Wzero-as-null-pointer-constant, on in
   the safe default).  NULL is the preprocessor's (macro_subst_tok); in a
   regular-C macro's expansion and under -ccompat nothing is said. */
void yasc_null_spelling(SValue *p)
{
  if (tcc_state->yasc && (p->type.t & VT_BTYPE) != VT_PTR && tcc_warn_wanted(warn_zero_as_null_pointer_constant) &&
      is_null_pointer(p))
    tcc_warning_once(warn_zero_as_null_pointer_constant, "0 as a null pointer: write nullptr");
}

/* the same for the operands A and B of a comparison or a ?: the source
   wrote (gen_op_checked, expr_cond_ternary): the one beside a pointer --
   not `!p`, which tcc makes `p == 0` */
void yasc_null_pair(SValue *a, SValue *b)
{
  if ((a->type.t & VT_BTYPE) == VT_PTR)
    yasc_null_spelling(b);
  else if ((b->type.t & VT_BTYPE) == VT_PTR)
    yasc_null_spelling(a);
}

/* compare function types. OLD functions match any new functions */
static int is_compatible_func(CType *type1, CType *type2)
{
  Sym *s1, *s2;

  s1 = type1->ref;
  s2 = type2->ref;
  sym_first(s1); /* the walk below steps from the type Syms to their parameters */
  sym_first(s2);
  if (s1->f.func_call != s2->f.func_call)
    return 0;
  if (s1->f.func_type != s2->f.func_type && s1->f.func_type != FUNC_OLD && s2->f.func_type != FUNC_OLD)
    return 0;
  for (;;)
  {
    if (s1->a.transparent_union && s1->type.ref)
      s1->type.ref->a.transparent_union = 1;
    if (s2->a.transparent_union && s2->type.ref)
      s2->type.ref->a.transparent_union = 1;
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

int is_transparent_union_type(CType *type)
{
  return (type->t & VT_BTYPE) == VT_STRUCT && type->ref && type->ref->a.transparent_union &&
         type->ref->type.t == VT_UNION;
}

static CType *find_transparent_union_compatible_member(CType *type, CType *other, int unqualified)
{
  Sym *field;

  if (!is_transparent_union_type(type))
    return NULL;

  for (field = sym_first(type->ref); field; field = field->next)
  {
    if ((unqualified && compare_types(&field->type, other, 1)) ||
        (!unqualified && is_compatible_types(&field->type, other)))
      return &field->type;
  }

  return NULL;
}

static int is_assign_compatible_pointer_types(CType *dt, CType *st)
{
  CType *type1, *type2;
  int dbt, sbt, lvl;

  dbt = dt->t & VT_BTYPE;
  sbt = st->t & VT_BTYPE;
  if (dbt != VT_PTR)
    return 0;

  type1 = pointed_type(dt);
  if (sbt == VT_PTR)
    type2 = pointed_type(st);
  else if (sbt == VT_FUNC)
    type2 = st;
  else
    return 0;

  if (is_compatible_types(type1, type2))
    return 1;

  for (lvl = 0;; ++lvl)
  {
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
      return 1;
    if (dbt == sbt && is_integer_btype(sbt & VT_BTYPE) &&
        IS_ENUM(type1->t) + IS_ENUM(type2->t) + !!((type1->t ^ type2->t) & VT_UNSIGNED) < 2)
      return 1;
    return 0;
  }

  return 1;
}

CType *find_assignable_transparent_union_member(CType *type)
{
  Sym *field;

  if (!is_transparent_union_type(type))
    return NULL;

  for (field = sym_first(type->ref); field; field = field->next)
  {
    int fbt = field->type.t & VT_BTYPE;

    if (is_compatible_unqualified_types(&field->type, &vtop->type))
      return &field->type;
    if (fbt == VT_PTR && (is_null_pointer(vtop) || is_assign_compatible_pointer_types(&field->type, &vtop->type)))
      return &field->type;
  }

  return NULL;
}

/* Structural type comparison for typedef redefinition checking.
   Unlike compare_types(), this compares struct/union types by their
   layout (size, field offsets, field types) rather than by Sym* identity.
   This is needed because PCH replay can create new Sym* instances for
   structurally identical types. */
int compare_types_structural(CType *type1, CType *type2)
{
  int bt1, t1, t2;

  t1 = type1->t & VT_TYPE;
  t2 = type2->t & VT_TYPE;

  if ((t1 & VT_BTYPE) != VT_BYTE)
  {
    t1 &= ~VT_DEFSIGN;
    t2 &= ~VT_DEFSIGN;
  }

  if (t1 != t2)
    return 0;

  if ((t1 & VT_ARRAY) && !(type1->ref->c < 0 || type2->ref->c < 0 || type1->ref->c == type2->ref->c))
    return 0;

  bt1 = t1 & VT_BTYPE;
  if (bt1 == VT_PTR)
  {
    type1 = pointed_type(type1);
    type2 = pointed_type(type2);
    return compare_types_structural(type1, type2);
  }
  else if (bt1 == VT_STRUCT)
  {
    Sym *s1 = type1->ref, *s2 = type2->ref;
    Sym *f1, *f2;
    if (s1 == s2)
      return 1;
    if (s1->c != s2->c)
      return 0;
    for (f1 = sym_first(s1), f2 = sym_first(s2); f1 && f2; f1 = f1->next, f2 = f2->next)
    {
      if (f1->c != f2->c)
        return 0;
      if (!compare_types_structural(&f1->type, &f2->type))
        return 0;
    }
    return !f1 && !f2;
  }
  else if (bt1 == VT_FUNC)
  {
    return is_compatible_func(type1, type2);
  }
  return 1;
}

/* return true if type1 and type2 are the same.  If unqualified is
   true, qualifiers on the types are ignored.
 */
int compare_types(CType *type1, CType *type2, int unqualified)
{
  int bt1, t1, t2;

  /* yasc error unions: the same payload (their nodes are interned) */
  if ((MAYBE_ERRUNION(type1) || MAYBE_ERRUNION(type2)) && (type_is_errunion(type1) || type_is_errunion(type2)))
    return type1->ref == type2->ref;
  /* yasc slices: the same element type and sentinel (interned too) */
  if ((MAYBE_ERRUNION(type1) || MAYBE_ERRUNION(type2)) && (type_is_slice(type1) || type_is_slice(type2)))
    return type1->ref == type2->ref;

  if (IS_ENUM(type1->t))
  {
    if (IS_ENUM(type2->t))
      return type1->ref == type2->ref;
    type1 = &type1->ref->type;
  }
  else if (IS_ENUM(type2->t))
    type2 = &type2->ref->type;

  if (find_transparent_union_compatible_member(type1, type2, unqualified) ||
      find_transparent_union_compatible_member(type2, type1, unqualified))
    return 1;

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
    if (type1->ref == type2->ref)
      return 1;
    /* Two vector types with different Sym*: compare structurally.
       (t1 already verified equal to t2, so both have VT_VECTOR.) */
    if (t1 & VT_VECTOR)
      return type1->ref->c == type2->ref->c && compare_types(&type1->ref->type, &type2->ref->type, unqualified);
    return 0;
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
int combine_types(CType *dest, SValue *op1, SValue *op2, int op)
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
        tcc_warning_once(warn_int_conversion, "pointer/integer mismatch in %s", op == '?' ? "conditional expression" : "comparison");
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
    /* Propagate VT_COMPLEX for integer complex types */
    if ((t1 & VT_COMPLEX) || (t2 & VT_COMPLEX))
      type.t |= VT_COMPLEX;
  }
  else
  {
    /* integer operations */
    type.t = VT_INT | (VT_LONG & (t1 | t2));
    /* convert to unsigned if it does not fit in an integer */
    if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) ||
        (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED))
      type.t |= VT_UNSIGNED;
    /* Propagate VT_COMPLEX for integer complex types */
    if ((t1 & VT_COMPLEX) || (t2 & VT_COMPLEX))
      type.t |= VT_COMPLEX;
  }
  if (dest)
    *dest = type;
  return ret;
}
