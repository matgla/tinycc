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

Sym *declarator_last_functype; /* the function type Sym post_type built last (decl's own-ref test) */

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
  int param_volatile, empty_proto = 0;
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
      /* C23 and yasc: `()` is `(void)`, a prototype; before C23 it says
         nothing about the parameters */
      l = (empty_proto = tcc_state->cversion >= 202311 || tcc_state->yasc) ? FUNC_NEW : 0;
    else if (tok == TOK_DOTS)
    {
      /* C23: f(...) — variadic function with no named parameters */
      l = FUNC_ELLIPSIS;
      next();
    }
    else if ((func_param_decl_depth++, n = parse_btype(&pt, &ad1, 0), func_param_decl_depth--, n))
    {
      btype_no_auto(&ad1);
      l = FUNC_NEW;
    }
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
    int param_idx = 0; /* yasc_pp_name */
    ++local_scope;
    if (l && l != FUNC_ELLIPSIS && !empty_proto)
    {
      func_param_decl_depth++;
      for (;;)
      {
        const int param_line = file->line_num; /* -Wunused-parameter */
        /* read param name and compute offset */
        if (l != FUNC_OLD)
        {
          if ((pt.t & VT_BTYPE) == VT_VOID && tok == ')')
            break;
          const int pp_mark = yasc_pp_mark();
          type_decl(&pt, &ad1, &n, TYPE_DIRECT | TYPE_ABSTRACT | TYPE_PARAM);
          if (yasc_pp_mark() != pp_mark)
            yasc_pp_name(pp_mark, n, param_idx);
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
        int param_array = (pt.t & VT_ARRAY) != 0;
        convert_parameter_type(&pt);
        arg_size += (type_size(&pt, &align) + PTR_SIZE - 1) / PTR_SIZE;
        /* these symbols may be evaluated for VLArrays (see below, under
           nocode_wanted) which is why we push them here as normal symbols
           temporarily.  Example: int func(int a, int b[++a]); */
        s = sym_push(n, &pt, VT_LOCAL | VT_LVAL, 0);
        s->a.param_volatile = param_volatile;
        s->a.param_array = param_array;
        if (tcc_state->unused_on)
        {
          if (l != FUNC_OLD && ad1.attr_unused)
            s->ymod |= SYM_UNUSED_OK;
          if (!local_stack && func_param_decl_depth == 1)
            unused_param_line(n, param_line, plast == &first);
        }
        else if (tcc_warn_wanted(warn_shadow) && !local_stack && func_param_decl_depth == 1)
          unused_param_line(n, param_line, plast == &first); /* -Wshadow's line */
        *plast = s;
        plast = &s->next;
        param_idx++;
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
        btype_no_auto(&ad1);
      }
      func_param_decl_depth--;
    }
    else if (l != FUNC_ELLIPSIS && l != FUNC_NEW)
      /* if no parameters, then old type prototype */
      l = FUNC_OLD;
    skip(')');
    /* -Wstrict-prototypes: `()` before C23.  A definition is checked where
       it is known to be one (decl()), against the declarations before it. */
    if (l == FUNC_OLD && !first && tok != '{' && tcc_warn_wanted(warn_strict_prototypes))
      tcc_warning_once(warn_strict_prototypes, "function declaration isn't a prototype");
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
    s->a.proto_own = 0;
    declarator_last_functype = s;
    s->f = ad->f;
    s->next = first;
    type->t = VT_FUNC;
    type->ref = s;
    if (first)
      sym_params_track(s, first->prev);
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
        if (tcc_state->sanitize & SAN_VLA)
          ubsan_vla_bound();
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
    /* An element whose size is only known at run time (a struct with a VLA
     * member, GNU C) makes the array variable-length too: its fixed-size
     * frame slot would be overrun by the second element. */
    if (local_stack && !(storage & VT_STATIC) && struct_has_vla_member(type))
      t1 |= VT_VLA;

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
        fe_conv_quiet++; /* the compiler's own store of the size */
        vstore();
        fe_conv_quiet--;
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
/* yasc forbids a plain pointer: every '*' says owned or view.  A pointer to a
   function is exempt (it can only borrow).  A pointer in a nested declarator
   (`(*fp)(int)`) gets its target from the suffix the caller applies after
   it, so it waits here until then (yasc_plain_check). */
#define YASC_PLAIN_MAX 16
static Sym *yasc_plain[YASC_PLAIN_MAX];
static int yasc_plain_line[YASC_PLAIN_MAX];
static int yasc_plain_nb;

/* An error, but the declaration parses on: the pointer is then `view maybe`,
   what a plain pointer was before yasc forbade it.  PARAM 1: a parameter's
   own pointer -- a function's own parameter waits for its body, which decides
   owned or view (decl/plain_param.c); one of a nested declarator is said
   now, with the view fix-it.  PARAM 2: a pointer written in a declarator's
   own chain (not abstract, nested or a parameter's), which a local's waits
   for its body as a parameter's does. */
static void yasc_plain_error(int line, int param)
{
  if (param == 1 && func_param_decl_depth == 1 && yasc_pp_hold(line))
    return;
  /* a local's own pointer waits for its body, which decides too (in an
     inline expansion: the function's own body says it) */
  if (param == 2 && yasc_pp_local_decl && !func_param_decl_depth &&
      (tcc_state->in_inline_expansion || yasc_pp_hold_local(line)))
    return;
  if (param == 1)
    tcc_fixit_next(line, "*", FIXIT_AFTER, " view ",
                   "a parameter the function does not free only borrows: write '* view' ('* owned' when it frees it)");
  tcc_error_noabort("%i:plain pointer in yasc: say owned or view", line);
}

/* error recovery: no pointer waits for its target any more */
void yasc_plain_reset(void)
{
  yasc_plain_nb = 0;
}

/* the waiting pointers from FROM on now know their target */
static void yasc_plain_check(int from)
{
  const int nb = yasc_plain_nb;
  yasc_plain_nb = from;
  for (int i = from; i < nb; i++)
    if ((yasc_plain[i]->type.t & VT_BTYPE) != VT_FUNC)
    {
      yasc_plain_error(yasc_plain_line[i], 0);
    }
}

#define YASC_BORROWS_MAX 28

/* a struct member's borrows(...) names, for struct_decl (yasc_borrows_field_ok) */
int yasc_borrows_field_ok, yasc_field_bnb, yasc_field_bstatic, yasc_field_bnames[YASC_BORROWS_MAX];

/* yasc `T * borrows(a, b, static)` after a '*': the names (tokens) and
   static.  Returns the YASC_BORROWS_STATIC bit; the names go in NAMES. */
static int yasc_parse_borrows(int *names, int *nb)
{
  int st = 0;
  next();
  skip('(');
  for (;;)
  {
    if (tok == TOK_STATIC)
      st = YASC_BORROWS_STATIC;
    else if (tok >= TOK_UIDENT)
    {
      if (*nb == YASC_BORROWS_MAX)
        tcc_error("borrows(...) names too many sources");
      names[(*nb)++] = tok;
    }
    else
      expect("a parameter's name or 'static'");
    next();
    if (tok != ',')
      break;
    next();
  }
  skip(')');
  return st;
}

/* yasc: where function F's result points, when it is a view -- what
   borrows(...) names (NB names, or -1 for none written, and STATIC), else
   the elision: every view parameter; a view result with none has no
   source to point into. */
static void yasc_result_sources(Sym *f, const int *names, int nb, int st)
{
  int q = type_yasc(&f->type), code = st, i;
  Sym *pa;
  if (!(q & YASC_VIEW) || (q & YASC_UNCHECKED) || (f->type.t & VT_ARRAY))
    return;
  for (i = 0; i < nb; i++)
  {
    int k = 0;
    for (pa = sym_first(f); pa && (pa->v & ~SYM_FIELD) != names[i]; pa = pa->next)
      k++;
    if (!pa)
      tcc_error("borrows(%s): no parameter is called '%s'", get_tok_str(names[i], NULL), get_tok_str(names[i], NULL));
    if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR)
      tcc_error("borrows(%s): '%s' is not a pointer", get_tok_str(names[i], NULL), get_tok_str(names[i], NULL));
    if (type_yasc(&pa->type) & YASC_OWNED)
      tcc_error("borrows(%s): an owned parameter is consumed, it is never a source", get_tok_str(names[i], NULL));
    if (k < YASC_BORROWS_MAX)
      code |= 1 << k;
  }
  if (nb < 0 && !st)
  {
    for (i = 0, pa = sym_first(f); pa; pa = pa->next, i++)
    {
      int pq = type_yasc(&pa->type);
      if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (pq & YASC_VIEW) && !(pq & YASC_UNCHECKED) &&
          i < YASC_BORROWS_MAX)
        code |= 1 << i;
    }
    if (!code)
      tcc_error("a view result needs a source: write borrows(static), or take a view parameter");
  }
  f->type.ref = sym_pointer_node_b(&f->type.ref->type, q, code);
}

/* F's result is a borrowing struct: what it borrows -- `struct T
   borrows(src)` in AD, else the elision (every view parameter) */
static void yasc_struct_result(Sym *f, AttributeDef *ad, int name)
{
  int code = 0, i;
  Sym *pa;
  if (!yasc_borrowing_struct(&f->type))
    return;
  if (ad->sbind_set)
  {
    code = ad->sbind_static ? YASC_BORROWS_STATIC : 0;
    for (int k = 0; k < ad->sbind_nb; k++)
    {
      for (i = 0, pa = sym_first(f); pa && (pa->v & ~SYM_FIELD) != ad->sbind_names[k]; pa = pa->next)
        i++;
      if (!pa)
        tcc_error("borrows(%s): no parameter is called '%s'", get_tok_str(ad->sbind_names[k], NULL),
                  get_tok_str(ad->sbind_names[k], NULL));
      if (type_yasc(&pa->type) & YASC_OWNED)
        tcc_error("borrows(%s): an owned parameter is consumed, it is never a source",
                  get_tok_str(ad->sbind_names[k], NULL));
      if (i < YASC_BORROWS_MAX)
        code |= 1 << i;
    }
  }
  else
  {
    for (i = 0, pa = sym_first(f); pa; pa = pa->next, i++)
      if ((type_yasc(&pa->type) & YASC_VIEW) && !(type_yasc(&pa->type) & YASC_UNCHECKED) && i < YASC_BORROWS_MAX)
        code |= 1 << i;
    if (!code)
      tcc_error("a 'struct %s' result borrows: write borrows(static), or take a view parameter",
                get_tok_str(f->type.ref->v & ~SYM_STRUCT, NULL));
  }
  if (name)
    yasc_struct_result_bind(name, code);
}

/* takes(NAME) on F's result: the owned parameter it takes over */
static void yasc_result_takes(Sym *f, int name)
{
  Sym *pa;
  if (!(type_yasc(&f->type) & YASC_OWNED))
    tcc_error("takes(%s): the result must be owned", get_tok_str(name, NULL));
  for (pa = sym_first(f); pa && (pa->v & ~SYM_FIELD) != name; pa = pa->next)
    ;
  if (!pa)
    tcc_error("takes(%s): no parameter is called '%s'", get_tok_str(name, NULL), get_tok_str(name, NULL));
  if (!(type_yasc(&pa->type) & YASC_OWNED) || (type_yasc(&pa->type) & YASC_DROP))
    tcc_error("takes(%s): '%s' must be an owned parameter that is not drop", get_tok_str(name, NULL),
              get_tok_str(name, NULL));
  yasc_requalify(&pa->type, pa->type.ref->yasc | YASC_TAKEN); /* a pointer or an int handle */
}

CType *type_decl(CType *type, AttributeDef *ad, int *v, int td)
{
  CType *post, *ret;
  int qualifiers, storage;
  int plain_from = yasc_plain_nb, nested = 0;
  /* borrows(...) on the outermost pointer: resolved once the parameters
     are known (yasc_result_sources) */
  int bnames[YASC_BORROWS_MAX], bnb = -1, bstatic = 0;
  int dealloc = 0; /* owned(fclose): the outermost pointer's deallocator */
  int takes = 0;   /* takes(p) on the outermost pointer: the parameter's name */
  /* -fstack-usage: where the declared name is (published at the end, after
     the parameters' own declarators) */
  int su_tok = 0, su_line = 0, su_col = 0;

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
    /* yasc: out / drop before the type qualify the first pointer's target */
    int yq = ret == type ? ad->yasc & (YASC_OUT | YASC_DROP) : 0;
    /* the '*' is in the dialect it was written in: `PTR(char) p` with a
       regular-C PTR is a regular-C pointer, though the tokens after it are
       yasc again (the marker after the expansion applies with the next one) */
    const int star_yasc = tcc_state->yasc, star_c = pp_in_c();
    if (bnb >= 0 || bstatic)
      tcc_error("borrows(...) qualifies the outermost pointer");
    qualifiers = 0;
  redo:
    next();
    if (tcc_state->yasc && tok == TOK_takes)
    {
      next();
      skip('(');
      if (tok < TOK_UIDENT)
        expect("a parameter's name");
      takes = tok;
      next();
      skip(')');
      unget_tok(0);
      goto redo;
    }
    if (tcc_state->yasc && tok == TOK_borrows)
    {
      if (yq & (YASC_OWNED | YASC_VIEW))
        tcc_error("a pointer is owned, a view or borrows(...): one of them");
      bnb = 0;
      bstatic = yasc_parse_borrows(bnames, &bnb);
      yq |= YASC_VIEW;
      unget_tok(0);
      goto redo;
    }
    if (tcc_state->yasc && tok >= TOK_owned && tok <= TOK_drop)
    {
      /* owned / view / maybe qualify this pointer; in yasc they are never
         names, so `f(char * view)` and `(char * owned)x` are abstract */
      int kw = tok;
      if (kw != TOK_owned && kw != TOK_maybe && kw != TOK_view)
        tcc_error("'%s' qualifies the object: write it before the type", get_tok_str(kw, NULL));
      yq |= kw == TOK_owned ? YASC_OWNED : kw == TOK_view ? YASC_VIEW : YASC_MAYBE;
      if (kw == TOK_owned)
      {
        /* owned(fclose): the deallocator; `owned (*fp)(...)` is a declarator */
        next();
        if (tok == '(')
        {
          next();
          if (tok >= TOK_UIDENT)
          {
            dealloc = tok;
            next();
            if (tok != ')')
              expect("')' after the deallocator's name");
            next();
          }
          else
            unget_tok('(');
        }
        unget_tok(0); /* redo's next() reads the token after */
      }
      if ((yq & (YASC_OWNED | YASC_VIEW)) == (YASC_OWNED | YASC_VIEW) || ((bnb >= 0 || bstatic) && kw != TOK_maybe))
        tcc_error("a pointer is owned, a view or borrows(...): one of them");
      goto redo;
    }
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
    case TOK_RODATA_REL:
      qualifiers |= rodata_rel_qualifier();
      goto redo;
    /* XXX: clarify attribute handling */
    case TOK_ATTRIBUTE1:
    case TOK_ATTRIBUTE2:
      parse_attribute(ad);
      break;
    }
    int plain = star_yasc && !(yq & (YASC_OWNED | YASC_VIEW));
    /* yasc: a pointer written in regular C says nothing about ownership
       (unless yasc code after it does: `PTR(char) owned p`) */
    if (star_c && !(yq & (YASC_OWNED | YASC_VIEW)))
      yq |= YASC_UNCHECKED;
    if (ret == type && (td & TYPE_NESTDECL))
    {
      /* the caller applies its suffix to this pointer's target, in place */
      mk_private_pointer(type);
      type->ref->yasc = yq;
      if (dealloc)
        type->ref->vreg = dealloc, dealloc = 0;
      if (plain)
      {
        if (yasc_plain_nb == YASC_PLAIN_MAX)
          yasc_plain_check(0);
        yasc_plain_line[yasc_plain_nb] = file ? file->line_num : 0;
        yasc_plain[yasc_plain_nb++] = type->ref;
      }
    }
    else
    {
      if (plain && (type->t & VT_BTYPE) != VT_FUNC)
        yasc_plain_error(file ? file->line_num : 0,
                         (td & TYPE_PARAM) ? 1 : !(td & (TYPE_ABSTRACT | TYPE_NESTDECL)) ? 2 : 0);
      mk_pointer_yasc(type, yq);
      if (dealloc)
        type->ref = sym_pointer_node_b(&type->ref->type, yq, dealloc), dealloc = 0;
      /* `regex_t owned(regfree) * view p`: the first pointer points at an
         object owned as a whole */
      else if (ad->yobj_dealloc && ret == type && (type->ref->type.t & VT_BTYPE) == VT_STRUCT && !(yq & YASC_OWNED))
        type->ref = sym_pointer_node_b(&type->ref->type, yq, YASC_OBJ_MARK | ad->yobj_dealloc);
    }
    type->t |= qualifiers;
    if (ret == type)
      /* innermost pointed to type is the one for the first derivation */
      ret = pointed_type(type);
  }
  /* yasc `!T`: the type before the first function or array derivation is
   * the union's payload -- `!char *f(void)` returns `!(char *)` */
  if ((ad->yasc & YASC_ERRUNION) && !(td & TYPE_NESTDECL))
    yasc_errunion_wrap(type);

  if (tok == '(')
  {
    /* This is possibly a parameter type list for abstract declarators
       ('int ()'), use post_type for testing this.  */
    if (!post_type(type, ad, 0, td & ~TYPE_NESTDECL))
    {
      /* It's not, so it's a nested declarator, and the post operations
         apply to the innermost pointed to type (if any).  */
      /* XXX: this is not correct to modify 'ad' at this point, but
         the syntax is not clear */
      parse_attribute(ad);
      post = type_decl(type, ad, v, td | TYPE_NESTDECL);
      skip(')');
      nested = 1;
      su_tok = tcc_state->su_name_tok, su_line = tcc_state->su_name_line, su_col = tcc_state->su_name_col;
    }
    else
      goto abstract;
  }
  else if (tok >= TOK_IDENT && (td & TYPE_DIRECT))
  {
    /* type identifier */
    *v = tok;
    if (TCC_SU_WANTED(tcc_state))
      su_tok = tok, su_line = file ? file->line_num : 0, su_col = tcc_stack_usage_name_column(tok);
    /* -Wshadow: a name a system header's macro wrote is none of its business */
    if (((tcc_state->warn_shadow & WARN_ON) || tcc_state->nb_diag_snaps) && !(td & TYPE_PARAM))
      shadow_name_sys = tcc_sys_macro_here();
    /* decl()'s name: declared even if the rest of the declarator is wrong */
    const int recover_name = recover_decl.l && !recover_decl.v && !(td & TYPE_PARAM);
    if (recover_name)
    {
      recover_decl.v = tok;
      recover_decl.type = *type;
      recover_decl.scope = local_scope;
    }
    next();
    if (recover_name)
      recover_decl.func = tok == '(' ? 1 : tok == '[' ? 2 : 0;
    /* C23 `int f [[deprecated]] (void);`: the attributes of the declared name */
    if (tok == '[')
      parse_c23_attribute(ad);
  }
  else
  {
  abstract:
    if (!(td & TYPE_ABSTRACT))
    {
      if (tcc_state->yasc && tok >= TOK_owned && tok <= TOK_drop)
        yasc_keyword_as_name(tok);
      expect("identifier");
    }
    *v = 0;
  }
  post_type(post, ad, post != ret ? 0 : storage, td & ~(TYPE_DIRECT | TYPE_ABSTRACT | TYPE_NESTDECL));
  if (takes && !(tcc_state->yasc && (post->t & VT_BTYPE) == VT_FUNC && post->ref))
    tcc_error("takes(%s): only a function's result takes a parameter over", get_tok_str(takes, NULL));
  if (tcc_state->yasc && (post->t & VT_BTYPE) == VT_FUNC && post->ref)
  {
    yasc_result_sources(post->ref, bnames, bnb, bstatic);
    if (takes)
      yasc_result_takes(post->ref, takes);
    yasc_struct_result(post->ref, ad, (td & TYPE_DIRECT) && !nested ? *v : 0);
  }
  else if (bnb > 0 && yasc_borrows_field_ok && !nested && (type->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
  {
    /* a struct member: struct_decl resolves the names once every member is in */
    memcpy(yasc_field_bnames, bnames, bnb * sizeof(int));
    yasc_field_bnb = bnb;
    yasc_field_bstatic = bstatic;
  }
  else if (bnb > 0)
    tcc_error("borrows(%s): only a function's result or a struct member names its sources",
              get_tok_str(bnames[0], NULL));
  else if (bstatic && (type->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && type->ref)
    type->ref = sym_pointer_node_b(&type->ref->type, type->ref->yasc, YASC_BORROWS_STATIC);
  /* the suffix is on: the nested declarator's pointers know their target */
  if (nested && yasc_plain_nb > plain_from)
    yasc_plain_check(plain_from);
  parse_attribute(ad);
  type->t |= storage;
  if (su_tok)
    tcc_state->su_name_tok = su_tok, tcc_state->su_name_line = su_line, tcc_state->su_name_col = su_col;
  return ret;
}
