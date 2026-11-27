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

/* plain_param.c -- yasc's "plain pointer in yasc: say owned or view" for a
 * parameter or a local, with the fix-it the body decides.
 *
 * A function's own parameter (not one of a nested declarator) that is a
 * plain pointer waits here instead of being reported at its '*'
 * (yasc_plain_error): once the declarator is known to be a definition whose
 * body is parsed now, the error waits for the end of that body, and the
 * note and fix-it say what the body does with the parameter:
 *
 *   owned  freed (free, realloc, a malloc(dealloc) deallocator), passed to a
 *          `drop` or `owned` parameter, stored where it outlives the call (a
 *          global, a static, through a pointer, into an owned variable), or
 *          returned from a function whose result is owned;
 *   view   anything else;
 *   maybe  as well when the body compares it with null (`if (p)`, `!p`,
 *          `p == NULL`) or passes it to a `maybe` parameter that borrows it
 *          (free's `drop ... maybe` takes null too, which says nothing).
 *
 * A local's own pointer (an automatic variable's outermost '*', written in
 * the declarator itself) waits the same way, for the end of the body it is
 * declared in: owned when it is given an owned value (`char *p =
 * malloc(n)`), freed, passed to an owned parameter, stored into an owned
 * pointer or returned as owned; else a view, never null unless it says
 * `maybe` (null or a maybe value stored into it is the flow checks' error,
 * with the `* maybe` fix-it).
 * Inner levels (`char **pp`'s first '*'), statics, typedefs, function
 * pointers' parameters and the pointers of nested declarators are reported at
 * once, as before.
 *
 * An alias is followed: a local given the value of a followed parameter or
 * local (`q = p`, `char *q = p`, also an annotated or `auto` one) carries its
 * uses to it -- `free(q)` frees p ("the function frees 'p' through 'q'") --
 * until it is given something else.  The analysis follows the source order,
 * not the paths.
 *
 * The error is still an error, given at the pointer's line and naming it
 * ("... say owned or view for 's'", so two on one line are both said), after
 * the body's own diagnostics and before the function's flow checks (which
 * run on the IR once the body is parsed), in line order -- so text, JSON and
 * SARIF keep each note and fix-it with its error, and the error comes where
 * the body's other reports do.  A definition with such a parameter is
 * compiled at once, never deferred (-finline-functions-called-once,
 * -fdrop-unused-statics): an error is coming, nothing is written, and the
 * order stays the same at every -O.  A static inline body is parsed at the
 * end of the unit, when used or by the check of uncompiled inline functions
 * (also at every -O); one never parsed is reported at the end of the unit
 * with `* view`.  A prototype, a nested function (its body is compiled later)
 * and a parameter of anything that is not a file-scope definition (a
 * function pointer, a cast, a typedef) are reported at the end of their
 * declarator with `* view`, as before.
 *
 * The uses are seen by the frontend as the body is parsed (an inline
 * expansion's body is not the function's).  The fix-it goes after the '*'
 * when the line has one, else rewrites `*name` when that is there once, else
 * goes before the name when that is there once, else there is none.
 */

#include "gen_priv.h"

/* what the body does with the parameter or local */
#define PP_FREED 0x01    /* free, a deallocator, a drop parameter */
#define PP_OWNPARAM 0x02 /* an owned parameter */
#define PP_STORED 0x04   /* kept where it outlives the call */
#define PP_RETURNED 0x08 /* returned as owned */
#define PP_OWNSRC 0x40   /* a local given an owned value */
#define PP_OWN (PP_FREED | PP_OWNPARAM | PP_STORED | PP_RETURNED | PP_OWNSRC)
#define PP_NULLTEST 0x10  /* compared with null */
#define PP_MAYBEARG 0x20  /* passed to a maybe parameter */
#define PP_MAYBESRC 0x80  /* a local given null or a maybe value */
#define PP_MAYBE (PP_NULLTEST | PP_MAYBEARG | PP_MAYBESRC)
/* a local given a pointer whose target is const (an `in` parameter):
   `const * view`, not `* view`, keeps it */
#define PP_CONSTSRC 0x100
/* what an alias's use says of what it aliases */
#define PP_CARRIED (PP_FREED | PP_OWNPARAM | PP_STORED | PP_RETURNED | PP_NULLTEST | PP_MAYBEARG)

enum
{
  PPS_HELD,  /* its declarator is not known to be a definition yet */
  PPS_BODY,  /* its function's body is being parsed (a local: bound) */
  PPS_TAKEN, /* yasc_pp_body_begin: its body begins now */
  PPS_LHELD, /* a local's declarator: its variable is not made yet */
  PPS_ALIAS  /* a variable that copies a followed one: never reported */
};

typedef struct PlainParam
{
  int line, tok, idx, id;
  Sym *def;   /* the definition's function type (yasc_pp_declarator) */
  Sym *local; /* PPS_BODY, PPS_ALIAS: the variable in the body */
  int local_vr, local_c; /* its vreg, else frame offset (a popped Sym may be reused) */
  int alias_of; /* the id of the entry it copies, 0: none */
  unsigned char state, is_local;
  unsigned char ptr_local;   /* a local pointer (not an array of them) */
  unsigned short flags;
  unsigned char own_why, maybe_why; /* the first PP_OWN / PP_MAYBE seen */
  int own_line, maybe_line;
  int own_via, maybe_via; /* through this alias (a token), 0: itself */
  char spell[64];         /* "char *": the pointer, for an error with no name */
} PlainParam;
static PlainParam *pp;
static int pp_nb, pp_cap, pp_ids;
static Sym *pp_last_def; /* yasc_pp_declarator's, for yasc_pp_rekey */
static int pp_bodies;    /* bodies being parsed */
int yasc_pp_live;        /* PPS_BODY / PPS_ALIAS entries: the hooks look */
int yasc_pp_local_decl;  /* a local declarator is being read (decl()) */
int yasc_global_decl;    /* a global's / static's declarator is (gen_priv.h) */
int yasc_pp_binding;     /* PPS_LHELD entries named: yasc_pp_local_bind looks */
int yasc_pp_view_line;   /* the local declarator's own pointer says view, at this line */

static void pp_count(void)
{
  yasc_pp_live = yasc_pp_binding = 0;
  for (int i = 0; i < pp_nb; i++)
  {
    yasc_pp_live += pp[i].state == PPS_BODY || pp[i].state == PPS_ALIAS;
    yasc_pp_binding += pp[i].state == PPS_LHELD && pp[i].tok;
  }
}

static int pp_new(int line, int state)
{
  if (pp_nb == pp_cap)
    pp = tcc_realloc(pp, (pp_cap = pp_cap ? 2 * pp_cap : 8) * sizeof *pp);
  memset(&pp[pp_nb], 0, sizeof pp[0]);
  pp[pp_nb].line = line;
  pp[pp_nb].state = state;
  pp[pp_nb].idx = -1;
  pp[pp_nb].id = ++pp_ids;
  return pp_nb++;
}

/* the error of entry I, which names no variable: the pointer's type */
static void pp_unnamed_error(int i)
{
  tcc_error_noabort("%i:plain pointer in yasc: say owned or view for '%s'", pp[i].line, pp[i].spell);
}

/* the note's text: literals for the immediate path, this for the decided */
static char pp_note[256];
/* the second place for the fix-it: `*name` replaced by `*kw name` */
static char pp_alt_word[80], pp_alt_text[96];

/* the fix-its of the pointer of entry I: after its '*', else `*name`
   rewritten, else before the name */
/* CNST: the target becomes const too, `char const * view p` (C lets the
   const stand right before the '*') */
static void pp_fixits(int i, const char *name, const char *kw, const char *note, int cnst)
{
  static char fix[32];
  snprintf(fix, sizeof fix, cnst ? "const *%s " : "%s ", kw);
  tcc_fixit_next(pp[i].line, "*", cnst ? FIXIT_REPLACE_FIRST : FIXIT_AFTER, fix, note);
  if (!name)
    return;
  snprintf(pp_alt_word, sizeof pp_alt_word, "*%s", name);
  snprintf(pp_alt_text, sizeof pp_alt_text, cnst ? "const *%s %s" : "*%s %s", kw, name);
  tcc_fixit_next_alt(pp_alt_word, cnst ? FIXIT_REPLACE_FIRST : FIXIT_REPLACE, pp_alt_text);
}

static void pp_report(int i)
{
  const char *name = pp[i].tok ? get_tok_str(pp[i].tok, NULL) : NULL;
  int f = pp[i].flags, n;
  if (pp[i].state == PPS_ALIAS)
    return;
  /* a local pointer the body does not own is the `view` it was while the
     body was parsed (declarator.c): no error.  The checks followed it as a
     written one (null stored into it: lifetime.c says `* maybe`). */
  if (pp[i].state == PPS_BODY && pp[i].is_local && pp[i].ptr_local && name && !(f & PP_OWN))
    return;
  /* an error: the flow checks say nothing more of it */
  if (pp[i].state == PPS_BODY && pp[i].is_local)
    tcc_ir_lt_silence(tcc_state->ir, pp[i].local_vr);
  if (pp[i].state == PPS_LHELD)
    ; /* a local never made: no decision, no fix-it */
  else if (pp[i].state == PPS_HELD || !name)
  {
    pp_fixits(i, name, "view",
              "a parameter the function does not free only borrows: write '*view' ('*owned' when it frees it)", 0);
  }
  else
  {
    static const char *const own_what[] = {"frees '%s'", "passes '%s' on to an owned parameter",
                                           "keeps '%s' where it outlives the call", "returns '%s' as owned",
                                           "gives '%s' an owned value"};
    static const char *const kws[] = {"view", "?view", "owned", "?owned"};
    int k = ((f & PP_OWN) ? 2 : 0) | ((f & PP_MAYBE) ? 1 : 0);
    const int cnst = !(k & 2) && (f & PP_CONSTSRC);

    if (f & PP_OWN)
    {
      int w = pp[i].own_why == PP_FREED      ? 0
              : pp[i].own_why == PP_OWNPARAM ? 1
              : pp[i].own_why == PP_STORED   ? 2
              : pp[i].own_why == PP_RETURNED ? 3
                                             : 4;
      n = snprintf(pp_note, sizeof pp_note, "the function ");
      n += snprintf(pp_note + n, sizeof pp_note - n, own_what[w], name);
      if (pp[i].own_via && n < (int)sizeof pp_note)
        n += snprintf(pp_note + n, sizeof pp_note - n, " through '%s'", get_tok_str(pp[i].own_via, NULL));
      if (n < (int)sizeof pp_note)
        n += snprintf(pp_note + n, sizeof pp_note - n, " (line %d)", pp[i].own_line);
    }
    else if (pp[i].is_local)
      n = snprintf(pp_note, sizeof pp_note, "the function only borrows through '%s'", name);
    else
      n = snprintf(pp_note, sizeof pp_note, "the function only borrows '%s'", name);
    if (pp[i].own_why == PP_OWNSRC && pp[i].maybe_why == PP_MAYBESRC && pp[i].own_line == pp[i].maybe_line &&
        !pp[i].maybe_via)
      /* `char *p = malloc(n)`: one value says both */
      n = snprintf(pp_note, sizeof pp_note, "the function gives '%s' an owned value that may be null (line %d)", name,
                   pp[i].own_line);
    else if (f & PP_MAYBE && n < (int)sizeof pp_note)
    {
      const char *what = pp[i].maybe_why == PP_NULLTEST    ? "tests it for null"
                         : pp[i].maybe_why == PP_MAYBEARG  ? "passes it to a '?' parameter"
                                                           : "gives it null or a '?' value";
      n += snprintf(pp_note + n, sizeof pp_note - n, " and %s", what);
      if (pp[i].maybe_via && n < (int)sizeof pp_note)
        n += snprintf(pp_note + n, sizeof pp_note - n, " through '%s'", get_tok_str(pp[i].maybe_via, NULL));
      if (n < (int)sizeof pp_note)
        n += snprintf(pp_note + n, sizeof pp_note - n, " (line %d)", pp[i].maybe_line);
    }
    if (n < (int)sizeof pp_note)
      snprintf(pp_note + n, sizeof pp_note - n, ": write '%s*%s'", cnst ? "const " : "", kws[k]);
    pp_fixits(i, name, kws[k], pp_note, cnst);
  }
  if (name && pp[i].state == PPS_BODY && pp[i].is_local && pp[i].ptr_local)
    tcc_error_noabort("%i:plain pointer in yasc: '%s' owns what it points at, say owned", pp[i].line, name);
  else if (name)
    tcc_error_noabort("%i:plain pointer in yasc: say owned or view for '%s'", pp[i].line, name);
  else
    pp_unnamed_error(i);
}

/* entries FROM.. in state STATE (-1: any) are reported and dropped */
static void pp_flush(int from, int state)
{
  int i, j = from;
  /* in line order (a body's parameters and locals), stable */
  for (i = from + 1; i < pp_nb; i++)
  {
    PlainParam t = pp[i];
    for (j = i; j > from && pp[j - 1].line > t.line; j--)
      pp[j] = pp[j - 1];
    pp[j] = t;
  }
  j = from;
  for (i = from; i < pp_nb; i++)
    if (state < 0 || pp[i].state == state)
      pp_report(i);
    else
      pp[j++] = pp[i];
  pp_nb = j;
  pp_count();
}

/* A function's own parameter at LINE is a plain pointer: 1 when it waits
   here (its name follows: yasc_pp_name) */
int yasc_pp_hold(int line, CType *target)
{
  const int i = pp_new(line, PPS_HELD);
  yasc_plain_spell(pp[i].spell, sizeof pp[i].spell, target);
  return 1;
}

/* A local declarator's own pointer at LINE is a plain pointer: 1 when it
   waits here (its name follows: yasc_pp_local_end).  An inner level of the
   same declarator (`char **pp`) waited first: it is said now. */
int yasc_pp_hold_local(int line, CType *target)
{
  if (!pp_bodies || tcc_state->in_inline_expansion)
    return 0;
  if (pp_nb && pp[pp_nb - 1].state == PPS_LHELD && !pp[pp_nb - 1].tok)
  {
    pp_unnamed_error(--pp_nb);
  }
  {
    int i = pp_new(line, PPS_LHELD); /* (pp may move) */
    pp[i].is_local = 1;
    yasc_plain_spell(pp[i].spell, sizeof pp[i].spell, target);
  }
  return 1;
}

/* decl() reads a local declarator: returns the mark yasc_pp_local_end takes */
int yasc_pp_local_begin(void)
{
  yasc_pp_local_decl = 1;
  yasc_pp_view_line = 0;
  return pp_nb;
}

/* the local declarator begun at MARK declares TOK; KEEP: an automatic
   variable (its pointer waits for the variable, yasc_pp_local_bind), else a
   static, typedef, extern or function, said now */
void yasc_pp_local_end(int mark, int tok, int keep)
{
  int i, j = mark;
  yasc_pp_local_decl = 0;
  for (i = mark; i < pp_nb; i++)
  {
    if (pp[i].state == PPS_LHELD && !pp[i].tok)
    {
      if (!keep || tok < TOK_UIDENT || (tok & SYM_FIELD))
      {
        pp_unnamed_error(i);
        continue;
      }
      pp[i].tok = tok;
    }
    pp[j++] = pp[i];
  }
  pp_nb = j;
  pp_count();
}

/* the local declarator just read declares TYPE, an automatic variable when
   KEEP: `view` on its own pointer says what a plain one is
   (-Wredundant-view).  `view maybe` says more (`* maybe` is enough), and so
   does `restrict view`. */
void yasc_pp_redundant_view(CType *type, int keep)
{
  const int line = yasc_pp_view_line;
  yasc_pp_view_line = 0;
  if (!line || !keep || (type->t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !tcc_warn_wanted(warn_redundant_view) ||
      (type_yasc(type) & (YASC_VIEW | YASC_OWNED | YASC_UNCHECKED | YASC_MAYBE | YASC_RESTRICT)) != YASC_VIEW)
    return;
  tcc_warning_once_at(warn_redundant_view, line, "a local's pointer is a view unless the function owns it: drop 'view'");
  tcc_fixit_drop(line, "view");
}

/* the local SYM is made: the pointer that waits for it is followed */
void yasc_pp_local_bind(Sym *sym)
{
  for (int i = pp_nb; i-- > 0;)
    if (pp[i].state == PPS_LHELD && pp[i].tok == (sym->v & ~SYM_FIELD))
    {
      pp[i].state = PPS_BODY;
      pp[i].local = sym;
      pp[i].local_vr = sym->vreg;
      pp[i].local_c = sym->c;
      pp[i].ptr_local = (sym->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR;
      /* its view is implicit: null stored into it says `* maybe` */
      if (pp[i].ptr_local)
        tcc_ir_lt_implicit_view(tcc_state->ir, sym->vreg);
      break;
    }
  pp_count();
}

/* the local just made is an array of plain pointers, said at the end of the
   body: its initializer's null elements are not said too (yasc_init_null) */
int yasc_pp_held_array(void)
{
  /* (bound to its variable, or not yet: -fsyntax-only makes none; an
     aggregate's held pointer is an array's element either way) */
  return pp_nb && pp[pp_nb - 1].is_local &&
         ((pp[pp_nb - 1].state == PPS_BODY && !pp[pp_nb - 1].ptr_local) ||
          (pp[pp_nb - 1].state == PPS_LHELD && pp[pp_nb - 1].tok));
}

/* the parameter IDX (from 0) whose declarator began at entry MARK is TOK */
void yasc_pp_name(int mark, int tok, int idx)
{
  for (int i = mark; i < pp_nb; i++)
    if (pp[i].state == PPS_HELD && pp[i].idx < 0)
      pp[i].tok = tok >= TOK_UIDENT && !(tok & SYM_FIELD) ? tok : 0, pp[i].idx = idx;
}

int yasc_pp_mark(void)
{
  return pp_nb;
}

/* A declarator of decl() is done: DEF the function type of a definition,
   else NULL.  The entries of no definition are reported now; a definition's
   wait for its body (yasc_pp_body_begin), parsed now or later (deferred,
   inline, at the end of the unit), or for the end of the unit. */
void yasc_pp_declarator(Sym *def)
{
  int i, j = 0;
  pp_last_def = def;
  for (i = 0; i < pp_nb; i++)
  {
    Sym *p = NULL;
    int k = 0;
    if (pp[i].state != PPS_HELD || pp[i].def)
    {
      pp[j++] = pp[i];
      continue;
    }
    if (def)
      for (p = sym_first(def); p && k < pp[i].idx; p = p->next)
        k++;
    if (p && pp[i].idx >= 0 && (!pp[i].tok || (p->v & ~SYM_FIELD) == pp[i].tok))
    {
      pp[i].def = def;
      pp[j++] = pp[i];
    }
    else
      pp_report(i);
  }
  pp_nb = j;
}

/* the definition the last declarator began has the function type TO, its
   symbol's (decl() may have copied the type since, a prototype's may be
   kept) */
void yasc_pp_rekey(Sym *to)
{
  for (int i = 0; i < pp_nb; i++)
    if (pp[i].state == PPS_HELD && pp[i].def && pp[i].def == pp_last_def)
      pp[i].def = to;
  pp_last_def = NULL;
}

/* whether a parameter waits for the body of the function type DEF */
int yasc_pp_waiting(Sym *def)
{
  for (int i = 0; i < pp_nb; i++)
    if (pp[i].state == PPS_HELD && pp[i].def == def)
      return 1;
  return 0;
}

/* SYM's body begins, its parameters in scope: the entries that wait for it
   are followed.  Returns the mark yasc_pp_body_end takes. */
int yasc_pp_body_begin(Sym *sym)
{
  int i, j;
  pp_bodies++;
  if (tcc_state->yasc)
    for (Sym *p = sym->type.ref->next; p; p = p->next)
    {
      Sym *l = (p->v & ~SYM_FIELD) ? sym_find(p->v & ~SYM_FIELD) : NULL;
      if (l)
        l->yasc |= YASC_BODY_PARAM;
    }
  if (!pp_nb)
    return 0;
  for (i = 0; i < pp_nb; i++)
    if (pp[i].state == PPS_HELD && pp[i].def && pp[i].def == sym->type.ref)
    {
      pp[i].state = PPS_TAKEN;
      pp[i].local = pp[i].tok ? sym_find(pp[i].tok) : NULL;
    }
  /* this body's entries go last: its end takes them from the mark */
  for (i = j = 0; i < pp_nb; i++)
    if (pp[i].state != PPS_TAKEN)
    {
      PlainParam t = pp[i];
      memmove(&pp[j + 1], &pp[j], (i - j) * sizeof pp[0]);
      pp[j++] = t;
    }
  for (i = j; i < pp_nb; i++)
    pp[i].state = PPS_BODY;
  pp_count();
  return j;
}

/* the body is parsed: its parameters' and locals' errors, decided */
void yasc_pp_body_end(int mark)
{
  if (pp_bodies)
    pp_bodies--;
  if (mark < pp_nb)
    pp_flush(mark, -1);
}

/* error recovery: in a body everything goes on; at file scope (BODY 0) the
   entries of no definition and those of a body left by the error are
   reported */
void yasc_pp_recover(int body)
{
  int i, j = 0;
  yasc_pp_local_decl = 0;
  yasc_global_decl = 0;
  if (body)
  {
    /* a local declarator the error left: said now */
    for (i = 0; i < pp_nb; i++)
      if (pp[i].state == PPS_LHELD && !pp[i].tok)
        pp_unnamed_error(i);
      else
        pp[j++] = pp[i];
    pp_nb = j;
    pp_count();
    return;
  }
  pp_bodies = 0;
  for (i = 0; i < pp_nb; i++)
    if (pp[i].state == PPS_HELD && pp[i].def)
      pp[j++] = pp[i];
    else
      pp_report(i);
  pp_nb = j;
  pp_count();
}

/* the end of the unit: nothing may stay unreported */
void yasc_pp_finish(void)
{
  if (pp_nb)
    pp_flush(0, -1);
  tcc_free(pp);
  pp = NULL;
  pp_cap = 0;
  pp_bodies = 0;
  yasc_pp_local_decl = 0;
  yasc_global_decl = 0;
}

/* entry I is the local of vreg VR, frame offset C (a register candidate's
   offset is not its own; vregs are) */
static int pp_same_local(int i, int vr, int c)
{
  return pp[i].local_vr >= 0 ? pp[i].local_vr == vr : pp[i].local_c == c;
}

/* SV is the value of an unsafe(expr) of this statement: what it does says
   nothing of what a variable in it owns (a view returned as owned) */
static int pp_unsafe(const SValue *sv)
{
  return sv == yasc_unsafe_value_at && sv->sym == yasc_unsafe_value.sym && sv->r == yasc_unsafe_value.r &&
         sv->c.i == yasc_unsafe_value.c.i && sv->type.ref == yasc_unsafe_value.type.ref;
}

/* the entry of the followed variable SV is the value of, or -1 (the newest:
   a popped local's Sym may be reused, its vreg not) */
static int pp_find(const SValue *sv)
{
  if (!(sv->r & VT_LVAL) || !sv->sym || (sv->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR ||
      tcc_state->in_inline_expansion || sv->type.ref != sv->sym->type.ref || pp_unsafe(sv))
    return -1;
  for (int i = pp_nb; i-- > 0;)
    if ((pp[i].state == PPS_BODY || pp[i].state == PPS_ALIAS) && pp[i].local == sv->sym &&
        (!pp[i].is_local || pp_same_local(i, sv->sym->vreg, sv->sym->c)))
      return i;
  return -1;
}

/* the followed variable a store goes to, or -1: pp_find's, or a local's
   initializer (init_putv names the slot, not the symbol) */
static int pp_find_dst(const SValue *dst)
{
  if (dst->sym)
    return pp_find(dst);
  if ((dst->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_LOCAL | VT_LVAL) ||
      (dst->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || tcc_state->in_inline_expansion)
    return -1;
  for (int i = pp_nb; i-- > 0;)
    if ((pp[i].state == PPS_BODY || pp[i].state == PPS_ALIAS) && pp[i].is_local &&
        pp_same_local(i, dst->vr, (int)dst->c.i))
      return i;
  return -1;
}

static int pp_by_id(int id)
{
  for (int i = pp_nb; i-- > 0;)
    if (pp[i].id == id && (pp[i].state == PPS_BODY || pp[i].state == PPS_ALIAS))
      return i;
  return -1;
}

static void pp_set_via(int i, int how, int via, int depth)
{
  int line = file ? file->line_num : 0;
  if ((how & PP_OWN) && !pp[i].own_why)
    pp[i].own_why = how, pp[i].own_line = line, pp[i].own_via = via;
  if ((how & PP_MAYBE) && !pp[i].maybe_why)
    pp[i].maybe_why = how, pp[i].maybe_line = line, pp[i].maybe_via = via;
  pp[i].flags |= how;
  /* an alias carries its uses to what it copies */
  if ((how & PP_CARRIED) && pp[i].alias_of && depth < 8)
  {
    int j = pp_by_id(pp[i].alias_of);
    if (j >= 0 && j != i)
      pp_set_via(j, how, via ? via : pp[i].tok, depth + 1);
  }
}

static void pp_set(int i, int how)
{
  pp_set_via(i, how, 0, 0);
}

/* SV, argument IDX (from 0) of a call of FSYM (NULL: through a pointer),
   goes to the parameter PARAM (NULL: variadic or unprototyped) */
void yasc_pp_arg(const SValue *sv, Sym *fsym, Sym *param, int idx)
{
  int i = pp_find(sv), q;
  if (i < 0)
    return;
  q = param ? type_yasc(&param->type) : 0;
  if (q & YASC_DROP)
    pp_set(i, PP_FREED);
  else if (q & YASC_OWNED)
    pp_set(i, PP_OWNPARAM);
  /* free(drop void * owned maybe p) takes null too: that says nothing of p */
  else if (q & YASC_MAYBE)
    pp_set(i, PP_MAYBEARG);
  if (fsym && (fsym->type.t & VT_BTYPE) == VT_FUNC)
  {
    const char *n = get_tok_str(fsym->v, NULL);
    const SymLocalFacts *f = fsym->a.attr_facts ? sym_facts_peek(fsym) : NULL;
    if (!strncmp(n, "__builtin_", 10))
      n += 10;
    if ((idx == 0 && (!strcmp(n, "free") || !strcmp(n, "realloc") || !strcmp(n, "reallocarray"))) ||
        (f && f->dealloc_arg == idx + 1))
      pp_set(i, PP_FREED);
  }
}

static int pp_null(const SValue *sv);

/* vstore: VAL is stored into DST */
void yasc_pp_store(const SValue *dst, const SValue *val)
{
  int i = pp_find(val), d = pp_find_dst(dst), v = dst->r & VT_VALMASK;
  int owned_dst = (type_yasc(&dst->type) & YASC_OWNED) != 0;
  if (d >= 0 && !owned_dst)
  {
    /* a followed variable given a value: a copy of another (an alias, until
       it is given something else), or what the value says */
    pp[d].alias_of = i >= 0 && i != d ? pp[i].id : 0;
    /* a copy of a parameter that becomes a view: an `in` one, a const target */
    if (i >= 0 && i != d && pp[d].is_local && !pp[i].is_local)
      pp[d].flags |= PP_CONSTSRC;
    /* an owned result (`malloc(n)`), not an owned variable or member, which
       a view borrows */
    if (i < 0 && pp[d].is_local && (type_yasc(&val->type) & YASC_OWNED) && !(val->r & VT_LVAL) && !pp_unsafe(val))
      pp_set(d, PP_OWNSRC);
    if (i < 0 && pp[d].is_local && ((type_yasc(&val->type) & YASC_MAYBE) || pp_null(val)))
      pp_set(d, PP_MAYBESRC);
    if (i < 0 && pp[d].is_local && (val->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && val->type.ref &&
        (val->type.ref->type.t & VT_CONSTANT) && !(dst->type.ref && (dst->type.ref->type.t & VT_CONSTANT)))
      pp[d].flags |= PP_CONSTSRC;
    return;
  }
  if (i < 0)
    return;
  if (owned_dst || !(dst->r & VT_LVAL) || (v != VT_LOCAL && v != VT_LLOCAL) || (dst->r & VT_SYM))
  {
    /* a local kept where it outlives the call owns only when that place
       does: a global or a pointer's target may just borrow it */
    if (owned_dst || !pp[i].is_local)
      pp_set(i, PP_STORED);
  }
  else if (dst->sym && (dst->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && dst->type.ref == dst->sym->type.ref)
  {
    /* into a local of its own (annotated, `auto`): followed as an alias */
    int a = pp_new(pp[i].line, PPS_ALIAS);
    pp[a].local = dst->sym;
    pp[a].local_vr = dst->sym->vreg;
    pp[a].local_c = dst->sym->c;
    pp[a].is_local = 1;
    pp[a].tok = dst->sym->v & ~SYM_FIELD;
    pp[a].alias_of = pp[i].id;
    pp_count();
  }
}

/* `return SV;` from a function whose result is owned */
void yasc_pp_return(const SValue *sv)
{
  int i = pp_find(sv);
  if (i >= 0)
    pp_set(i, PP_RETURNED);
}

static int pp_null(const SValue *sv)
{
  return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && sv->c.i == 0;
}

/* gen_op(TOK_EQ / TOK_NE) on vtop[-1], vtop: a waiting parameter against null */
void yasc_pp_compare(void)
{
  int i = -1;
  if (pp_null(vtop))
    i = pp_find(vtop - 1);
  else if (pp_null(vtop - 1))
    i = pp_find(vtop);
  if (i >= 0)
    pp_set(i, PP_NULLTEST);
}
