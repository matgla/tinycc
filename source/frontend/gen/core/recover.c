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
 * Recovery points, each a setjmp: one around the file-scope declaration
 * loop (tccgen_decls_recovering), one around the parse of each function body
 * (gen_body_recovering, from gen_function and gen_function_check_only), one
 * per struct body, per replay, and per function's compile (RECOVER_FUNC:
 * its setup and code generation, backend/generators/function.c).  A
 * tcc_error longjmps to the innermost armed one (error1 -> tcc_recover_jump)
 * when the state allows; otherwise, as before, out of the compile:
 *
 *   - inside a preprocessor directive or an assembler file, after an
 *     internal error, and in const_eval's speculative parse;
 *   - when a recovery would not move (the same token twice), and at the end
 *     of the input inside a body.
 *
 * A macro used with the wrong number of arguments, and a lexer error in a
 * body saved as tokens, are said later, where the parse reads the marker
 * tcc_error_stash put in the token stream.  After an error in a declarator
 * the declaration list goes on with its next declarator
 * (recover_next_declarator).
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
  RECOVER_FUNC,   /* a function's compile: its setup, its code generation
                     (backend/generators/function.c) */
};

_Static_assert(RECOVER_FUNC == RECOVER_CALL_FUNC, "tcc.h names RECOVER_FUNC");

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
  unsigned char body;    /* RECOVER_* */
  unsigned char errored; /* an error was recovered from here */
  unsigned char heap;    /* tcc_malloc'ed (recover_call, gen_body_recovering), not a frame's */
  unsigned char in_loop, in_switch, can_break;
  int nb_errors;         /* when armed */
  /* the state when armed */
  TokenString *mstack;
  SValue *vtop;
  int nocode_wanted, local_scope, unevaluated_operand, global_expr, func_param_decl_depth;
  int param_names_nb, param_names_base; /* decl_param_names */
  int compound_depth, struct_depth, ym_depth, defer_level, cleanup_error_exit, check_only, parse_flags;
  int nb_stk_data, hidden_labels, inline_return_loc, inline_return_vr, rsym, func_var, func_has_label_addr;
  unsigned pending_call_temp_slots, diag_parse;
  void *call_checks, *obj_calls; /* builtin/call.c, builtin/objsize.c: the calls begun */
  void *fort_calls;              /* builtin/fortify.c */
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
  /* the declaration list the error was in, which goes on after its ',' */
  RecoverDeclList *decl_list;
  TokenString *pp_capture;
  int diag_anchor, init_depth;
  /* the initializer being parsed (init/initializer.c) */
  int self_lo, self_hi, self_tok, self_vreg, self_on, self_depth;
  struct InitOvr *ovr_top;
  int dr_braces; /* the initializer braces the error was in */
  unsigned char decl_resume;
  RecoverDeclResume dr;
};

/* ------------------------------------------------------------------------- */
/* state */

RecoverDecl recover_decl;
int init_brace_depth;

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
  decl_param_names(0, &r->param_names_nb, &r->param_names_base);
  r->struct_depth = struct_body_depth;
  r->ym_depth = yasc_methods_depth(-1);
  r->defer_level = defer_body_level;
  r->cleanup_error_exit = cleanup_error_exit;
  r->check_only = s1->check_only;
  r->parse_flags = parse_flags;
  r->nb_stk_data = nb_stk_data;
  r->call_checks = call_arg_checks_mark();
  r->obj_calls = objsize_calls_mark();
  r->fort_calls = fortify_calls_mark();
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
  r->decl_list = recover_decl_list;
  r->pp_capture = pp_pragma_capture;
  r->diag_anchor = s1->diag_anchor;
  r->init_depth = init_brace_depth;
  r->self_lo = init_self_lo, r->self_hi = init_self_hi, r->self_tok = init_self_tok;
  r->self_vreg = init_self_vreg, r->self_on = init_self_on, r->self_depth = init_self_depth;
  r->ovr_top = init_ovr_top;
}

/* What the frames the longjmp left would have put back on their way out. */
static void recover_restore(TCCRecover *r)
{
  TCCState *s1 = tcc_state;
  vtop = r->vtop;
  nocode_wanted = r->nocode_wanted;
  unevaluated_operand = r->unevaluated_operand;
  global_expr = r->global_expr;
  func_param_decl_depth = r->func_param_decl_depth;
  decl_param_names(1, &r->param_names_nb, &r->param_names_base);
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
  decl_list_unwind(r->decl_list);
  recover_decl_resume.on = 0;
  pp_pragma_capture = r->pp_capture;
  init_brace_depth = r->init_depth;
  init_self_lo = r->self_lo, init_self_hi = r->self_hi, init_self_tok = r->self_tok;
  init_self_vreg = r->self_vreg, init_self_on = r->self_on, init_self_depth = r->self_depth;
  init_ovr_unwind(r->ovr_top);
  if (r->body != RECOVER_FILE)
    s1->diag_anchor = r->diag_anchor; /* the file-scope loop's is the declaration's */
  if (s1->asm_inline)
    asm_inline_recover(); /* an inline asm statement's text, assembled */
  discarded_call_vtop = NULL;
  unary_addr_operand = 0;
  indir_keep_lvalue = 0;
  yasc_returning = yasc_argument = 0;
  if (s1->unsafe_depth)
  {
    BufferedFile *uf;
    const char *un = tcc_diag_file(&uf);
    tcc_unsafe_reset(s1, un, uf ? uf->line_num : 0);
  }
  fe_compound_pending = 0;
  funcall_self_pending = 0;
  expr_eq_assigned = 0;
  yasc_sbind_pending = 0;
  yasc_plain_reset();
  yasc_pp_recover(r->body);
  attr_checks_recover();
  call_arg_checks_unwind(r->call_checks);
  objsize_calls_unwind(r->obj_calls);
  fortify_calls_unwind(r->fort_calls);
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
  TCCRecover *r = s1->recover, *outer;
  for (; r; r = outer)
  {
    outer = r->outer;
    tcc_free(r->lev);
    tcc_free(r->scopes);
    r->lev = NULL;
    r->scopes = NULL;
    if (r->heap)
      tcc_free(r);
  }
  s1->recover = NULL;
}

/* The stk_push'd buffers of the frames a longjmp to R leaves: freed while
   those frames still live -- after it, their pointers are dead stack. */
static void recover_free_stk(const TCCRecover *r)
{
  while (nb_stk_data > r->nb_stk_data)
    tcc_free(*(void **)stk_data[--nb_stk_data]);
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

  if (r->body >= RECOVER_STRUCT)
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

  /* an error in a declaration list's declarator: the list may go on with
     the next one (recover_next_declarator) -- at the point's own level, no
     scope or struct body opened since the declaration began */
  r->decl_resume = 0;
  {
    RecoverDeclList *dl = recover_decl_list;
    if (!inj && dl && dl != r->decl_list && dl->active && dl->struct_depth == struct_body_depth &&
        ((r->body == RECOVER_FILE && dl->l == VT_CONST && dl->outer == r->decl_list) ||
         (r->body == RECOVER_BODY && dl->l == VT_LOCAL && dl->scope == cur_scope && r->keep == r->nlev &&
          r->closes == 0 && cur_scope && (cur_scope->kind & SCOPE_BRACE))))
    {
      r->decl_resume = 1;
      r->dr_braces = init_brace_depth - dl->init_base;
      r->dr.on = 1;
      r->dr.l = dl->l;
      r->dr.oldint = dl->oldint;
      r->dr.exported = dl->exported;
      r->dr.btype = *dl->btype;
      r->dr.adbase = *dl->adbase;
    }
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
  recover_free_stk(r);
  longjmp(r->jb, 1);
}

/* The parse stops (a fatal error, or the -fmax-errors cap, while the
   diagnostics are held: driver/diag_out.c): out to the compile of the
   function it is in (RECOVER_FUNC) or to the file-scope loop, which see
   tcc_state->diag_stop and read nothing more; the end of the unit still
   parses the bodies saved before it, whose diagnostics come first, as at
   -O0.  Returns when there is no such point. */
ST_FUNC void tcc_recover_stop(TCCState *s1)
{
  TCCRecover *r = s1->recover;
  while (r && r->body != RECOVER_FILE && r->body != RECOVER_FUNC)
    r = r->outer;
  if (!r)
    return;
  for (TCCRecover *q = s1->recover, *outer; q != r; q = outer)
  {
    outer = q->outer;
    tcc_free(q->lev);
    tcc_free(q->scopes);
    q->lev = NULL;
    q->scopes = NULL;
    if (q->heap)
      tcc_free(q); /* an inline expansion's or a body's: its frame is gone */
  }
  while (tok_macro_stack() && tok_macro_stack() != r->mstack)
    end_macro();
  pp_expr = 0;
  pp_subst_depth = 0;
  s1->error_fatal = 0;
  s1->recover = r;
  recover_free_stk(r);
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
/* errors said later (tcc_error_stash) */

/* An error said later, where the parse reaches the marker the caller puts
   in the token stream: a TOK_PACK_REPLAY of the value returned.  The line is
   the one error1 would give now.  For a macro used with the wrong number of
   arguments (its expansion is the marker: the parser's recovery goes on
   from there, out of the macro's substitution) and for a number's error in
   a body being saved as tokens (-O0 says it where the parse reads it). */
static int diag_stash_add(int kind, const char *msg)
{
  TCCState *s1 = tcc_state;
  BufferedFile *f;
  DiagStash *e;
  int line = 0;
  for (f = file; f && f->filename[0] == ':'; f = f->prev)
    ;
  if (f)
    line = f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
  if (s1->nb_diag_stash >= 0xffff)
    tcc_error("%i:%s", line, msg); /* MSG is in the caller's frame */
  if (s1->nb_diag_stash == s1->diag_stash_cap)
  {
    s1->diag_stash_cap = s1->diag_stash_cap ? 2 * s1->diag_stash_cap : 8;
    s1->diag_stash = tcc_realloc(s1->diag_stash, s1->diag_stash_cap * sizeof *s1->diag_stash);
  }
  e = &s1->diag_stash[s1->nb_diag_stash++];
  e->msg = tcc_strdup(msg);
  e->line = line;
  e->kind = kind;
  e->raised = 0;
  return (TCC_PCH_REPLAY_ERROR << 16) | s1->nb_diag_stash;
}

ST_FUNC int tcc_error_stash(const char *fmt, ...)
{
  /* in this frame: diag_stash_add's error (too many) is recovered from */
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  return diag_stash_add(DIAG_STASH_ERROR, msg);
}

/* #error or #warning in a body being saved as tokens: a marker in the saved
   stream says it where the parse of the body reaches it, as at -O0.  1 when
   it is deferred so. */
ST_FUNC int tcc_pp_diag_deferred(int kind, const char *msg)
{
  TCCState *s1 = tcc_state;
  CValue cv;
  if (!s1->save_body_jmp || !pp_pragma_capture || s1->fe_speculative)
    return 0;
  cv.i = diag_stash_add(kind, msg);
  tok_str_add2(pp_pragma_capture, TOK_PACK_REPLAY, &cv);
  return 1;
}

static void diag_stash_say(TCCState *s1, DiagStash *e)
{
  if (e->kind == DIAG_STASH_WARNING)
  {
    s1->diag_any_header = 1; /* given in a system header too, as gcc */
    tcc_warning("%i:%s", e->line, e->msg);
  }
  else
    tcc_error_noabort("%i:%s", e->line, e->msg);
}

/* next() read a TCC_PCH_REPLAY_ERROR marker: say its error, here */
ST_FUNC void tcc_error_stash_raise(TCCState *s1, int idx)
{
  DiagStash *e;
  if (!idx)
  {
    save_body_raise(s1);
    return;
  }
  if (idx > s1->nb_diag_stash)
    return;
  e = &s1->diag_stash[idx - 1];
  if (e->kind == DIAG_STASH_ERROR)
  {
    if (!s1->fe_speculative)
      e->raised = 1;
    tcc_error("%i:%s", e->line, e->msg);
  }
  /* #error, #warning: once, not in a speculative parse nor in an inline
     expansion (quiet: the body's own parse, or the end of the unit, says it) */
  if (e->raised || s1->fe_speculative || s1->in_inline_expansion)
    return;
  e->raised = 1;
  diag_stash_say(s1, e);
}

/* The end of the unit: an error whose marker the parse never reached (a
   saved body nothing parsed) is said all the same. */
ST_FUNC void tcc_error_stash_unraised(TCCState *s1)
{
  for (int i = 0; i < s1->nb_diag_stash; i++)
    if (!s1->diag_stash[i].raised)
    {
      s1->diag_stash[i].raised = 1;
      diag_stash_say(s1, &s1->diag_stash[i]);
    }
}

/* A lexer error of a token that can be read past (a number's): in a body
   being saved as tokens it goes in the saved stream as a marker, said where
   the parse of the body reaches it, as at -O0 (1: the caller makes the token
   a 0 and goes on); otherwise said now. */
ST_FUNC int tcc_lex_error_deferred(const char *fmt, ...)
{
  TCCState *s1 = tcc_state;
  /* in this frame, not on the heap: the error said now is recovered from,
     and a heap copy would be lost with its longjmp */
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (s1->save_body_jmp && pp_pragma_capture && !s1->fe_speculative)
  {
    CValue cv;
    cv.i = diag_stash_add(DIAG_STASH_ERROR, msg);
    tok_str_add2(pp_pragma_capture, TOK_PACK_REPLAY, &cv);
    return 1;
  }
  tcc_error("%s", msg);
  return 0;
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

/* After an error in a declarator of a declaration list or in its
   initializer: the ',' at the list's level, before the ';' that ends it.
   The brackets open where the error is are not known, so the tokens are read
   up to the first ';' at the lowest depth reached (never inside a statement
   expression opened after the error), and a ',' at that depth after the
   depth got there is the list's.  With one, the parse goes on after it with
   the list's next declarator (decl() takes recover_decl_resume) and 1 is
   returned; with none, 2: the declaration is skipped, its ';' too.  A '}'
   to a new low followed by what no initializer's brace is (a block's end:
   the ';' is missing), or the end of the input: 0, the tokens read are put
   back for the plain skip. */
static int recover_next_declarator(TCCRecover *r)
{
  TokenString *buf = tok_str_alloc(), *rest;
  int depth = 0, min = 0, sep = -1, sep_line = 0, end = 0, prev = 0, brace_low = 0;
  CValue cv;
  while (tok != TOK_EOF)
  {
    const int t = tok;
    if (brace_low && t != ',' && t != ';' && t != '}' && t != ')' && t != ']' && t != '.' && t != '[' &&
        t != TOK_ARROW)
    {
      /* the initializer the error was in ends there, its ';' missing:
         the declaration ends with it, the parse goes on at T */
      if (depth >= -r->dr_braces)
        end = 2, sep = -1;
      break;
    }
    brace_low = 0;
    /* a function body (the error was in its declarator): the plain skip's */
    if (t == '{' && prev == ')' && depth == min)
      break;
    if (t == '(' || t == '[' || t == '{')
      depth++;
    else if (t == ')' || t == ']' || t == '}')
    {
      if (--depth < min)
        min = depth, sep = -1, brace_low = t == '}';
    }
    else if (t == ';' && depth == min)
    {
      tok_str_add_tok(buf);
      end = 1;
      break;
    }
    tok_str_add_tok(buf);
    prev = t;
    /* out of the initializer braces the error was in, at least */
    if (t == ',' && depth == min && depth <= -r->dr_braces && sep < 0)
    {
      sep = buf->len;
      sep_line = file->line_num;
    }
    next();
  }
  if (end && sep < 0)
  {
    /* no other declarator: the declaration is skipped */
    prev = 0;
    for (const int *p = tok_str_buf(buf), *e = p + buf->len; p < e;)
    {
      int t;
      tok_get(&t, &p, &cv);
      if (t != TOK_LINENUM)
        recover_mention(t, prev), prev = t;
    }
    tok_str_free(buf);
    if (end == 1)
      next(); /* past its ';' */
    return 2;
  }
  if (!end)
  {
    /* no ',' of the list: back to the first token read (the one the scan
       stopped at comes after them again) */
    if (buf->len)
    {
      if (tok != TOK_EOF)
        tok_str_add_tok(buf);
      tok_str_add(buf, 0);
      begin_macro(buf, 1);
      next();
    }
    else
      tok_str_free(buf);
    return 0;
  }
  /* the skipped names count as used; the rest is read again */
  prev = 0;
  {
    const int *p = tok_str_buf(buf), *e = p + sep;
    while (p < e)
    {
      int t;
      tok_get(&t, &p, &cv);
      if (t != TOK_LINENUM)
        recover_mention(t, prev), prev = t;
    }
  }
  rest = tok_str_alloc();
  cv.i = sep_line;
  tok_str_add2(rest, TOK_LINENUM, &cv);
  for (const int *p = tok_str_buf(buf) + sep, *e = tok_str_buf(buf) + buf->len; p < e;)
  {
    int t;
    tok_get(&t, &p, &cv);
    tok_str_add2(rest, t, &cv);
  }
  tok_str_add(rest, 0);
  tok_str_free(buf);
  begin_macro(rest, 1);
  next();
  recover_decl_resume = r->dr;
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
    if (s1->diag_stop)
      recover_decl.l = 0; /* the parse stops: decl() reads no more */
    else
    {
      recover_declare_pending(VT_CONST);
      if (!rp->decl_resume || !recover_next_declarator(rp))
        recover_skip_decl(rp);
    }
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
  /* on the heap: the body's parse is under it, and inline expansions make
     that deep (replay_recovering) */
  TCCRecover *r = tcc_malloc(sizeof *r);
  int errored;
  recover_save(r, RECOVER_BODY);
  r->heap = 1;
  r->outer = s1->recover;
  s1->recover = r;
  s1->undecl_func = NULL; /* a new function: its undeclared names are new */
  if (setjmp(r->jb))
  {
    TCCRecover *rp = s1->recover;
    recover_restore(rp);
    rp->errored = 1;
    recover_syntax_only(s1);
    if (!rp->decl_resume || !recover_next_declarator(rp))
      recover_skip_stmt(rp);
    recover_resume(rp);
  }
  else
    block(0);
  s1->recover = r->outer;
  errored = r->errored; /* an error that did not stop the parse leaves a sound IR */
  tcc_free(r->lev);
  tcc_free(r->scopes);
  tcc_free(r);
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
  return recover_call(fn, arg, RECOVER_INLINE);
}

/* FN(ARG) with a recovery point of kind KIND (RECOVER_INLINE: a replay,
   RECOVER_FUNC: a function's compile, outside its body's parse): an error
   in it ends it, the unit goes on as -fsyntax-only.  1 when an error ended
   it -- what FN left half done is the caller's to put back. */
ST_FUNC int recover_call(void (*fn)(void *), void *arg, int kind)
{
  TCCState *s1 = tcc_state;
  /* On the heap, not in this frame: inline expansions nest (INLINE_NEST_MAX
     deep, called-once chains in Zig's C get there), each one a replay with
     the parser's frames around it, and the point's jmp_buf and saved state
     (360 bytes on the device) were a sixth of the stack an expansion level
     takes -- the device tcc's 32 KiB ran out on the kernel's C. */
  TCCRecover *r = tcc_malloc(sizeof *r);
  recover_save(r, kind);
  r->heap = 1;
  r->outer = s1->recover;
  s1->recover = r;
  if (setjmp(r->jb))
  {
    TCCRecover *rp = s1->recover;
    recover_restore(rp);
    recover_body_errored(rp);
    recover_syntax_only(s1);
    s1->recover = rp->outer;
    tcc_free(rp);
    return 1;
  }
  fn(arg);
  s1->recover = r->outer;
  tcc_free(r);
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
  /* recover_call itself, not replay_recovering: one frame less a level of
     nested inline expansions */
  if (recover_call(replay_block, ts, RECOVER_INLINE) && callee && callee->type.ref)
  {
    callee->type.ref->f.func_noinline = 1;
    callee->type.ref->f.func_auto_inline = 0;
    callee->type.ref->f.func_eval_only_inline = 0;
    callee->type.ref->f.func_small_leaf = 0;
  }
}
