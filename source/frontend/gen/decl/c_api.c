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

/* c_api.c -- yasc `c_api(T)`: the type C callers see in place of a yasc one
 * (docs/yasc_c_api.md).
 *
 *   const char * borrows(static) maybe c_api(char *) dlerror(void);
 *   int execv(const char *path, const char * const c_api(char * const) argv[]);
 *   long strtol(const char *s, out const char * borrows(s) c_api(char *) * maybe endptr, int base);
 *
 * c_api(T) follows a '*' and stands for everything to its left in that
 * declarator: the base type and the '*'s before it.  What is to its right --
 * more '*'s, the name, [ ], the parameter list -- is the yasc declaration's.
 * The body and yasc callers keep the yasc type; the C header written from
 * the yasc one (-yasc-c-header, driver/yasc_cheader.c) has T there, and a
 * regular-C declaration of the same function in a yasc unit is compared with
 * the C type.  T may differ from the yasc type only in const and volatile at
 * some pointer level: the ABI is the same, so no code is generated for it.
 *
 * In regular C the written header carries yasc_c_api(n, T) (n 0: the result,
 * else parameter n) with the yasc type stripped of its words, for the
 * -Wyasc-c-api checks of C callers (yasc_capi_attr_apply).
 */

#include "gen_priv.h"

int yasc_capi_c_seen;

/* pointer (and array) levels of TYPE, down to what is not a pointer */
int yasc_capi_levels(CType *type)
{
  int n = 0;
  while ((type->t & VT_BTYPE) == VT_PTR && type->ref)
    n++, type = &type->ref->type;
  return n;
}

/* `c_api(T)` at tok: T, in regular C (its pointers say nothing) */
static void capi_parse(CType *c)
{
  const int saved = yasc_cast_type_plain;
  next();
  skip('(');
  /* a plain pointer is what T is made of: counted, never an error */
  yasc_cast_type_plain = 0;
  parse_type(c);
  yasc_cast_type_plain = saved;
  skip(')');
  c->t &= ~VT_STORAGE;
}

/* The c_api(...) of the declarator being read, by func_param_decl_depth: 0
   a declaration's own (a result, a variable, a member), 1 a parameter's.
   Kept here, not in type_decl's or post_type's frame: they recurse, and the
   parser's stack budget allows them no growth (test_parse_stack_depth.py). */
static struct
{
  unsigned char star; /* after the star-th '*' of the declarator (0: none) */
  signed char lv;     /* the levels of the type it ends, once known */
  CType c;
} capi_g[2];
static int capi_nparams; /* the parameters of the list being read that said it */
int yasc_capi_pending;   /* any of the above */

static void capi_update(void)
{
  yasc_capi_pending = capi_g[0].star || capi_g[1].star || capi_nparams;
}

/* type_decl: `c_api` after the NSTAR-th '*' */
void yasc_capi_word(int nstar)
{
  const int d = func_param_decl_depth;
  if (d > 1)
    tcc_error("c_api(...) in a nested parameter list is not supported");
  if (capi_g[d].star)
    tcc_error("c_api(...) twice in one declarator");
  capi_parse(&capi_g[d].c);
  capi_g[d].star = (unsigned char)nstar;
  capi_g[d].lv = -1;
  capi_update();
}

/* type_decl: `c_api` where a declarator's name goes, in regular C.  C reads
   `const char * c_api(char *) name` as a function `c_api` and then fails on
   something else ("invalid type for 'c_api'", "';' expected"); say what is
   wrong instead, and read past `c_api(...)` so the declarator goes on with
   its name.  Off the parser's recursive path: its frame stays out of
   type_decl's. */
__attribute__((noinline)) void yasc_capi_in_c(void)
{
  int depth = 0;
  const int word = tok;
  next();
  const int is_call = tok == '(';
  unget_tok(word);
  if (!is_call)
    return; /* a name `c_api` that is only a name */
  if (file && tcc_yasc_file(tcc_state, file->filename))
    tcc_error_noabort("'c_api(...)' is a yasc word, and this is regular C (a '#pragma tcc c_begin' section): "
                      "put the declaration after '#pragma tcc c_end'");
  else
    tcc_error_noabort("'c_api(...)' is a yasc word, and this is regular C (this file does not say '#pragma yasc'): "
                      "a C declaration writes the C type alone");
  next(); /* c_api */
  do
  {
    if (tok == '(')
      depth++;
    else if (tok == ')')
      depth--;
    next();
  } while (depth > 0 && tok != TOK_EOF);
}

/* type_decl: the NSTAR-th '*' is complete, TYPE what it made */
void yasc_capi_star(int nstar, CType *type)
{
  const int d = func_param_decl_depth;
  if (d <= 1 && capi_g[d].star == nstar && capi_g[d].lv < 0)
    capi_g[d].lv = (signed char)yasc_capi_levels(type);
}

/* Why C type C cannot stand for yasc type Y, LEVEL pointer levels below
   the declared object (0: the object itself, whose own qualifiers are
   nobody's business); NULL when it can.  TO_C: the value goes from yasc to
   C (a function's result), where C may also say const where yasc does not. */
static const char *capi_mismatch(CType *y, CType *c, int level, int to_c)
{
  for (;; level++)
  {
    const int yb = y->t & VT_BTYPE, cb = c->t & VT_BTYPE;
    if (level > 0 && !to_c && (c->t & ~y->t & (VT_CONSTANT | VT_VOLATILE)))
      return "it is const for C callers, and the yasc declaration may write it";
    if (yb != VT_PTR || cb != VT_PTR || !y->ref || !c->ref)
      break;
    if ((y->t ^ c->t) & VT_ARRAY)
      return "an array and a pointer";
    if ((y->t & VT_ARRAY) && y->ref->c != c->ref->c)
      return "arrays of different sizes";
    y = &y->ref->type, c = &c->ref->type;
  }
  if (((y->t & VT_BTYPE) == VT_PTR) != ((c->t & VT_BTYPE) == VT_PTR))
    return "a pointer and something that is not one";
  if (type_is_slice(y))
    return "a yasc slice has no C form";
  if (!is_compatible_unqualified_types(y, c))
    return "different types (c_api(...) may differ in const and volatile only)";
  return NULL;
}

/* SLOT with its part DEPTH levels down replaced by C */
static void capi_rebuild(CType *out, CType *slot, int depth, CType *c)
{
  CType inner;
  if (!depth)
  {
    *out = *c;
    out->t &= ~VT_STORAGE;
    return;
  }
  capi_rebuild(&inner, &slot->ref->type, depth - 1, c);
  if (slot->t & VT_ARRAY)
  {
    Sym *s = sym_push2(local_stack ? &local_stack : &global_stack, SYM_FIELD, inner.t, slot->ref->c);
    s->type.ref = inner.ref;
    out->t = VT_PTR | VT_ARRAY | (slot->t & (VT_CONSTANT | VT_VOLATILE));
    out->ref = s;
  }
  else
  {
    *out = inner;
    mk_pointer(out);
    out->t |= slot->t & (VT_CONSTANT | VT_VOLATILE);
  }
}

/* The declared SLOT (a result, a parameter, a member) had c_api(C) after
   the '*' that made a type of LV levels: checked, and its C type in FULL. */
static void capi_slot(CType *slot, int lv, CType *c, int to_c, CType *full)
{
  int depth = yasc_capi_levels(slot) - lv, i;
  CType *y = slot;
  const char *why;
  if (depth < 0 || (slot->t & VT_VLA))
    tcc_error("c_api(...) here is not supported");
  for (i = 0; i < depth; i++)
    y = &y->ref->type;
  why = capi_mismatch(y, c, depth, to_c);
  if (why)
  {
    char yb[256], cb[256];
    CType yt = *y;
    yt.t &= ~VT_STORAGE;
    type_to_str(yb, sizeof yb, &yt, NULL);
    type_to_str(cb, sizeof cb, c, NULL);
    tcc_error("c_api(%s) cannot stand for '%s': %s", cb, yb, why);
  }
  capi_rebuild(full, slot, depth, c);
}

/* the facts of function type F, which are keyed by it: never parked
   (sym_park_proto rebuilds a prototype's type at another address) */
static SymLocalFacts *capi_facts(Sym *f)
{
  f->a.attr_facts = 1;
  return sym_facts(f);
}

Sym *yasc_capi_of(Sym *fref)
{
  const SymLocalFacts *f = fref ? sym_facts_peek(fref) : NULL;
  return f ? f->c_api_ref : NULL;
}

static Sym *yasc_api_of(Sym *fref)
{
  const SymLocalFacts *f = fref ? sym_facts_peek(fref) : NULL;
  return f ? f->yasc_api_ref : NULL;
}

/* the parameters' C types for capi_functype (on[i]: parameter i has one) */
typedef struct CapiList
{
  CType *t;
  unsigned char *on;
  int n;
} CapiList;

/* A copy of function type F with result RES (NULL: F's) and the parameters
   L replaces (the others F's) */
static Sym *capi_functype(Sym *f, CType *res, CapiList *l)
{
  Sym **ps = local_stack ? &local_stack : &global_stack;
  Sym *s, *p, *first = NULL, **plast = &first;
  int i = 0;
  for (p = sym_first(f); p; p = p->next, i++)
  {
    CType *t = l && i < l->n && l->on[i] ? &l->t[i] : &p->type;
    Sym *q = sym_push2(ps, p->v | SYM_FIELD, t->t, 0);
    q->type.ref = t->ref;
    q->a = p->a;
    q->a.attr_facts = 0;
    *plast = q;
    plast = &q->next;
  }
  s = sym_push2(ps, SYM_FIELD, res ? res->t : f->type.t, f->c);
  s->type.ref = res ? res->ref : f->type.ref;
  s->a = f->a;
  s->a.proto_own = 0;
  s->a.attr_facts = 0;
  s->f = f->f;
  s->next = first;
  return s;
}

/* post_type: parameter S is in (its type final: in made its target const) */
void yasc_capi_param(Sym *s)
{
  CType full, c;
  int lv;
  Sym *h;
  if (func_param_decl_depth != 1 || !capi_g[1].star)
    return;
  c = capi_g[1].c, lv = capi_g[1].lv;
  capi_g[1].star = 0;
  capi_update();
  capi_slot(&s->type, lv, &c, 0, &full);
  /* the parameter's C type, as a type Sym's, until its function is complete */
  h = sym_push2(local_stack ? &local_stack : &global_stack, SYM_FIELD, full.t, 0);
  h->type.ref = full.ref;
  capi_facts(s)->c_api_ref = h;
  capi_nparams++;
  capi_update();
}

/* post_type: function type F is complete; its parameters' C types */
void yasc_capi_function(Sym *f)
{
  CapiList l;
  Sym *p;
  int i, np = 0;
  if (func_param_decl_depth != 0 || !capi_nparams)
    return;
  capi_nparams = 0;
  capi_update();
  for (p = sym_first(f); p; p = p->next)
    np++;
  l.n = np;
  l.t = tcc_mallocz((np ? np : 1) * sizeof *l.t);
  l.on = tcc_mallocz(np ? np : 1);
  for (p = sym_first(f), i = 0; p; p = p->next, i++)
  {
    Sym *h = yasc_capi_of(p);
    if (h)
      l.t[i] = h->type, l.on[i] = 1;
  }
  capi_facts(f)->c_api_ref = capi_functype(f, NULL, &l);
  tcc_free(l.t);
  tcc_free(l.on);
}

/* type_decl, at its end: the declarator of TYPE (TD: type_decl's) is read */
void yasc_capi_declarator(CType *type, int td, int nested)
{
  const int d = func_param_decl_depth;
  CType full, c;
  int lv;
  if (d > 1 || !capi_g[d].star)
    return;
  /* a parameter: post_type checks it once its type is final (yasc_capi_param) */
  if ((td & TYPE_PARAM) && !nested && !(td & TYPE_NESTDECL))
    return;
  c = capi_g[d].c, lv = capi_g[d].lv;
  capi_g[d].star = 0;
  capi_update();
  if (nested || (td & TYPE_NESTDECL))
    tcc_error("c_api(...) in a declarator in parentheses is not supported");
  if ((type->t & VT_BTYPE) == VT_FUNC && type->ref)
  {
    /* the result: C may say more const than yasc -- it only reads */
    Sym *f = type->ref, *ca = yasc_capi_of(f);
    capi_slot(&f->type, lv, &c, 1, &full);
    full.t &= ~VT_CONSTANT;
    if (ca)
      ca->type = full;
    else
      capi_facts(f)->c_api_ref = capi_functype(f, &full, NULL);
  }
  else
    /* a member or a variable: checked; the C header is the generator's */
    capi_slot(type, lv, &c, 0, &full);
}

/* patch_type: SYM declared again with function TYPE, which C's
   compatibility rejects.  1 when one of the two said c_api(...) and the
   other is that C type: the symbol keeps (or takes) the yasc one. */
int yasc_capi_redecl(Sym *sym, CType *type)
{
  Sym *co = yasc_capi_of(sym->type.ref), *cn = yasc_capi_of(type->ref);
  CType t;
  t.t = VT_FUNC;
  if (co && !cn)
  {
    t.ref = co;
    return is_compatible_types(&t, type);
  }
  if (cn && !co)
  {
    t.ref = cn;
    if (!is_compatible_types(&t, &sym->type))
      return 0;
    sym->type.ref = type->ref;
    return 1;
  }
  return 0;
}

/* patch_type, after the merge: the C type of either declaration stays with
   the symbol's function type; two that say different ones are an error */
void yasc_capi_merge(Sym *sym, Sym *ca_old, Sym *ca_new, Sym *ya_old, Sym *ya_new)
{
  Sym *ref = sym->type.ref;
  if (ref && (ya_old || ya_new) && !yasc_api_of(ref))
    capi_facts(ref)->yasc_api_ref = ya_old ? ya_old : ya_new;
  if (ca_old && ca_new && ca_old != ca_new)
  {
    CType a, b;
    a.t = b.t = VT_FUNC;
    a.ref = ca_old, b.ref = ca_new;
    if (!is_compatible_types(&a, &b))
      tcc_error("'%s' is declared again with another c_api(...)", get_tok_str(sym->v, NULL));
  }
  if (ref && (ca_old || ca_new) && !yasc_capi_of(ref))
    capi_facts(ref)->c_api_ref = ca_old ? ca_old : ca_new;
}

/* verify_assign_cast: function SRC (its type) to a pointer to function
   DST: 1 when SRC said c_api(...) and DST is that C type */
int yasc_capi_fnptr_ok(CType *dst, CType *src)
{
  Sym *ca = (src->t & VT_BTYPE) == VT_FUNC ? yasc_capi_of(src->ref) : NULL;
  CType t;
  if (!ca)
    return 0;
  t.t = VT_FUNC;
  t.ref = ca;
  return is_compatible_types(dst, &t);
}

/* ---- regular C: yasc_c_api(n, T) ---------------------------------------- */

/* yasc_c_api(n, T) attributes parsed into OWNER, until its declaration takes
   them (attr_apply_decl_facts); a new declaration's AttributeDef forgets
   them (parse_btype).  Not in AttributeDef: post_type has one in its frame. */
static struct
{
  AttributeDef *owner;
  int n;
  struct
  {
    int slot;
    CType type;
  } a[8];
} capi_attrs;

/* attribute.c: `yasc_c_api(n, T)` after its name, at the '(' */
void yasc_capi_attr_parse(AttributeDef *ad)
{
  int n;
  CType t;
  skip('(');
  n = expr_const();
  skip(',');
  parse_type(&t);
  skip(')');
  t.t &= ~VT_STORAGE;
  if (capi_attrs.owner != ad)
    capi_attrs.owner = ad, capi_attrs.n = 0;
  if (n < 0 || n > 255)
    tcc_warning_c(warn_ignored_attributes)("yasc_c_api(%d, ...): no such parameter", n);
  else if (capi_attrs.n == (int)(sizeof capi_attrs.a / sizeof capi_attrs.a[0]))
    tcc_warning_c(warn_ignored_attributes)("yasc_c_api: more than %d on one declaration, ignored",
                                           (int)(sizeof capi_attrs.a / sizeof capi_attrs.a[0]));
  else
  {
    capi_attrs.a[capi_attrs.n].slot = n;
    capi_attrs.a[capi_attrs.n++].type = t;
  }
}

/* parse_btype: AD starts a declaration of its own */
void yasc_capi_attr_forget(AttributeDef *ad)
{
  if (capi_attrs.owner == ad)
    capi_attrs.owner = NULL, capi_attrs.n = 0;
}

/* attr_apply_decl_facts: function TYPE was declared with AD's yasc_c_api(...):
   the yasc declaration's type, beside the C one, for -Wyasc-c-api */
void yasc_capi_attr_apply(CType *type, AttributeDef *ad)
{
  CapiList l;
  CType res, *rp = NULL;
  Sym *f, *p;
  int i, np = 0, nattr;
  if (capi_attrs.owner != ad || !capi_attrs.n)
    return;
  nattr = capi_attrs.n;
  capi_attrs.owner = NULL, capi_attrs.n = 0;
  if ((type->t & VT_BTYPE) != VT_FUNC || !(f = type->ref) || yasc_api_of(f))
    return;
  for (p = sym_first(f); p; p = p->next)
    np++;
  l.n = np;
  l.t = tcc_mallocz((np ? np : 1) * sizeof *l.t);
  l.on = tcc_mallocz(np ? np : 1);
  for (i = 0; i < nattr; i++)
  {
    int n = capi_attrs.a[i].slot, k;
    CType t = capi_attrs.a[i].type;
    if (!n)
    {
      if (capi_mismatch(&t, &f->type, 0, 1))
        continue;
      res = t;
      rp = &res;
      continue;
    }
    for (p = sym_first(f), k = 1; p && k < n; p = p->next)
      k++;
    if (!p)
    {
      tcc_warning_c(warn_ignored_attributes)("yasc_c_api(%d, ...): the function has no parameter %d", n, n);
      continue;
    }
    convert_parameter_type(&t);
    /* which side writes is the yasc declaration's business: both directions */
    if (capi_mismatch(&t, &p->type, 0, 1))
    {
      char b[256];
      type_to_str(b, sizeof b, &t, NULL);
      tcc_warning_c(warn_ignored_attributes)("yasc_c_api(%d, %s) does not fit parameter %d: ignored", n, b, n);
      continue;
    }
    l.t[n - 1] = t;
    l.on[n - 1] = 1;
  }
  capi_facts(f)->yasc_api_ref = capi_functype(f, rp, &l);
  yasc_capi_c_seen = 1;
  tcc_free(l.t);
  tcc_free(l.on);
}

/* The yasc type function type F's caller is told about by yasc_c_api(...),
   or NULL */
Sym *yasc_capi_yasc_type(Sym *f)
{
  return yasc_api_of(f);
}

/* ---- -Wyasc-c-api's facts of a call (ir/capi_check.c) ------------------- */


/* yasc type Y says const LEVEL pointer levels down where C type C does not */
static int capi_read_only(CType *y, CType *c, int level)
{
  int i;
  for (i = 0; i < level; i++)
  {
    if ((y->t & VT_BTYPE) != VT_PTR || (c->t & VT_BTYPE) != VT_PTR || !y->ref || !c->ref)
      return 0;
    y = &y->ref->type, c = &c->ref->type;
  }
  return (y->t & VT_CONSTANT) && !(c->t & VT_CONSTANT);
}

void yasc_capi_call_masks(Sym *f, int *ret, uint32_t *out, uint32_t *wr)
{
  Sym *y = yasc_api_of(f), *p, *q;
  int k;
  *ret = 0, *out = 0, *wr = 0;
  for (p = sym_first(f), k = 0; p && k < 32; p = p->next, k++)
    if ((p->type.t & VT_BTYPE) == VT_PTR && p->type.ref && !(p->type.ref->type.t & VT_CONSTANT) &&
        (p->type.ref->type.t & VT_BTYPE) != VT_FUNC)
      *wr |= 1u << k;
  if (!y)
    return;
  *ret = capi_read_only(&y->type, &f->type, 1);
  for (p = sym_first(f), q = sym_first(y), k = 0; p && q && k < 32; p = p->next, q = q->next, k++)
    if (capi_read_only(&q->type, &p->type, 2))
      *out |= 1u << k;
}
