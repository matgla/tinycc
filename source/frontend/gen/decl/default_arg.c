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

/* default_arg.c -- yasc default arguments (docs/yasc_default_args.md).
 *
 *   void log_warn(in char [:0] *msg, yasc_src at = @src());
 *   int parse(in char [:0] *s, int base = 10);
 *
 *   log_warn("disk full");     // log_warn("disk full", <this call's @src()>)
 *   parse("777", 8);           // given: as written
 *   parse("42");               // parse("42", 10)
 *
 * As in C++: a call may leave out trailing parameters that have a default,
 * and the caller passes the default in their place -- the function is an
 * ordinary C function with every parameter, so C links with it as it is and
 * a C header of it simply lists them all.  `@src()` in a default is the
 * CALLER's location: a new @src() value is made at each call that leaves the
 * parameter out (builtin/misc.c).  Any other default is a constant
 * expression (a number, nullptr, a string literal, the address of a static
 * object or a function), evaluated and converted to the parameter's type
 * where it is written, so the names in it mean what they meant there and no
 * call site can capture them.
 *
 * Only a function declaration's own parameters take defaults, only at file
 * scope.  A later declaration may add defaults, or say one again, but not
 * change one, and once a parameter has one every parameter after it has one
 * too.  The defaults are
 * kept by the function's name (a prototype's Syms may be parked or spilled,
 * decl/... symtab.c), and only a direct call uses them: a call through a
 * pointer passes every argument, as in C++. */

#include "gen_priv.h"

typedef struct DefaultArg
{
  SValue sv; /* the value, already of the parameter's type */
  int line;
  unsigned char set;
} DefaultArg;

struct YascDefaults
{
  Sym *fn;      /* while pending: the function type the parameters are of */
  int v;        /* once bound: the function's name */
  int n;        /* parameters */
  DefaultArg d[];
};

static YascDefaults **pending, **bound;
static int nb_pending, nb_bound;
static Sym marker;    /* @src() while a default is checked */
static int marker_on; /* a default is being parsed */

Sym *yasc_default_marker(void)
{
  /* (a default is at file scope: an error that cut one short leaves no
     stale marker for an @src() in a body) */
  return marker_on && !local_stack ? &marker : NULL;
}

static YascDefaults *defaults_grow(YascDefaults *d, int n)
{
  int old = d ? d->n : 0;
  if (old >= n)
    return d;
  d = tcc_realloc(d, sizeof *d + n * sizeof d->d[0]);
  if (!old)
    d->fn = NULL, d->v = 0;
  memset(&d->d[old], 0, (n - old) * sizeof d->d[0]);
  d->n = n;
  return d;
}

/* `= EXPR` after parameter IDX of type PT: tok is '=' */
void yasc_default_param(YascDefaults **pd, int idx, CType *pt)
{
  const int line = file->line_num, saved_nocode = nocode_wanted;
  int is_src;

  if (!tcc_state->yasc || pp_in_c())
    tcc_error("a default argument is yasc ('#pragma yasc' or -std=yasc)");
  if (local_stack)
    tcc_error("a default argument goes in a file-scope declaration only");
  next();
  memset(&marker, 0, sizeof marker);
  marker.r = VT_CONST | VT_SYM;
  marker.vreg = -1;
  /* a static initializer's rules: constants, addresses of static objects */
  nocode_wanted = DATA_ONLY_WANTED | CONST_WANTED_BIT;
  marker_on = 1;
  expr_cond();
  marker_on = 0;
  gen_assign_cast(pt);
  nocode_wanted = saved_nocode;
  is_src = vtop->sym == &marker && (vtop->r & VT_SYM);
  /* @src() itself, the value: not a member of it, not its address */
  if (is_src ? (vtop->r & (VT_VALMASK | VT_LVAL)) != (VT_CONST | VT_LVAL) || vtop->c.i
             : (vtop->r & (VT_VALMASK | VT_LVAL | VT_NONCONST)) != VT_CONST)
    tcc_error("a default argument is a constant expression or @src()");
  *pd = defaults_grow(*pd, idx + 1);
  (*pd)->d[idx].sv = *vtop;
  (*pd)->d[idx].line = line;
  (*pd)->d[idx].set = 1;
  vpop();
}

/* the parameter list of function type FN, NPARAMS long, ended: its defaults
 * wait for the declaration to name the function */
void yasc_default_attach(YascDefaults *d, Sym *fn, int nparams)
{
  if (!d)
    return;
  d = defaults_grow(d, nparams);
  d->fn = fn;
  pending = tcc_realloc(pending, (nb_pending + 1) * sizeof *pending);
  pending[nb_pending++] = d;
}

static YascDefaults *defaults_find(int v)
{
  for (int i = 0; i < nb_bound; i++)
    if (bound[i]->v == v)
      return bound[i];
  return NULL;
}

static int first_line(YascDefaults *d)
{
  for (int i = 0; i < d->n; i++)
    if (d->d[i].set)
      return d->d[i].line;
  return 0;
}

/* the same default twice: a header and its definition both writing it, or a
 * yasc method, whose tokens are replayed as a prototype and as the
 * definition (decl/method.c) */
static int default_same(SValue *a, SValue *b)
{
  if (a->r != b->r || a->c.i != b->c.i || !is_compatible_types(&a->type, &b->type))
    return 0;
  if (!(a->r & VT_SYM) || a->sym == b->sym)
    return 1;
  if (a->sym == &marker || b->sym == &marker)
    return 0;
  /* two identical string literals: one copy in .rodata (strlit_pool.c) */
  ElfSym *ea = elfsym(a->sym), *eb = elfsym(b->sym);
  return ea && eb && ea->st_shndx == eb->st_shndx && ea->st_value == eb->st_value;
}

static void defaults_trailing(YascDefaults *d)
{
  int seen = -1;
  for (int i = 0; i < d->n; i++)
    if (d->d[i].set)
    {
      if (seen < 0)
        seen = i;
    }
    else if (seen >= 0)
      tcc_error("parameter %d of '%s' has no default argument but parameter %d before it has one: defaults go "
                "at the end",
                i + 1, get_tok_str(d->v, NULL), seen + 1);
}

/* A declarator named V with TYPE ended (OWN: it built the function type
 * itself).  The defaults written in it go to V; any written elsewhere -- a
 * typedef, a pointer, a parameter of function type -- are an error. */
void yasc_default_bind(int v, CType *type, int own_functype)
{
  YascDefaults *mine = NULL, *old;
  int i;

  marker_on = 0; /* (an error inside a default may have skipped its reset) */
  if (!nb_pending)
    return;
  for (i = 0; i < nb_pending; i++)
  {
    YascDefaults *p = pending[i];
    if (own_functype && v && (type->t & VT_BTYPE) == VT_FUNC && !(type->t & VT_TYPEDEF) && p->fn == type->ref)
      mine = p;
    else
    {
      int line = first_line(p);
      for (int j = 0; j < nb_pending; j++)
        if (pending[j] != mine)
          tcc_free(pending[j]);
      nb_pending = 0;
      if (mine)
        tcc_free(mine);
      tcc_error("default arguments (line %d) go on a function declaration's own parameters, not in a "
                "typedef, a pointer or a parameter",
                line);
    }
  }
  nb_pending = 0;
  if (!mine)
    return;
  mine->fn = NULL;
  mine->v = v;
  old = defaults_find(v);
  if (!old)
  {
    bound = tcc_realloc(bound, (nb_bound + 1) * sizeof *bound);
    bound[nb_bound++] = mine;
    defaults_trailing(mine);
    return;
  }
  if (old->n != mine->n)
  {
    tcc_free(mine); /* the declarations conflict: their types say so */
    return;
  }
  for (i = 0; i < mine->n; i++)
    if (mine->d[i].set)
    {
      if (old->d[i].set)
      {
        int line = old->d[i].line;
        if (default_same(&old->d[i].sv, &mine->d[i].sv))
          continue;
        tcc_free(mine);
        tcc_error("parameter %d of '%s' has another default argument already (line %d)", i + 1,
                  get_tok_str(v, NULL), line);
      }
      old->d[i] = mine->d[i];
    }
  tcc_free(mine);
  defaults_trailing(old);
}

/* Has parameter IDX of the function FSYM a default argument? */
int yasc_default_have(Sym *fsym, int idx)
{
  YascDefaults *d;
  if (!fsym || !nb_bound)
    return 0;
  d = defaults_find(fsym->v);
  return d && idx < d->n && d->d[idx].set;
}

/* push parameter IDX's default for a call of FSYM on LINE */
void yasc_default_push(Sym *fsym, int idx, int line)
{
  DefaultArg *a = &defaults_find(fsym->v)->d[idx];
  if (a->sv.sym == &marker && (a->sv.r & VT_SYM))
    yasc_src_push(NULL, line); /* the caller's location */
  else
    vpushv(&a->sv);
}

void yasc_default_reset(void)
{
  for (int i = 0; i < nb_pending; i++)
    tcc_free(pending[i]);
  for (int i = 0; i < nb_bound; i++)
    tcc_free(bound[i]);
  tcc_free(pending);
  tcc_free(bound);
  pending = bound = NULL;
  nb_pending = nb_bound = 0;
  marker_on = 0;
}
