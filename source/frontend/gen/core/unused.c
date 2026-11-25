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

/* unused.c -- the -Wunused-* checks, decided from the source.
 *
 * Uses are recorded on the Syms (Sym.ymod, SYM_UNUSED_*) while the frontend
 * parses: unary() resolving an identifier marks it named and read (_REF,
 * _READ), and an assignment whose value is discarded takes the read back
 * (set but not used).  What is still unused is reported when its scope ends
 * (locals, parameters, labels) or at the end of the unit (file-scope
 * statics), with the line of its declaration.
 *
 * Results must not depend on -O, where bodies are parsed more than once, or
 * not at all:
 *  - locals, parameters and labels are tracked only outside inline
 *    expansions (and the speculative constant evaluation); any other parse of
 *    a body tracks them afresh, and a diagnostic already given for the same
 *    declaration is not given again (uu_first_report, tcc_diag_once) -- a deferred, re-optimized or
 *    check-only parse repeats the first one;
 *  - a static inline body is saved as tokens at every -O and may never be
 *    parsed (nothing calls it): its mentions of file-scope statics are taken
 *    from the tokens when it is saved, as gcc counts uses in a body it never
 *    emits.  Every other body is parsed at every -O (compiled, or checked
 *    once prune_unused_statics dropped it), and what its parse resolves is
 *    a subset of what its tokens name;
 *  - a static function naming itself is not a use of it (gcc: "defined but
 *    not used" for a function only recursion calls).
 *
 * -Wunused-value works from two parser facts: uv_effects counts the side
 * effects (calls, stores, ++/--, statement expressions, builtins) and
 * uv_last says what the last operation completed was (UV_*), which in a
 * recursive-descent parser is the top of the expression when it is done. */

#include "gen_priv.h"

int uv_effects, uv_last, uv_ident;

enum
{
  UK_LOCAL,
  UK_STATIC_LOCAL,
  UK_PARAM,
  UK_LABEL,
  UK_TYPEDEF,
  UK_FUNC,
  UK_VAR
};

typedef struct UnusedRec
{
  Sym *sym;
  const char *file; /* tcc_filename_intern */
  int line;
  unsigned char kind;
} UnusedRec;

typedef struct UnusedRecs
{
  UnusedRec *r;
  int nb, cap;
} UnusedRecs;

/* locals and parameters (a stack: their scopes nest), labels, statics */
static UnusedRecs uu_locals, uu_labels, uu_statics;

/* the line of each parameter of a function definition: the last
   file-scope parameter list post_type parsed (uu_plist), bound to the
   function when its body follows (uu_plines).  Parameter Syms are no keys:
   a prototype's list is spilled and read back as new Syms. */
typedef struct UnusedParamLine
{
  Sym *func;
  int v, line;
  int sys; /* -Wshadow: its name a system header's macro spelled */
} UnusedParamLine;
static UnusedParamLine *uu_plist, *uu_plines;
static int uu_nb_plist, uu_cap_plist, uu_nb_plines, uu_cap_plines;

/* alias("name") targets: named, so used */
static int *uu_alias_toks;
static int uu_nb_alias, uu_cap_alias;

/* set but not used: the identifier unary() resolved last, whether that
   mention is what first marked it read, and the variable the last assignment
   stored to whose value nothing has read since */
static Sym *uu_last_sym;
static unsigned char uu_last_newly_read;
static Sym *uu_pending;

/* A part of a local reached by an address the frontend did not fold
 * (`a[i]`, `s.arr[i].f`, a member of a local in a register): the local, kept
 * by the part's place and contents on the stack, and whether the local's
 * mention was what first marked it read */
static struct
{
  Sym *s;
  SValue *at;
  SValue sv;
  unsigned char newly_read;
} uu_part;
/* the bases of the subscripts being parsed: named in their index? */
#define UU_WATCH_MAX 8
static Sym *uu_part_watch[UU_WATCH_MAX];
static unsigned char uu_part_watch_hit[UU_WATCH_MAX];
static int uu_nb_watch;

/* the right operand of the last comma operator */
static unsigned char uv_comma_rhs_noeffect, uv_comma_rhs_last;

static void uu_add(UnusedRecs *v, Sym *sym, const char *file, int line, int kind)
{
  if (v->nb == v->cap)
  {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->r = tcc_realloc(v->r, v->cap * sizeof *v->r);
  }
  v->r[v->nb].sym = sym;
  v->r[v->nb].file = file;
  v->r[v->nb].line = line;
  v->r[v->nb].kind = kind;
  v->nb++;
}

static void uu_reset(UnusedRecs *v)
{
  tcc_free(v->r);
  v->r = NULL;
  v->nb = v->cap = 0;
}

/* the file the parser reads, past the ':asm:' and ':paste:' ones */
static BufferedFile *uu_file(void)
{
  BufferedFile *f = file;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  return f;
}

/* Is the current source a system header?  (A replayed body too: an inline
   expansion, a deferred or checked one -- tcc_in_sys_header.) */
static int uu_in_sys(void)
{
  return tcc_in_sys_header();
}

static const char *uu_cur_file(void)
{
  BufferedFile *f = uu_file();
  return f ? tcc_filename_intern(f->filename) : NULL;
}

/* a parse whose locals are not the source's own: an inline expansion, the
   speculative constant evaluation */
static int uu_replay(void)
{
  return tcc_state->in_inline_expansion || tcc_state->fe_speculative;
}

/* Not reported for that declaration yet?  Then it is now.  Nothing is
   recorded while warnings are off for the copy being parsed (a defer
   body's later copies).  The Sym is compared by its name: it may be gone
   when the same declaration comes up in another parse. */
static int uu_first_report(const char *file, int line, Sym *sym, int kind)
{
  const int v = sym ? sym->v : 0;
  if (tcc_state->warn_none)
    return 0;
  /* the unit's report-once set (tcc_diag_once): a declaration is reported
     once per unit */
  return tcc_diag_once(tcc_state, DIAG_ONCE_DECL, (unsigned)kind + (unsigned)v * 0x9E3779B1u, file, line, NULL, 1);
}

static const char *uu_name(Sym *s)
{
  return get_tok_str(s->v & ~SYM_FIELD, NULL);
}

/* ------------------------------------------------------------------------ */

static int uu_any_on(TCCState *s1)
{
  return (s1->warn_unused_variable | s1->warn_unused_const_variable | s1->warn_unused_function |
          s1->warn_unused_but_set_variable | s1->warn_unused_value | s1->warn_unused_label |
          s1->warn_unused_local_typedefs |
          s1->warn_unused_parameter | s1->warn_unused_but_set_parameter) &
         WARN_ON;
}

ST_FUNC void unused_options_changed(TCCState *s1)
{
  if (uu_any_on(s1))
    s1->unused_on = 1;
}

ST_FUNC void unused_unit_free(TCCState *s1)
{
  (void)s1;
  uu_reset(&uu_locals);
  uu_reset(&uu_labels);
  uu_reset(&uu_statics);
  tcc_free(uu_plines);
  tcc_free(uu_plist);
  uu_plines = uu_plist = NULL;
  uu_nb_plines = uu_cap_plines = uu_nb_plist = uu_cap_plist = 0;
  tcc_free(uu_alias_toks);
  uu_alias_toks = NULL;
  uu_nb_alias = uu_cap_alias = 0;
  uu_last_sym = uu_pending = NULL;
  uu_part.s = NULL;
  uu_nb_watch = 0;
}

ST_FUNC void unused_unit_begin(TCCState *s1)
{
  unused_unit_free(s1);
  s1->unused_on = 0;
  unused_options_changed(s1);
  uv_effects = uv_last = 0;
}

/* Do the checks reported at a scope's end want the bodies nothing generated
   parsed (check_uncompiled_inline_functions)? */
ST_FUNC int unused_locals_wanted(void)
{
  return tcc_state->unused_on && !tcc_state->warn_none;
}

/* ------------------------------------------------------------------------ */
/* uses */

/* unary() resolved an identifier to S */
ST_FUNC void unused_mention(Sym *s)
{
  /* a function naming itself: recursion is not a use */
  if (s == tcc_state->cur_func_sym && (s->type.t & VT_BTYPE) == VT_FUNC)
  {
    uu_last_sym = NULL;
    return;
  }
  uu_last_sym = s;
  uu_last_newly_read = !(s->ymod & SYM_UNUSED_READ);
  s->ymod |= SYM_UNUSED_REF | SYM_UNUSED_READ;
  for (int i = 0; i < uu_nb_watch; i++)
    if (uu_part_watch[i] == s)
      uu_part_watch_hit[i] = 1;
}

/* The identifiers of saved tokens STR, as a mention each: a static inline
   body (SELF: its function) when it is saved, a defer body when it is met.
   A name after '.' or '->' is a member.  ANY: mark whatever the name is
   bound to here; else only the tracked Syms (file-scope statics). */
ST_FUNC void unused_scan_tokens(TokenString *str, Sym *self, int any)
{
  if (!str)
    return;
  const int reload = str->allocated_len < 0; /* spilled: see tok_str_release */
  const int *p = tok_str_buf(str), *end = p + str->len;
  int prev = 0;
  while (p < end)
  {
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    if (t == TOK_EOF)
      break;
    if (t >= TOK_UIDENT && prev != '.' && prev != TOK_ARROW)
    {
      Sym *s = sym_find(t);
      if (s && s != self && !IS_ASM_SYM(s) && (any || (s->ymod & SYM_UNUSED_TRACK)))
        s->ymod |= SYM_UNUSED_REF | SYM_UNUSED_READ;
    }
    if (t != TOK_LINENUM)
      prev = t;
  }
  if (reload)
    tok_str_release(str);
}

ST_FUNC void unused_alias_target(int tok_target)
{
  if (!tcc_state->unused_on)
    return;
  if (uu_nb_alias == uu_cap_alias)
  {
    uu_cap_alias = uu_cap_alias ? uu_cap_alias * 2 : 8;
    uu_alias_toks = tcc_realloc(uu_alias_toks, uu_cap_alias * sizeof *uu_alias_toks);
  }
  uu_alias_toks[uu_nb_alias++] = tok_target;
}

/* ------------------------------------------------------------------------ */
/* set but not used */

static int uu_part_is_vtop(void)
{
  return uu_part.s && uu_part.at == vtop && vtop->r == uu_part.sv.r && vtop->vr == uu_part.sv.vr &&
         vtop->c.i == uu_part.sv.c.i && vtop->type.t == uu_part.sv.type.t && vtop->type.ref == uu_part.sv.type.ref &&
         vtop->sym == uu_part.sv.sym;
}

/* vtop is S itself or in it: a local's frame range (an array need not be an lvalue) */
static int uu_vtop_in_local(Sym *s)
{
  int align, size;
  if ((s->r & (VT_VALMASK | VT_SYM)) != VT_LOCAL || (vtop->r & (VT_VALMASK | VT_SYM)) != VT_LOCAL ||
      (!(vtop->r & VT_LVAL) && !(vtop->type.t & VT_ARRAY)))
    return 0;
  size = type_size(&s->type, &align);
  if (size <= 0)
    size = 1;
  return (int64_t)vtop->c.i >= s->c && (int64_t)vtop->c.i < (int64_t)s->c + size;
}

/* A member access or (SUBSCRIPT) a subscript of vtop starts: the tracked
   local it is part of, or NULL; *NEWLY_READ as unused_lhs's */
ST_FUNC Sym *unused_part_begin(unsigned char *newly_read, int subscript)
{
  Sym *s;
  if (!tcc_state->unused_on || uu_replay())
    return NULL;
  if (uu_part_is_vtop())
    s = uu_part.s, *newly_read = uu_part.newly_read;
  else if ((s = uu_last_sym) && (s->ymod & SYM_UNUSED_TRACK) && uu_vtop_in_local(s))
    *newly_read = uu_last_newly_read;
  else
    return NULL;
  if (subscript && !(vtop->type.t & VT_ARRAY))
    return NULL; /* through a pointer: it is read */
  if (subscript)
  {
    if (uu_nb_watch == UU_WATCH_MAX)
      return NULL;
    uu_part_watch[uu_nb_watch] = s;
    uu_part_watch_hit[uu_nb_watch++] = 0;
  }
  return s;
}

/* ... and vtop is the part now (unless a subscript's index named it) */
ST_FUNC void unused_part_end(Sym *s, unsigned char newly_read, int subscript)
{
  if (subscript && uu_nb_watch > 0 && uu_part_watch_hit[--uu_nb_watch])
    return;
  uu_part.s = s;
  uu_part.at = vtop;
  uu_part.sv = *vtop;
  uu_part.newly_read = newly_read;
}

/* `PART = value`: the part of a local unused_part_* followed, taken as set */
ST_FUNC Sym *unused_lhs_assign(void)
{
  Sym *s;
  if (!tcc_state->unused_on || uu_replay() || !uu_part_is_vtop())
    return unused_lhs();
  s = uu_part.s;
  uu_part.s = NULL;
  if (!uu_part.newly_read || !(s->ymod & SYM_UNUSED_TRACK))
    return NULL;
  uu_last_sym = NULL;
  s->ymod &= ~SYM_UNUSED_READ;
  return s;
}

/* The lvalue on vtop is about to be assigned to (or incremented): the
   variable it is, when it is one named just now whose value nothing had read
   before -- the variable itself, a member of it or a constant element, not
   something reached through a pointer.  Its read mark is taken back until the
   right-hand side reads it again (unused_assigned). */
ST_FUNC Sym *unused_lhs(void)
{
  Sym *s = uu_last_sym;
  if (!tcc_state->unused_on || !s || !uu_last_newly_read || !(s->ymod & SYM_UNUSED_TRACK) || uu_replay())
    return NULL;
  if (!(vtop->r & VT_LVAL))
    return NULL;
  if (vtop->sym == s && vtop->c.i == s->c && (vtop->r & (VT_VALMASK | VT_SYM)) == (s->r & (VT_VALMASK | VT_SYM)) &&
      vtop->type.t == s->type.t && vtop->type.ref == s->type.ref)
    ; /* the variable itself (a parameter's value is kept in its own way) */
  else if (s->r & VT_SYM)
  {
    if (!(vtop->r & VT_SYM) || vtop->sym != s)
      return NULL;
  }
  else
  {
    int align, size = type_size(&s->type, &align);
    if ((s->r & VT_VALMASK) != VT_LOCAL || (vtop->r & (VT_VALMASK | VT_SYM)) != VT_LOCAL)
      return NULL;
    if (size <= 0)
      size = 1;
    if ((int64_t)vtop->c.i < s->c || (int64_t)vtop->c.i >= (int64_t)s->c + size) /* c.i is unsigned */
      return NULL;
  }
  uu_last_sym = NULL;
  s->ymod &= ~SYM_UNUSED_READ;
  return s;
}

/* An assignment (or ++/--) to LHS (unused_lhs, or NULL) is complete.  Its
   value may be used (`y = x = 1`, `f(x++)`): LHS is read again, unless the
   value is discarded (unused_discard), or the right-hand side read it. */
ST_FUNC void unused_assigned_to(Sym *lhs)
{
  uv_effects++;
  uv_last = UV_ASSIGN;
  uu_pending = NULL;
  if (lhs && !(lhs->ymod & SYM_UNUSED_READ))
  {
    lhs->ymod |= SYM_UNUSED_READ;
    uu_pending = lhs;
  }
}

ST_FUNC void unused_assigned(void)
{
  unused_assigned_to(NULL);
}

/* (void)EXPR: gcc counts that as a use of an assignment's variable, and it
   is never an unused value */
ST_FUNC void unused_void_cast(void)
{
  uu_pending = NULL;
  uv_last = UV_VALUE;
}

/* the right operand of a comma operator, parsed from EFFECTS_BEFORE_RHS,
   left RHS_LAST in uv_last: the comma is the expression's top now */
ST_FUNC void unused_comma_end(int effects_before_rhs, int rhs_last)
{
  const int bt = vtop->type.t & VT_BTYPE;
  if (bt == VT_VOID || ((vtop->r & VT_LVAL) && ((vtop->type.t & VT_VOLATILE) || vtop->volatile_access)))
  {
    uv_comma_rhs_noeffect = 0;
    uv_comma_rhs_last = UV_EFFECT;
  }
  else
  {
    uv_comma_rhs_noeffect = uv_effects == effects_before_rhs;
    uv_comma_rhs_last = rhs_last;
  }
  uv_last = UV_COMMA;
}

/* ------------------------------------------------------------------------ */
/* -Wunused-value */

static void uv_warn(int line, const char *msg)
{
  const char *f = uu_cur_file();
  if (uu_first_report(f, line, (Sym *)NULL, 100 + (msg[0] == 'v' ? 0 : msg[0] == 's' ? 1 : msg[0] == 'l' ? 2 : 3)))
    tcc_warning_c(warn_unused_value)("%i:%s", line, msg);
}

/* The value on vtop, of an expression parsed from EFFECTS_BEFORE at LINE,
   is discarded: an expression statement, a for clause, or (COMMA_LHS) the
   left operand of a comma. */
ST_FUNC void unused_discard(int effects_before, int line, int comma_lhs)
{
  const int last = uv_last;
  if (!tcc_state->unused_on || uu_replay())
    return;
  /* set but not used: the value of the assignment goes nowhere */
  if (uu_pending && (last == UV_ASSIGN || last == UV_INCDEC ||
                     (last == UV_COMMA && (uv_comma_rhs_last == UV_ASSIGN || uv_comma_rhs_last == UV_INCDEC))))
    uu_pending->ymod &= ~SYM_UNUSED_READ;
  uu_pending = NULL;
  if (!(tcc_state->warn_unused_value & WARN_ON) && !tcc_diag_snaps_on(warn_unused_value))
    return;
  if ((vtop->type.t & VT_BTYPE) == VT_VOID)
    return;
  if ((vtop->r & VT_LVAL) && ((vtop->type.t & VT_VOLATILE) || vtop->volatile_access))
    return; /* a volatile read is the effect */
  if (uu_in_sys())
    return;
  if (!comma_lhs && last == UV_COMMA)
  {
    if (uv_comma_rhs_noeffect)
      uv_warn(line, "right-hand operand of comma expression has no effect");
    else if (uv_comma_rhs_last == UV_VALUE)
      uv_warn(line, "value computed is not used");
    return;
  }
  if (uv_effects == effects_before)
    uv_warn(line, comma_lhs ? "left-hand operand of comma expression has no effect" : "statement with no effect");
  else if (last == UV_VALUE)
    uv_warn(line, "value computed is not used");
}

/* A statement of a statement expression `({ a; b; })`: its value stays on
 * the stack in case it is the last one.  Kept here with what the parser knew
 * at its end, and checked as a discarded value when another statement
 * follows (unused_stmt_expr_drop). */
static struct
{
  SValue *at;
  int effects, effects_after, line, last;
  Sym *pending;
  unsigned char comma_noeffect, comma_last;
} uu_se;

ST_FUNC void unused_stmt_expr_end(int effects_before, int line)
{
  uu_se.at = NULL;
  if (!tcc_state->unused_on || uu_replay())
    return;
  uu_se.at = vtop;
  uu_se.effects = effects_before;
  uu_se.effects_after = uv_effects;
  uu_se.line = line;
  uu_se.last = uv_last;
  uu_se.pending = uu_pending;
  uu_se.comma_noeffect = uv_comma_rhs_noeffect;
  uu_se.comma_last = uv_comma_rhs_last;
}

/* vtop, a statement expression's statement other than its last, is dropped */
ST_FUNC void unused_stmt_expr_drop(void)
{
  int effects = uv_effects, last = uv_last;
  unsigned char cn = uv_comma_rhs_noeffect, cl = uv_comma_rhs_last;
  if (!uu_se.at || uu_se.at != vtop)
    return;
  uu_se.at = NULL;
  uv_effects = uu_se.effects_after;
  uv_last = uu_se.last;
  uv_comma_rhs_noeffect = uu_se.comma_noeffect;
  uv_comma_rhs_last = uu_se.comma_last;
  uu_pending = uu_se.pending;
  unused_discard(uu_se.effects, uu_se.line, 0);
  uv_effects = effects, uv_last = last;
  uv_comma_rhs_noeffect = cn, uv_comma_rhs_last = cl;
}

/* ------------------------------------------------------------------------ */
/* declarations */

/* the local S (a variable declared at LINE) is followed to its scope's end */
ST_FUNC void unused_declare_local(Sym *s, int line, int attr_unused)
{
  if (!tcc_state->unused_on || uu_replay() || !s || s->v >= SYM_FIRST_ANOM)
    return;
  if (attr_unused)
  {
    s->ymod |= SYM_UNUSED_OK;
    return;
  }
  if (s->ymod & SYM_UNUSED_TRACK || uu_in_sys())
    return;
  s->ymod |= SYM_UNUSED_TRACK;
  uu_add(&uu_locals, s, uu_cur_file(), line, (s->r & VT_SYM) ? UK_STATIC_LOCAL : UK_LOCAL);
}

static void uu_pline_add(UnusedParamLine **v, int *nb, int *cap, Sym *func, int tokv, int line, int sys)
{
  if (*nb == *cap)
  {
    *cap = *cap ? *cap * 2 : 16;
    *v = tcc_realloc(*v, *cap * sizeof **v);
  }
  (*v)[*nb].func = func;
  (*v)[*nb].v = tokv;
  (*v)[*nb].line = line;
  (*v)[*nb].sys = sys;
  (*nb)++;
}

/* post_type parsed parameter V of a file-scope declarator at LINE; FIRST:
   the list's first */
ST_FUNC void unused_param_line(int v, int line, int first, int sys)
{
  if (first)
    uu_nb_plist = 0;
  uu_pline_add(&uu_plist, &uu_nb_plist, &uu_cap_plist, NULL, v & ~SYM_FIELD, line, sys);
}

/* the body of FUNC follows its declarator: its parameters' lines are those
   of the list parsed last */
ST_FUNC void unused_params_bind(Sym *func)
{
  if (!tcc_state->unused_on && !tcc_warn_wanted(warn_shadow))
    return;
  for (int i = 0; i < uu_nb_plist; i++)
    uu_pline_add(&uu_plines, &uu_nb_plines, &uu_cap_plines, func, uu_plist[i].v, uu_plist[i].line,
                 uu_plist[i].sys);
  uu_nb_plist = 0;
}

/* The line parameter V of FUNC was declared at (unused_param_line), or 0 */
ST_FUNC int unused_param_decl_line(Sym *func, int v)
{
  for (int i = uu_nb_plines; i-- > 0;)
    if (uu_plines[i].func == func && uu_plines[i].v == v)
      return uu_plines[i].line;
  return 0;
}

/* Parameter V of FUNC was named by a system header's macro (-Wshadow) */
ST_FUNC int unused_param_decl_sys(Sym *func, int v)
{
  for (int i = uu_nb_plines; i-- > 0;)
    if (uu_plines[i].func == func && uu_plines[i].v == v)
      return uu_plines[i].sys;
  return 0;
}

/* The parameters of FUNC were pushed on local_stack, above PARAMS_END:
   followed to the function's end. */
ST_FUNC void unused_params_begin(Sym *func, Sym *params_end)
{
  const char *f;
  int fallback;
  if (!tcc_state->unused_on || uu_replay() || uu_in_sys() || !func || !func->type.ref)
    return;
  f = uu_cur_file();
  fallback = file ? file->line_num : 0;
  for (Sym *s = local_stack; s && s != params_end; s = s->prev)
  {
    int v = s->v, line = fallback;
    if ((v & SYM_FIELD) || v >= SYM_FIRST_ANOM || v < TOK_UIDENT || (s->ymod & SYM_UNUSED_TRACK))
      continue;
    Sym *p;
    for (p = sym_first(func->type.ref); p && (p->v & ~SYM_FIELD) != v; p = p->next)
      ;
    if (!p)
      continue;
    if (p->ymod & SYM_UNUSED_OK)
      continue;
    for (int i = uu_nb_plines; i-- > 0;)
      if (uu_plines[i].func == func && uu_plines[i].v == v)
      {
        line = uu_plines[i].line;
        break;
      }
    s->ymod |= SYM_UNUSED_TRACK;
    uu_add(&uu_locals, s, f, line, UK_PARAM);
  }
}

/* sym_pop is freeing (or keeping past its scope) the tracked local S */
ST_FUNC void unused_sym_popped(Sym *s)
{
  int i, kind, line;
  const char *f;
  s->ymod &= ~SYM_UNUSED_TRACK;
  if (uu_last_sym == s)
    uu_last_sym = NULL;
  if (uu_pending == s)
    uu_pending = NULL;
  for (i = uu_locals.nb; i-- > 0;)
    if (uu_locals.r[i].sym == s)
      break;
  if (i < 0)
    return;
  kind = uu_locals.r[i].kind;
  line = uu_locals.r[i].line;
  f = uu_locals.r[i].file;
  memmove(&uu_locals.r[i], &uu_locals.r[i + 1], (uu_locals.nb - i - 1) * sizeof *uu_locals.r);
  uu_locals.nb--;
  if (s->ymod & SYM_UNUSED_OK)
    return;
  if (kind == UK_TYPEDEF)
  {
    if (!(s->ymod & SYM_UNUSED_REF) && uu_first_report(f, line, s, kind))
      tcc_warning_c(warn_unused_local_typedefs)("%i:typedef '%s' locally defined but not used", line, uu_name(s));
    return;
  }
  if (!(s->ymod & SYM_UNUSED_REF))
  {
    if (!uu_first_report(f, line, s, kind))
      return;
    if (kind == UK_PARAM)
      tcc_warning_c(warn_unused_parameter)("%i:unused parameter '%s'", line, uu_name(s));
    else
      tcc_warning_c(warn_unused_variable)("%i:unused variable '%s'", line, uu_name(s));
  }
  else if (!(s->ymod & SYM_UNUSED_READ))
  {
    if (!uu_first_report(f, line, s, kind))
      return;
    if (kind == UK_PARAM)
      tcc_warning_c(warn_unused_but_set_parameter)("%i:parameter '%s' set but not used", line, uu_name(s));
    else
      tcc_warning_c(warn_unused_but_set_variable)("%i:variable '%s' set but not used", line, uu_name(s));
  }
}

/* -Wunused-local-typedefs: the typedef S declared in a block at LINE */
ST_FUNC void unused_declare_typedef(Sym *s, int line, int attr_unused)
{
  if (!tcc_state->unused_on || uu_replay() || !s || (s->ymod & SYM_UNUSED_TRACK) || uu_in_sys() ||
      s->v >= SYM_FIRST_ANOM)
    return;
  if (attr_unused)
  {
    s->ymod |= SYM_UNUSED_OK;
    return;
  }
  s->ymod |= SYM_UNUSED_TRACK;
  uu_add(&uu_locals, s, uu_cur_file(), line, UK_TYPEDEF);
}

/* the file-scope static S, a function when IS_FUNC, is defined at LINE */
ST_FUNC void unused_declare_static(Sym *s, int line, int is_func)
{
  if (!tcc_state->unused_on || !s || (s->ymod & SYM_UNUSED_TRACK) || uu_in_sys())
    return;
  s->ymod |= SYM_UNUSED_TRACK;
  uu_add(&uu_statics, s, uu_cur_file(), line, is_func ? UK_FUNC : UK_VAR);
}

/* the label S was defined here */
ST_FUNC void unused_label_defined(Sym *s, int line, int attr_unused)
{
  if (!tcc_state->unused_on || uu_replay() || !s)
    return;
  if (attr_unused)
  {
    s->ymod |= SYM_UNUSED_OK;
    return;
  }
  if ((s->ymod & SYM_UNUSED_TRACK) || uu_in_sys())
    return;
  s->ymod |= SYM_UNUSED_TRACK;
  uu_add(&uu_labels, s, uu_cur_file(), line, UK_LABEL);
}

ST_FUNC void unused_label_popped(Sym *s)
{
  int i;
  s->ymod &= ~SYM_UNUSED_TRACK;
  for (i = uu_labels.nb; i-- > 0;)
    if (uu_labels.r[i].sym == s)
      break;
  if (i < 0)
    return;
  const int line = uu_labels.r[i].line;
  const char *f = uu_labels.r[i].file;
  uu_labels.r[i] = uu_labels.r[--uu_labels.nb];
  if (s->ymod & (SYM_UNUSED_REF | SYM_UNUSED_OK) || s->c == -3 || s->a.addrtaken)
    return;
  if (uu_first_report(f, line, s, UK_LABEL))
    tcc_warning_c(warn_unused_label)("%i:label '%s' defined but not used", line, uu_name(s));
}

/* The end of the unit: the file-scope statics nothing named. */
ST_FUNC void unused_unit_end(TCCState *s1)
{
  int opened = 0;
  if (!s1->unused_on)
    return;
  for (int i = 0; i < uu_nb_alias; i++)
  {
    Sym *t = sym_find(uu_alias_toks[i]);
    if (t)
      t->ymod |= SYM_UNUSED_REF;
    /* alias("name") of a static's asm label */
    for (int j = 0; j < uu_statics.nb; j++)
      if (uu_statics.r[j].sym->asm_label == uu_alias_toks[i])
        uu_statics.r[j].sym->ymod |= SYM_UNUSED_REF;
  }
  for (int i = 0; i < uu_statics.nb; i++)
  {
    UnusedRec *r = &uu_statics.r[i];
    Sym *s = r->sym;
    if ((s->ymod & (SYM_UNUSED_REF | SYM_UNUSED_OK)) || s->a.used)
      continue;
    if (r->kind == UK_FUNC)
    {
      if ((s->type.t & VT_INLINE) || !s->type.ref || s->type.ref->f.func_ctor || s->type.ref->f.func_dtor ||
          yasc_method_name_of(s->v))
        continue;
    }
    else
    {
      if (s->type.t & VT_VOLATILE)
        continue;
      /* gcc's -Wunused-const-variable=1: of the main file only */
      if ((s->type.t & VT_CONSTANT) && r->file != s1->main_file)
        continue;
    }
    if (!opened)
    {
      tcc_open_bf(s1, ":unused:", 0);
      opened = 1;
    }
    tccpp_setfile_replay(r->file);
    if (!uu_first_report(r->file, r->line, s, r->kind))
      continue;
    if (r->kind == UK_FUNC)
      tcc_warning_c(warn_unused_function)("%i:'%s' defined but not used", r->line, uu_name(s));
    else if (s->type.t & VT_CONSTANT)
      tcc_warning_c(warn_unused_const_variable)("%i:'%s' defined but not used", r->line, uu_name(s));
    else
      tcc_warning_c(warn_unused_variable)("%i:'%s' defined but not used", r->line, uu_name(s));
  }
  if (opened)
    tcc_close();
}
