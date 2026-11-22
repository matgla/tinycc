/*
 *  TCC - Tiny C Compiler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* recover.c -- error recovery: an error goes on to the next external
 * declaration, or to the next statement of a function body, so a file
 * reports more than its first mistake (gcc's -fmax-errors=N, here 8 by
 * default).
 *
 * Two recovery points, each a setjmp: one around the file-scope declaration
 * loop (tccgen_decls_recovering), one around the parse of each function body
 * (gen_body_recovering, from gen_function and gen_function_check_only).  A
 * tcc_error longjmps to the innermost armed one (error1 -> tcc_recover_jump)
 * when the state allows; otherwise, as before, out of the compile:
 *
 *   - inside a preprocessor directive, a macro's substitution or inline asm,
 *     after an internal error, and in const_eval's speculative parse;
 *   - while a function's parameters are set up or its code generated (no
 *     point is armed there);
 *   - when a recovery would not move (the same token twice), and at the end
 *     of the input inside a body.
 *
 * At file scope the declaration is skipped to its ';' (out of the struct
 * bodies it is in), or past the braces of a function body.  In a body the
 * statement is skipped to its ';' or its closing brace (out of statement
 * expressions and struct bodies), and the rest of the body is parsed again
 * statement by statement, the compound statements still open closed as their
 * '}' come -- with the locals they declared before the error.  Replays an
 * error left (an inline expansion, a defer body) are unwound first.
 *
 * A body with an error gets no code and no flow checks; from the first
 * error on, the rest of the unit compiles as -fsyntax-only (no output is
 * written anyway).  The names in skipped tokens count as used (-Wunused),
 * and labels a skipped part would have defined are not reported.  Nothing
 * here runs while a compile has no error: arming costs a setjmp per body.
 */

#include "gen_priv.h"

typedef struct TCCRecover TCCRecover;

/* the kinds of recovery points */
enum
{
  RECOVER_FILE,   /* the file-scope declaration loop */
  RECOVER_BODY,   /* a function body */
  RECOVER_STRUCT, /* a struct or union body: the next member */
  RECOVER_INLINE, /* a replay (an inline expansion, a defer body, a saved
                     initializer): after it */
};

typedef struct RecoverLevel
{
  Sym *lstk, *llstk;
  int level;
  unsigned char kind;
} RecoverLevel;

struct TCCRecover
{
  jmp_buf jb;
  struct TCCRecover *outer;
  unsigned char body;    /* RECOVER_FILE, RECOVER_BODY, RECOVER_STRUCT or RECOVER_INLINE */
  unsigned char errored; /* an error was recovered from here */
  unsigned char in_loop, in_switch, can_break;
  int nb_errors;         /* when armed */
  /* the state when armed */
  TokenString *mstack;
  SValue *vtop;
  int nocode_wanted, local_scope, unevaluated_operand, global_expr, func_param_decl_depth;
  int compound_depth, struct_depth, ym_depth, defer_level, cleanup_error_exit, check_only, parse_flags;
  int nb_stk_data, hidden_labels, inline_return_loc, inline_return_vr, rsym, func_var, func_has_label_addr;
  unsigned pending_call_temp_slots, diag_parse;
  Sym *all_cleanups, *lt_loop, *lt_switch;
  struct scope *cur_scope, *loop_scope, *root_scope;
  struct switch_t *cur_switch;
  CType func_vt;
  const char *funcname, *diag_replay_file;
  TokenString *compile_replay;
  unsigned char yasc, in_inline_expansion, inline_expansion_depth, diag_flow_only, warn_none;
  unsigned char inline_return_redirected, inline_return_copied;
  int pack_depth, pack[PACK_STACK_SIZE];
  /* recorded when the error is raised (body: the compound statements open) */
  RecoverLevel *lev;
  int nlev, keep, closes, in_parens;
  /* progress: where the last recovery from here was */
  const void *last_buf, *last_macro;
  int last_tok, last_line, recoveries, same_place;
  /* the resumed parse's scopes and the stand-ins for a loop and a switch */
  struct scope *scopes;
  int dummy_b, dummy_c;
  struct switch_t *dummy_sw;
};

/* ------------------------------------------------------------------------- */
/* state */

RecoverDecl recover_decl;

static void recover_body_errored(TCCRecover *r);
static void recover_syntax_only(TCCState *s1);

static void recover_save(TCCRecover *r, int body)
{
  TCCState *s1 = tcc_state;
  memset(r, 0, sizeof *r);
  r->body = body;
  r->nb_errors = s1->nb_errors;
  r->mstack = tok_macro_stack();
  r->vtop = vtop;
  r->nocode_wanted = nocode_wanted;
  r->local_scope = local_scope;
  r->unevaluated_operand = unevaluated_operand;
  r->global_expr = global_expr;
  r->func_param_decl_depth = func_param_decl_depth;
  r->struct_depth = struct_body_depth;
  r->ym_depth = yasc_methods_depth(-1);
  r->defer_level = defer_body_level;
  r->cleanup_error_exit = cleanup_error_exit;
  r->check_only = s1->check_only;
  r->parse_flags = parse_flags;
  r->nb_stk_data = nb_stk_data;
  r->hidden_labels = inline_hidden_label_depth();
  r->pending_call_temp_slots = pending_call_temp_slots;
  r->diag_parse = s1->diag_parse;
  r->all_cleanups = all_cleanups;
  block_recover_get(&r->lt_loop, &r->lt_switch, &r->compound_depth);
  r->cur_scope = cur_scope;
  r->loop_scope = loop_scope;
  r->root_scope = root_scope;
  r->cur_switch = cur_switch;
  r->func_vt = func_vt;
  r->func_var = func_var;
  r->func_has_label_addr = func_has_label_addr;
  r->rsym = rsym;
  r->funcname = funcname;
  r->diag_replay_file = s1->diag_replay_file;
  r->compile_replay = s1->compile_replay;
  r->yasc = s1->yasc;
  r->in_inline_expansion = s1->in_inline_expansion;
  r->inline_expansion_depth = s1->inline_expansion_depth;
  r->diag_flow_only = s1->diag_flow_only;
  r->warn_none = s1->warn_none;
  r->inline_return_loc = s1->inline_return_loc;
  r->inline_return_vr = s1->inline_return_vr;
  r->inline_return_redirected = s1->inline_return_redirected;
  r->inline_return_copied = s1->inline_return_copied;
  r->pack_depth = s1->pack_stack_ptr - s1->pack_stack;
  memcpy(r->pack, s1->pack_stack, sizeof r->pack);
}

/* What the frames the longjmp left would have put back on their way out. */
static void recover_restore(TCCRecover *r)
{
  TCCState *s1 = tcc_state;
  while (nb_stk_data > r->nb_stk_data)
    tcc_free(*(void **)stk_data[--nb_stk_data]);
  vtop = r->vtop;
  nocode_wanted = r->nocode_wanted;
  unevaluated_operand = r->unevaluated_operand;
  global_expr = r->global_expr;
  func_param_decl_depth = r->func_param_decl_depth;
  struct_body_depth = r->struct_depth;
  yasc_methods_depth(r->ym_depth);
  if (defer_body_level != r->defer_level)
    s1->warn_none = r->warn_none; /* a defer body's later copies are quiet */
  defer_body_level = r->defer_level;
  cleanup_error_exit = r->cleanup_error_exit;
  s1->check_only = r->check_only;
  parse_flags = r->parse_flags;
  pending_call_temp_slots = r->pending_call_temp_slots;
  s1->diag_parse = r->diag_parse; /* an inline expansion's parse the error left */
  inline_unwind_hidden_labels(r->hidden_labels);
  block_recover_set(r->lt_loop, r->lt_switch, r->compound_depth);
  while (cur_switch && cur_switch != r->cur_switch)
    end_switch(); /* the resumed parse's stand-in too */
  r->dummy_sw = NULL;
  cur_scope = r->cur_scope;
  loop_scope = r->loop_scope;
  root_scope = r->root_scope;
  func_vt = r->func_vt;
  func_var = r->func_var;
  func_has_label_addr = r->func_has_label_addr;
  rsym = r->rsym;
  funcname = r->funcname;
  s1->diag_replay_file = r->diag_replay_file;
  s1->compile_replay = r->compile_replay;
  s1->yasc = r->yasc;
  s1->in_inline_expansion = r->in_inline_expansion;
  s1->inline_expansion_depth = r->inline_expansion_depth;
  s1->diag_flow_only = r->diag_flow_only;
  s1->inline_return_loc = r->inline_return_loc;
  s1->inline_return_vr = r->inline_return_vr;
  s1->inline_return_redirected = r->inline_return_redirected;
  s1->inline_return_copied = r->inline_return_copied;
  memcpy(s1->pack_stack, r->pack, sizeof r->pack);
  s1->pack_stack_ptr = s1->pack_stack + r->pack_depth;
  discarded_call_vtop = NULL;
  unary_addr_operand = 0;
  indir_keep_lvalue = 0;
  yasc_returning = yasc_argument = 0;
  fe_compound_pending = 0;
  funcall_self_pending = 0;
  expr_eq_assigned = 0;
  yasc_sbind_pending = 0;
  yasc_plain_reset();
  yasc_pp_recover(r->body);
  attr_checks_recover();
  tcc_free(r->scopes);
  r->scopes = NULL;
  /* the defer bodies of the scopes the error left: one never replayed (its
     scope's exits were after the error) is checked once now, where the
     names it sees are still there -- a function body's own, not a replayed
     one's (the callee's compile checks those) */
  for (Sym *cls = all_cleanups; cls && cls != r->all_cleanups; cls = cls->prev)
    if (cls->type.t && cls->defer_str)
    {
      if (!cls->vreg && r->body == RECOVER_BODY)
      {
        recover_syntax_only(s1);
        s1->check_only++;
        nocode_wanted++;
        defer_replay(cls);
        nocode_wanted--;
        s1->check_only--;
      }
      tok_str_free(cls->defer_str), cls->defer_str = NULL;
    }
  sym_pop(&all_cleanups, r->all_cleanups, 0);
}

/* The rest of the unit is checked, not compiled: no object is written. */
static void recover_syntax_only(TCCState *s1)
{
  if (s1->recover_flipped)
    return;
  s1->recover_syntax_only = s1->syntax_only;
  s1->syntax_only = 1;
  s1->recover_flipped = 1;
  tcc_check_scratch_begin(s1);
}

/* ------------------------------------------------------------------------- */
/* raising */

/* The compile ends (an error not recovered from): what the armed points
   hold, while their frames are still there. */
ST_FUNC void tcc_recover_release(TCCState *s1)
{
  for (TCCRecover *r = s1->recover; r; r = r->outer)
  {
    tcc_free(r->lev);
    tcc_free(r->scopes);
    r->lev = NULL;
    r->scopes = NULL;
  }
  s1->recover = NULL;
}

/* error1 is raising an error that may be recovered from: unwind what the
   frames above the recovery point started and go there.  Returns when this
   one cannot be recovered from. */
ST_FUNC void tcc_recover_jump(TCCState *s1)
{
  TCCRecover *r = s1->recover;
  TokenString *m, *inj = NULL;
  unsigned inj_serial = ~0u;

  /* the end of the input in a struct body: the outer point says what is left */
  while (r && r->body == RECOVER_STRUCT && tok == TOK_EOF)
    r = r->outer;
  if (!r)
    return;
  /* the replays above the recovery point: a borrowed body (an inline
     expansion, a defer, a cleanup) is no part of the stream, macros are */
  for (m = tok_macro_stack(); m && m != r->mstack; m = m->prev)
    if (m->alloc >= 2)
      inj = m;
  if (m != r->mstack)
    return;
  if (inj)
  {
    inj_serial = inj->serial;
    recover_decl.l = 0; /* a declarator of the replayed body */
  }

  if (r->body == RECOVER_STRUCT || r->body == RECOVER_INLINE)
  {
    r->closes = 0;
    r->in_parens = 0;
  }
  else if (r->body)
  {
    struct scope *sc;
    int n = 0, i, inner_kind = -1, stmt_expr = -1;
    /* the scopes of the body the file's braces opened: outermost first */
    for (sc = cur_scope; sc && sc != r->root_scope; sc = sc->prev)
      if (sc->mserial < inj_serial && (sc->kind & (SCOPE_BRACE | SCOPE_STMT_EXPR)))
        n++;
    if (!sc || !n)
      return; /* not inside the body's braces */
    tcc_free(r->lev);
    r->lev = tcc_malloc(n * sizeof *r->lev);
    r->nlev = n;
    i = n;
    for (sc = cur_scope; sc != r->root_scope; sc = sc->prev)
    {
      if (sc->mserial >= inj_serial)
        continue;
      if (inner_kind < 0)
        inner_kind = sc->kind;
      if (!(sc->kind & (SCOPE_BRACE | SCOPE_STMT_EXPR)))
        continue;
      --i;
      r->lev[i].lstk = sc->lstk;
      r->lev[i].llstk = sc->llstk;
      r->lev[i].level = sc->level;
      r->lev[i].kind = sc->kind;
      if (sc->kind & SCOPE_STMT_EXPR)
        stmt_expr = i;
    }
    /* a statement expression is part of its statement */
    r->keep = stmt_expr >= 0 ? stmt_expr : n;
    if (!r->keep)
      return;
    r->closes = n - r->keep + (inj ? 0 : struct_body_depth - r->struct_depth);
    r->in_parens = r->keep == n && (inner_kind & (SCOPE_FOR | SCOPE_FOR_BODY)) == SCOPE_FOR;
    r->in_loop = loop_scope != r->loop_scope;
    r->can_break = r->in_loop || (cur_scope && cur_scope->bsym);
    r->in_switch = cur_switch != r->cur_switch;
  }
  else
  {
    r->closes = inj ? 0 : struct_body_depth - r->struct_depth;
    r->in_parens = 0;
  }

  /* the expansions the error is in are not expanded again */
  for (int d = r->inline_expansion_depth; d < s1->inline_expansion_depth && d < INLINE_NEST_MAX; d++)
  {
    Sym *f = s1->inline_expansion_syms[d];
    if (f && f->type.ref)
    {
      f->type.ref->f.func_noinline = 1;
      f->type.ref->f.func_auto_inline = 0;
      f->type.ref->f.func_eval_only_inline = 0;
      f->type.ref->f.func_small_leaf = 0;
    }
  }

  /* unwind the replays: the parser goes on where the outermost began */
  if (inj)
  {
    int t = inj->save_tok;
    while (tok_macro_stack() != inj)
      end_macro();
    end_macro();
    tok = TOK_HAS_VALUE(t) ? TOK_CINT : t;
    tokc.i = 0;
  }

  /* a recovery that would not move: give up -- the same token, or many in a
     row at one place of the file (replayed tokens, a macro's expansion) */
  {
    const void *buf = file ? (const void *)file->buf_ptr : NULL;
    const void *mp = macro_ptr;
    const int line = file ? file->line_num : 0;
    if (r->recoveries && buf == r->last_buf && mp == r->last_macro && tok == r->last_tok && line == r->last_line)
      return;
    if (r->recoveries && buf == r->last_buf && line == r->last_line)
    {
      if (++r->same_place > 32)
        return;
    }
    else
      r->same_place = 0;
    if (tok == TOK_EOF && r->body == RECOVER_BODY)
      return; /* the end of the input inside a body */
    r->last_buf = buf;
    r->last_macro = mp;
    r->last_tok = tok;
    r->last_line = line;
    r->recoveries++;
  }
  s1->recover = r;
  longjmp(r->jb, 1);
}

/* "'x' undeclared": once per function, as gcc ("reported only once for
   each function it appears in"); a later use only skips its statement. */
ST_FUNC void tcc_undeclared(int t, const char *name)
{
  TCCState *s1 = tcc_state;
  if (s1->recover && !s1->fe_speculative)
  {
    /* the function: a struct body's or an inline expansion's point is in it */
    TCCRecover *f = s1->recover;
    while (f->outer && f->body >= RECOVER_STRUCT)
      f = f->outer;
    if (s1->undecl_func != f)
    {
      s1->undecl_func = f;
      s1->nb_undecl_toks = 0;
    }
    for (int i = 0; i < s1->nb_undecl_toks; i++)
      if (s1->undecl_toks[i] == t)
        tcc_recover_jump(s1); /* silently: it was said */
    if (s1->nb_undecl_toks == s1->undecl_cap)
    {
      s1->undecl_cap = s1->undecl_cap ? 2 * s1->undecl_cap : 8;
      s1->undecl_toks = tcc_realloc(s1->undecl_toks, s1->undecl_cap * sizeof(int));
    }
    s1->undecl_toks[s1->nb_undecl_toks++] = t;
  }
  tcc_error("'%s' undeclared", name);
}

/* ------------------------------------------------------------------------- */
/* skipping */

/* a name in skipped tokens: whatever it names counts as used */
static void recover_mention(int t, int prev)
{
  if (t >= TOK_UIDENT && prev != '.' && prev != TOK_ARROW && tcc_state->unused_on)
  {
    Sym *s = sym_find(t);
    if (s && !IS_ASM_SYM(s))
      s->ymod |= SYM_UNUSED_REF | SYM_UNUSED_READ;
  }
}

/* The end of a replay the parser began above the recovery point (an
   unsized array's initializer, parsed twice from a copy): it is no end of
   the input -- the tokens after it come from what it was begun in. */
static int recover_leave_replay(TCCRecover *r)
{
  TokenString *m = tok_macro_stack();
  if (tok != TOK_EOF || !m || m == r->mstack)
    return 0;
  for (TokenString *k = m->prev; k != r->mstack; k = k->prev)
    if (!k)
      return 0;
  end_macro();
  next();
  return 1;
}

/* past the balanced braces tok opens */
static void recover_skip_braces(void)
{
  int depth = 0, prev = 0;
  do
  {
    if (tok == '{')
      depth++;
    else if (tok == '}')
      depth--;
    recover_mention(tok, prev);
    prev = tok;
    next();
  } while (depth > 0 && tok != TOK_EOF);
}

/* File scope: to the end of the declaration -- its ';' out of the struct
   bodies it is in, or past a function body. */
static void recover_skip_decl(TCCRecover *r)
{
  int depth = 0, paren = 0, prev = 0;
  for (;;)
  {
    if (recover_leave_replay(r))
      continue;
    const int t = tok;
    if (t == TOK_EOF)
      return;
    if (t == '(' || t == '[')
      paren++;
    else if (t == ')' || t == ']')
      paren -= paren > 0;
    else if (t == '{' && paren == 0 && prev == ')')
    {
      /* a function body: the declaration ends with it at file scope */
      recover_skip_braces();
      if (depth <= -r->closes)
        return;
      prev = '}';
      continue;
    }
    else if (t == '{')
      depth++;
    else if (t == '}')
      depth--;
    else if (t == ';' && paren == 0 && depth <= -r->closes)
    {
      next();
      return;
    }
    recover_mention(t, prev);
    prev = t;
    next();
  }
}

/* In a body: to the end of the statement -- its ';' or the '}' of the block
   it is, out of the statement expressions and struct bodies it is in.
   A '}' closing the innermost compound statement left open (the statement
   was its last) is left for the caller. */
static void recover_skip_stmt(TCCRecover *r)
{
  int depth = 0, paren = r->in_parens, prev = 0, closes = r->closes, entered = !closes;
  int in_init = 0; /* the error was in an initializer's braces: no block ends */
  for (;;)
  {
    if (recover_leave_replay(r))
      continue;
    const int t = tok;
    if (t == TOK_EOF)
      return;
    recover_mention(t, prev);
    prev = t;
    if (t == '(' || t == '[')
      paren++;
    else if (t == ')' || t == ']')
      paren -= paren > 0;
    else if (t == '{')
      depth++;
    else if (t == '}' && depth > -closes)
    {
      if (--depth == -closes)
      {
        if (!entered)
          entered = 1; /* out of what the error was in */
        else if (!in_init)
        {
          /* a block statement ends here, with its else if it has one */
          next();
          if (tok != TOK_ELSE)
            return;
          continue;
        }
      }
    }
    else if (t == '}')
    {
      /* an initializer's (then ';' or ',' follow: the brace the error was
         in is closed, the statement goes on at this depth) or the
         compound's */
      next();
      if (tok == ';' || tok == ',')
      {
        entered = in_init = 1;
        continue;
      }
      unget_tok('}');
      return;
    }
    else if (t == ';' && paren == 0 && entered && depth == -closes)
    {
      next();
      return;
    }
    next();
  }
}

/* In a struct body: to the end of the member declaration -- its ';', or
   the '}' that ends the body (left for the caller). */
static void recover_skip_member(TCCRecover *r)
{
  int depth = 0, paren = 0, prev = 0;
  for (;;)
  {
    if (recover_leave_replay(r))
      continue;
    const int t = tok;
    if (t == TOK_EOF)
      return;
    if (t == '(' || t == '[')
      paren++;
    else if (t == ')' || t == ']')
      paren -= paren > 0;
    else if (t == '{')
      depth++;
    else if (t == '}' && --depth < 0)
      return;
    else if (t == ';' && paren == 0 && depth == 0)
    {
      next();
      return;
    }
    recover_mention(t, prev);
    prev = t;
    next();
  }
}

/* ------------------------------------------------------------------------- */
/* the declarator an error was in */

/* `int y[ ;`: its name is declared as far as it was read, so the uses after
   it are not undeclared (gcc declares what it can) -- a function (old-style)
   when '(' followed the name, an array when '[' did, else an
   object of the type before the error; a typedef stays a typedef.  l is the
   kind of the recovery point (VT_CONST: file scope, VT_LOCAL: a body, at the
   scope the declaration was in).  Nothing when the name is declared there. */
static void recover_declare_pending(int l)
{
  RecoverDecl d = recover_decl;
  AttributeDef ad;
  CType t;
  Sym *s;
  int ext;
  recover_decl.l = 0;
  if (d.l != l || !d.v || (l == VT_LOCAL && d.scope != local_scope))
    return;
  s = sym_find(d.v);
  if (s && s->sym_scope == (l == VT_CONST ? 0 : d.scope))
    return;
  memset(&ad, 0, sizeof ad);
  ext = !(d.storage & VT_TYPEDEF) && (l == VT_CONST || d.func == 1 || (d.storage & VT_EXTERN));
  t = d.type;
  t.t &= ~VT_STORAGE;
  if (d.func == 1)
  {
    s = sym_push(SYM_FIELD, &t, 0, 0);
    s->f.func_type = FUNC_OLD;
    t.t = VT_FUNC;
    t.ref = s;
  }
  else if (d.func == 2)
  {
    /* a later definition completes an extern; a typedef's or a local's
       size is never asked of the parse after an error */
    s = sym_push(SYM_FIELD, &t, 0, ext ? -1 : 1);
    t.t = VT_ARRAY | VT_PTR;
    t.ref = s;
  }
  if (d.storage & VT_TYPEDEF)
  {
    t.t |= VT_TYPEDEF;
    s = sym_push(d.v, &t, 0, 0);
  }
  else if (ext)
  {
    t.t |= VT_EXTERN | (l == VT_CONST ? d.storage & VT_STATIC : 0);
    s = external_sym(d.v, &t, 0, &ad);
  }
  else
    s = sym_push(d.v, &t, VT_LOCAL | VT_LVAL, 0);
  s->ymod |= SYM_UNUSED_REF | SYM_UNUSED_READ;
}

/* ------------------------------------------------------------------------- */
/* the file-scope loop */

ST_FUNC void tccgen_decls_recovering(TCCState *s1)
{
  TCCRecover r;
  recover_save(&r, RECOVER_FILE);
  r.outer = s1->recover;
  s1->recover = &r;
  if (setjmp(r.jb))
  {
    TCCRecover *rp = s1->recover; /* &r, read back from memory */
    recover_restore(rp);
    sym_pop(&local_stack, NULL, 0);
    /* a file-scope prototype's parameters are pushed on the global stack,
       above every global (which has scope 0) */
    {
      Sym *b = global_stack;
      while (b && !(b->v & SYM_FIELD) && (b->v & ~SYM_STRUCT) < SYM_FIRST_ANOM && sym_scope(b) > 0)
        b = b->prev;
      if (b != global_stack)
        sym_pop(&global_stack, b, 0);
    }
    local_scope = 0;
    nocode_wanted = DATA_ONLY_WANTED;
    s1->cur_func_sym = NULL;
    recover_syntax_only(s1);
    recover_declare_pending(VT_CONST);
    recover_skip_decl(rp);
  }
  decl(VT_CONST);
  s1->recover = r.outer;
  tcc_free(r.lev);
}

/* ------------------------------------------------------------------------- */
/* a function body */

/* pop what a compound statement declared (no cleanups, no code: the body
   gets none) */
static void recover_close_level(RecoverLevel *l)
{
  /* a label a skipped part would have defined is no error */
  for (Sym *s = local_label_stack; s && s != l->llstk; s = s->prev)
    if (s->r == LABEL_FORWARD)
      s->r = LABEL_DEFINED;
  sym_pop(&local_stack, l->lstk, 0);
  label_pop(&local_label_stack, l->llstk, 0);
  nested_scope_end(l->level);
}

/* the rest of the body, as the compound statements still open would have
   parsed it, without code */
static void recover_resume(TCCRecover *r)
{
  TCCState *s1 = tcc_state;
  int n = r->keep, i;
  struct scope *prev = r->root_scope, *sc;

  /* the locals a replay pushed above the innermost brace's */
  {
    Sym *b = local_stack;
    const RecoverLevel *l = &r->lev[n - 1];
    while (b && b != l->lstk &&
           ((b->v & SYM_FIELD) || (b->v & ~SYM_STRUCT) >= SYM_FIRST_ANOM || b->sym_scope > l->level))
      b = b->prev;
    if (b != local_stack)
      sym_pop(&local_stack, b, 0);
  }

  s1->check_only++;
  sc = r->scopes = tcc_mallocz(n * sizeof *sc);
  for (i = 0; i < n; i++)
  {
    sc[i] = *prev;
    sc[i].prev = prev;
    sc[i].vla.num = sc[i].vla.loc = sc[i].vla.locorig = 0;
    sc[i].cl = r->root_scope->cl; /* no defer runs */
    sc[i].lstk = r->lev[i].lstk;
    sc[i].llstk = r->lev[i].llstk;
    sc[i].level = r->lev[i].level;
    sc[i].kind = r->lev[i].kind;
    sc[i].mserial = 0;
    r->dummy_b = r->dummy_c = -1;
    sc[i].bsym = r->can_break || r->in_switch ? &r->dummy_b : NULL;
    sc[i].csym = r->in_loop ? &r->dummy_c : NULL;
    prev = &sc[i];
  }
  if (r->in_switch)
  {
    /* on the heap: an error out of the compile frees it with the others */
    struct switch_t *sw = r->dummy_sw = tcc_mallocz(sizeof *sw);
    sw->nocode_wanted = 1;
    sw->bsym = &r->dummy_b;
    sw->scope = &sc[n - 1];
    sw->prev = cur_switch;
    sw->sv.type.t = VT_INT;
    cur_switch = sw;
  }
  loop_scope = r->in_loop ? &sc[n - 1] : r->loop_scope;
  cur_scope = &sc[n - 1];
  local_scope = sc[n - 1].level;
  nocode_wanted = 0;
  recover_declare_pending(VT_LOCAL);

  for (;;)
  {
    while (tok != '}' && tok != TOK_EOF)
    {
      decl(VT_LOCAL);
      if (tok != '}' && tok != TOK_EOF)
        block(STMT_COMPOUND);
    }
    if (tok == TOK_EOF)
    {
      /* no '}' comes: the end of the compile (gcc's words) */
      s1->error_fatal = 1;
      tcc_error("expected declaration or statement at end of input");
    }
    if (n == 1)
      break; /* the body's own '}': block(0) leaves it to its caller */
    next();
    recover_close_level(&r->lev[--n]);
    r->keep = n;
    cur_scope = &sc[n - 1];
    local_scope = sc[n - 1].level;
    if (r->in_loop)
      loop_scope = cur_scope;
    if (tok == TOK_ELSE)
    {
      /* the if the closed block belonged to has an else */
      next();
      block(0);
    }
  }
  recover_close_level(&r->lev[0]);
  if (r->dummy_sw && cur_switch == r->dummy_sw)
    end_switch();
  r->dummy_sw = NULL;
  cur_scope = r->root_scope;
  loop_scope = r->loop_scope;
  local_scope = r->local_scope;
  s1->check_only--;
}

/* block(0) for a function body; 1 when it was recovered from an error
   (reported, and the rest of the body checked): the body then gets no code
   and no flow checks. */
ST_FUNC int gen_body_recovering(void)
{
  TCCState *s1 = tcc_state;
  TCCRecover r;
  int errored;
  recover_save(&r, RECOVER_BODY);
  r.outer = s1->recover;
  s1->recover = &r;
  s1->undecl_func = NULL; /* a new function: its undeclared names are new */
  if (setjmp(r.jb))
  {
    TCCRecover *rp = s1->recover;
    recover_restore(rp);
    rp->errored = 1;
    recover_syntax_only(s1);
    recover_skip_stmt(rp);
    recover_resume(rp);
  }
  else
    block(0);
  s1->recover = r.outer;
  errored = r.errored; /* an error that did not stop the parse leaves a sound IR */
  tcc_free(r.lev);
  tcc_free(r.scopes);
  return errored;
}

/* ------------------------------------------------------------------------- */
/* a struct or union body */

/* The members of a struct or union body: an error in one goes on with the
   next, so the struct is defined with the members that were right and the
   declarators after its body are declared (gcc).  The function body the
   struct is in, if any, gets no code. */
ST_FUNC void struct_body_recovering(void (*member)(void *), void *arg)
{
  TCCState *s1 = tcc_state;
  TCCRecover r;
  if (!s1->recover)
  {
    while (tok != '}')
      member(arg);
    return;
  }
  recover_save(&r, RECOVER_STRUCT);
  r.outer = s1->recover;
  s1->recover = &r;
  if (setjmp(r.jb))
  {
    TCCRecover *rp = s1->recover;
    recover_restore(rp);
    recover_body_errored(rp);
    recover_syntax_only(s1);
    recover_skip_member(rp);
  }
  while (tok != '}')
    member(arg);
  s1->recover = r.outer;
}

/* ------------------------------------------------------------------------- */
/* an inline expansion's body */

/* The function body the innermost point is in gets no code. */
static void recover_body_errored(TCCRecover *r)
{
  for (r = r->outer; r && r->body != RECOVER_BODY; r = r->outer)
    ;
  if (r)
    r->errored = 1;
}

static void replay_block(void *ts)
{
  begin_macro(ts, 2);
  next();
  block(0);
  end_macro();
}

/* FN(ARG) parses a replay (a body saved as tokens) the parse goes on after
   whatever it holds: an error in it ends the replay as if it had ended there
   (tcc_recover_jump unwinds it), the function body it is in, if any, gets no
   code, and the unit goes on as -fsyntax-only.  1 when an error ended it. */
ST_FUNC int replay_recovering(void (*fn)(void *), void *arg)
{
  TCCState *s1 = tcc_state;
  TCCRecover r;
  recover_save(&r, RECOVER_INLINE);
  r.outer = s1->recover;
  s1->recover = &r;
  if (setjmp(r.jb))
  {
    TCCRecover *rp = s1->recover;
    recover_restore(rp);
    recover_body_errored(rp);
    recover_syntax_only(s1);
    s1->recover = rp->outer;
    return 1;
  }
  fn(arg);
  s1->recover = r.outer;
  return 0;
}

/* Replay TS, a body borrowed with begin_macro(.., 2): an inline expansion's
   (CALLEE) or a defer's (NULL).  An error in an inline expansion is the
   callee's -- said once, its own compile says it again: the expansion ends
   there, the callee is not expanded again, and the caller's parse goes on
   after the call, as at -O0 where nothing was expanded.  An error in a defer
   body ends that copy; the code after it goes on.  Either way the function
   body it is in gets no code (its IR holds half a replay). */
ST_FUNC void replay_body_recovering(TokenString *ts, Sym *callee)
{
  if (replay_recovering(replay_block, ts) && callee && callee->type.ref)
  {
    callee->type.ref->f.func_noinline = 1;
    callee->type.ref->f.func_auto_inline = 0;
    callee->type.ref->f.func_eval_only_inline = 0;
    callee->type.ref->f.func_small_leaf = 0;
  }
}
