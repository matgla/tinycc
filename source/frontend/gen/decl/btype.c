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
/* Qualify `type`; for an array type that means its (innermost) element type,
 * as C 6.7.3p9 says, on fresh copies of the array type syms so the shared
 * typedef or struct-member type is left alone. */
void parse_btype_qualify(CType *type, int qualifiers)
{
  while (type->t & VT_ARRAY)
  {
    type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
    type = &type->ref->type;
  }
  type->t |= qualifiers;
}

/* Where a type is read but no initializer follows -- a parameter, a member,
   a type name -- an `auto` that would take the initializer's type is an
   error. */
void btype_no_auto(AttributeDef *ad)
{
  if (ad->auto_tok)
    tcc_error("'%s' takes the type of an initializer: it cannot be used here", get_tok_str(ad->auto_tok, NULL));
}

/* return 0 if no type declaration. otherwise, return the basic type
   and skip it.
 */
static int parse_btype_body(CType *type, AttributeDef *ad, int ignore_label)
{
  int t, u, bt, st, type_found, typespec_found, g, n;
  Sym *s;
  CType type1;

  memset(ad, 0, sizeof(AttributeDef));
  yasc_capi_attr_forget(ad);
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
      yasc_sbind_pending = 0;
      struct_decl(&type1, VT_STRUCT);
      if (yasc_sbind_pending)
      {
        if (!yasc_struct_params(type1.ref))
          tcc_error("borrows(...): 'struct %s' does not borrow", get_tok_str(type1.ref->v & ~SYM_STRUCT, NULL));
        ad->sbind_set = 1;
        ad->sbind_static = yasc_sbind_static;
        ad->sbind_nb = yasc_sbind_nb;
        memcpy(ad->sbind_names, yasc_sbind_names, yasc_sbind_nb * sizeof(int));
        yasc_sbind_pending = 0;
      }
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
    case TOK_RODATA_REL:
      tcc_error("'__rodata_relative' qualifies a pointer: write it after the '*'");
      break;
    case TOK_AUTO:
      /* C23: with no type specifier, the type of the initializer; with one
         (and before C23) the storage class, which changes nothing */
      if (tcc_state->cversion > 201710 || tcc_state->yasc)
        ad->auto_tok = TOK_AUTO;
      next();
      break;
    case TOK_AUTO_TYPE:
      ad->auto_tok = TOK_AUTO_TYPE;
      next();
      break;
    case TOK_REGISTER:
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
      if (typespec_found)
        goto the_end;
      /* yasc: `[]T`, `[:0]T`, a slice; the declarator wraps it (type_decl).
         `[[` is a C23 attribute, anything else after '[' not a type */
      if (tcc_state->yasc && tok == '[')
      {
        AttributeDef ad1;
        int sentinel = 0;
        next();
        if (tok == ':')
        {
          next();
          if (tok != TOK_CINT || tokc.i != 0)
            expect("0: [:0]T is a slice ending in a zero");
          next();
          sentinel = 1;
          if (tok != ']')
            expect("']' of [:0]");
        }
        if (tok != ']')
        {
          unget_tok('[');
          goto the_end;
        }
        next();
        if (!parse_btype(&type1, &ad1, 0))
          expect("a slice's element type after []");
        if (type1.t & VT_STORAGE)
          tcc_error("a storage class after []");
        if (ad1.yslice) /* `[][]T`: the inner one now */
          yasc_slice_wrap(&type1, ad1.yslice - 1);
        ad->yslice = 1 + sentinel;
        type->t = type1.t;
        type->ref = type1.ref;
        t = type->t;
        typespec_found = 1;
        st = bt = -2;
        break;
      }

      goto the_end;
      /* GNUC typeof */
    case TOK_TYPEOF1:
    case TOK_TYPEOF2:
    case TOK_TYPEOF3:
      next();
      parse_expr_type(&type1);
      /* remove all storage modifiers except typedef */
      type1.t &= ~(VT_STORAGE & ~VT_TYPEDEF);
      if ((type1.t & VT_BTYPE) == VT_FUNC && type1.ref)
        type1.ref->a.proto_own = 0; /* another declaration may take this function's type now */
      if (type1.ref)
        sym_to_attr(ad, type1.ref);
      goto basic_type2;
    case TOK_THREAD_LOCAL:
      tcc_error("_Thread_local is not implemented");
    default:
      if (tok_is_ident(tok, "__thread", 8))
      {
        next();
        break;
      }

      if (tcc_state->yasc && tok >= TOK_owned && tok <= TOK_drop)
      {
        n = tok;
        /* `int owned(close) maybe fd`, `int view fd`: an int handle */
        if ((tok == TOK_owned || tok == TOK_maybe || tok == TOK_view) && typespec_found &&
            (t & VT_BTYPE) == VT_INT && !IS_ENUM(t))
        {
          next();
          /* `f(int owned)`, `f(int view, ...)`: a handle parameter with no name */
          if (YASC_NAME_FOLLOWS(tok) && !(func_param_decl_depth > 0 && (tok == ')' || tok == ',')))
            yasc_keyword_as_name(n);
          ad->yhandle |= n == TOK_owned ? YASC_OWNED : n == TOK_view ? YASC_VIEW : YASC_MAYBE;
          if ((ad->yhandle & (YASC_OWNED | YASC_VIEW)) == (YASC_OWNED | YASC_VIEW))
            tcc_error("a handle is owned or a view, not both");
          if (n == TOK_owned && tok == '(')
          {
            next();
            if (tok < TOK_UIDENT)
              expect("the deallocator's name");
            ad->yhandle_dealloc = tok;
            next();
            skip(')');
          }
          break;
        }
        /* `regex_t owned(regfree) re`: the object is owned as a whole */
        if (tok == TOK_owned && typespec_found && (t & VT_BTYPE) == VT_STRUCT && !(t & VT_ARRAY))
        {
          next();
          if (tok != '(')
            tcc_error("an owned object names its deallocator: owned(release)");
          next();
          if (tok < TOK_UIDENT)
            expect("the deallocator's name");
          ad->yobj_dealloc = tok;
          next();
          skip(')');
          break;
        }
        if (tok == TOK_owned || tok == TOK_maybe || tok == TOK_view)
        {
          next();
          if (typespec_found && YASC_NAME_FOLLOWS(tok))
            yasc_keyword_as_name(n);
          tcc_error("'%s' qualifies a pointer: write it after the '*'", get_tok_str(n, NULL));
        }
        {
          int w = tok == TOK_out ? YASC_OUT : tok == TOK_inout ? YASC_INOUT : YASC_DROP;
          if ((w & (YASC_OUT | YASC_INOUT)) && (ad->yasc & (YASC_OUT | YASC_INOUT | YASC_IN)))
            tcc_error("a parameter is in, inout or out: one of them");
          ad->yasc |= w;
        }
        next();
        if (typespec_found && YASC_NAME_FOLLOWS(tok))
          yasc_keyword_as_name(n);
        break;
      }

      /* yasc `in`, the default: only where a parameter's declaration starts
         (a name everywhere else, `int in` included) */
      if (tcc_state->yasc && tok == TOK_in && !typespec_found && func_param_decl_depth > 0 && !pp_in_c())
      {
        if (ad->yasc & (YASC_OUT | YASC_INOUT))
          tcc_error("a parameter is in, inout or out: one of them");
        ad->yasc |= YASC_IN;
        next();
        break;
      }

      if (typespec_found)
        goto the_end;

      /* yasc: `error ...` (an error set, or any error), unless a variable
       * or function named error is in scope, and not `error.NAME` */
      if (tcc_state->yasc && tok == TOK_ERROR && !sym_find(TOK_ERROR))
      {
        next();
        if (tok == '.')
        {
          unget_tok(TOK_ERROR);
          goto the_end;
        }
        yasc_error_type(&type1);
        goto basic_type2;
      }
      /* yasc: `!T`, an error union; the declarator wraps it (type_decl) */
      if (tcc_state->yasc && tok == '!')
      {
        AttributeDef ad1;
        next();
        if (!parse_btype(&type1, &ad1, 0))
        {
          unget_tok('!');
          goto the_end;
        }
        if (type1.t & VT_STORAGE)
          tcc_error("a storage class after '!'");
        ad->yasc |= YASC_ERRUNION;
        /* the storage class before the '!' stays: `static !int f(void)` */
        type->t = type1.t | (t & VT_STORAGE);
        type->ref = type1.ref;
        t = type->t;
        typespec_found = 1;
        st = bt = -2;
        break;
      }

      if ((tcc_state->cversion > 201710 || tcc_state->yasc) && tok_is_ident(tok, "bool", 4))
      {
        u = VT_BOOL;
        next();
        typespec_found = 1;
        break;
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
      /* yasc: `T.method(...)`, a struct's method through its typedef
         (decl/method.c), starts an expression */
      if (tok == '.' && tcc_state->yasc && (s->type.t & VT_BTYPE) == VT_STRUCT &&
          (s->type.ref->ymod & YASC_HAS_METHODS))
      {
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
      if (s->a.attr_facts)
        attr_deprecated_use(s);
      s->ymod |= SYM_UNUSED_REF | SYM_UNUSED_READ; /* -Wunused-local-typedefs */
      if (s->a.transparent_union && type->ref)
        type->ref->a.transparent_union = 1;
      typespec_found = 1;
      st = bt = -2;
      break;
    }
    type_found = 1;
  }
the_end:
  if (ad->auto_tok && typespec_found)
  {
    if (ad->auto_tok == TOK_AUTO_TYPE)
      tcc_error("'__auto_type' with a type: the type comes from the initializer");
    ad->auto_tok = 0; /* `auto int x`: the storage class */
  }
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
  if (ad->yhandle)
  {
    if (!(ad->yhandle & (YASC_OWNED | YASC_VIEW)))
      tcc_error("a handle says owned or view");
    type->ref = sym_handle_node(ad->yhandle | (ad->yasc & (YASC_OUT | YASC_DROP)), ad->yhandle_dealloc);
  }

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

/* The deprecated typedefs and tags the specifiers name are reported at their
 * end, unless the specifiers say deprecated too (attr_btype_end) */
int parse_btype(CType *type, AttributeDef *ad, int ignore_label)
{
  int mark = attr_btype_begin();
  int found = parse_btype_body(type, ad, ignore_label);
  attr_btype_end(mark, ad);
  return found;
}
