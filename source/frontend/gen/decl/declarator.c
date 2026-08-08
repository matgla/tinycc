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

/* declarator.c -- Declarator parsing: post_type, type_decl and asm labels.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* convert a function parameter type (array to pointer and function to
   function pointer) */
void convert_parameter_type(CType *pt)
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
int asm_label_instr(void)
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

int post_type(CType *type, AttributeDef *ad, int storage, int td)
{
  int n, l, t1, arg_size, align;
  int param_volatile;
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
    if (TYPE_DIRECT == (td & (TYPE_DIRECT | TYPE_ABSTRACT)) && tok != TOK_DOTS)
      return 0;
    if (tok == ')')
      l = 0;
    else if (tok == TOK_DOTS)
    {
      /* C23: f(...) — variadic function with no named parameters */
      l = FUNC_ELLIPSIS;
      next();
    }
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
    if (l && l != FUNC_ELLIPSIS)
    {
      func_param_decl_depth++;
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
        param_volatile = (pt.t & VT_VOLATILE) != 0;
        convert_parameter_type(&pt);
        arg_size += (type_size(&pt, &align) + PTR_SIZE - 1) / PTR_SIZE;
        /* these symbols may be evaluated for VLArrays (see below, under
           nocode_wanted) which is why we push them here as normal symbols
           temporarily.  Example: int func(int a, int b[++a]); */
        s = sym_push(n, &pt, VT_LOCAL | VT_LVAL, 0);
        s->a.param_volatile = param_volatile;
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
      func_param_decl_depth--;
    }
    else if (l != FUNC_ELLIPSIS)
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
          vla_array_str = tok_str_ensure_heap(vla_array_tok);
          vla_array_str_on_heap = 1;
          begin_macro(vla_array_tok, 2);
          next();
          gexpr();
          end_macro();
          next();
          goto check;
        }
        break;
      }
    else if (func_param_decl_depth && tok != ']')
    {
      /* GNU C accepts variably modified types declared within function
         parameter scope, including array members inside parameter-local
         struct definitions.  As with parameter VLAs, defer evaluation to
         function entry by saving the bound expression tokens now. */
      nocode_wanted = 1;
      skip_or_save_block(&vla_array_tok);
      unget_tok(0);
      vla_array_str = tok_str_ensure_heap(vla_array_tok);
      vla_array_str_on_heap = 1;
      begin_macro(vla_array_tok, 2);
      next();
      gexpr();
      end_macro();
      next();
      goto check;
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
      if ((t1 & VT_VLA) && ((td & TYPE_NEST) || (func_param_decl_depth && !(td & TYPE_PARAM))))
      {
        s->vla_array_str = vla_array_str;
        /* Track for end-of-TU reclamation.  func_vla_arg_code frees this at a
           function definition's entry (and drops it from the list there), but
           an inner VLA dimension inside an abstract / function-pointer
           declarator is never materialized and would otherwise leak. */
        if (vla_array_str_on_heap)
        {
          int vi = tcc_state->nb_vla_inner_exprs++;
          tcc_state->vla_inner_exprs = tcc_realloc(tcc_state->vla_inner_exprs,
                                                   tcc_state->nb_vla_inner_exprs * sizeof(*tcc_state->vla_inner_exprs));
          tcc_state->vla_inner_exprs[vi] = vla_array_str;
        }
      }
      else if ((t1 & VT_VLA) && (td & TYPE_PARAM))
      {
        /* Outermost VLA dimension of a function param: save the token string
           separately in TCCState. We can't use s->vla_array_str because it's
           in a union with s->next, and sym_copy_ref would follow it as a
           Sym pointer, causing corruption. */
        int i = tcc_state->nb_vla_param_exprs++;
        tcc_state->vla_param_exprs = tcc_realloc(tcc_state->vla_param_exprs,
                                                 tcc_state->nb_vla_param_exprs * sizeof(*tcc_state->vla_param_exprs));
        tcc_state->vla_param_exprs[i].param = s;
        tcc_state->vla_param_exprs[i].tokens = vla_array_str;
      }
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
CType *type_decl(CType *type, AttributeDef *ad, int *v, int td)
{
  CType *post, *ret;
  int qualifiers, storage;

  /* recursive type, remove storage bits first, apply them later again */
  storage = type->t & VT_STORAGE;
  type->t &= ~VT_STORAGE;
  post = ret = type;

  /* Attributes may prefix a declarator inside a declaration list, e.g.
     'int a, __attribute__((unused)) b;'.  Consume them before looking for
     pointer or direct-declarator syntax. */
  parse_decl_attributes(ad);

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
