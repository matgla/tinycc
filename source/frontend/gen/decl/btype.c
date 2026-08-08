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

/* btype.c -- Declaration-specifier (base type) parsing.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

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
int parse_btype(CType *type, AttributeDef *ad, int ignore_label)
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
    case TOK_COMPLEX_GCC2:
      /* DONE: Phase 1 - Mark that we saw _Complex, will combine with float/double */
      if (t & VT_COMPLEX)
        tcc_error("duplicate _Complex specifier");
      t |= VT_COMPLEX;
      typespec_found = 1;
      next();
      break;
    case TOK_DECIMAL32:
      tcc_warning_c(warn_all)("_Decimal32 is approximated by binary float");
      u = VT_FLOAT;
      goto basic_type;
    case TOK_DECIMAL64:
      tcc_warning_c(warn_all)("_Decimal64 is approximated by binary double");
      u = VT_DOUBLE;
      goto basic_type;
    case TOK_DECIMAL128:
      tcc_warning_c(warn_all)("_Decimal128 is approximated by binary long double");
      u = VT_LDOUBLE;
      goto basic_type;
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
    case '[':
      /* C23 [[ ... ]] standard attribute */
      if (parse_c23_attribute(ad))
        continue;
      goto the_end;
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
      if (tok >= TOK_IDENT)
      {
        const char *tok_str = get_tok_str(tok, NULL);
        if (tok_str && tok_str[0] == '_' && strcmp(tok_str, "__thread") == 0)
        {
          next();
          break;
        }
      }

      if (typespec_found)
        goto the_end;

      if (tok >= TOK_IDENT && tcc_state->cversion > 201710)
      {
        const char *tok_str = get_tok_str(tok, NULL);
        if (tok_str && tok_str[0] == 'b' && strcmp(tok_str, "bool") == 0)
        {
          u = VT_BOOL;
          next();
          typespec_found = 1;
          break;
        }
      }

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
      if (s->a.transparent_union && type->ref)
        type->ref->a.transparent_union = 1;
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

  /* Apply __attribute__((vector_size(N))) if present.
   * Wrap the just-parsed base type into a vector type.
   * Guard against re-application when a vector typedef is looked up (in that
   * case the type is already VT_STRUCT|VT_VECTOR and ad->vector_size would be
   * 0 anyway since sym_to_attr doesn't copy it, but be defensive). */
  if (ad->vector_size && !(type->t & VT_VECTOR))
  {
    int storage = t & VT_STORAGE; /* remember VT_TYPEDEF / VT_EXTERN etc. */
    CType elem = {t & ~VT_STORAGE, type->ref};
    make_vector_type(type, &elem, ad->vector_size);
    type->t |= storage; /* make_vector_type overwrites type->t; restore flags */
  }

  return type_found;
}
