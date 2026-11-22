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

/* defer.c -- the yasc dialect's `defer` and `errdefer` statements.
 *
 * `defer STMT` runs STMT whenever its scope is left: falling off the end,
 * return, break, continue or goto out of it, last registered first, in the
 * same list as __attribute__((cleanup)) variables (cleanup.c).  The body is
 * saved as tokens when the defer is met and compiled again at every exit
 * (try_call_scope_cleanup), the way Zig expands defers, so it has no run-time
 * cost beyond its own code.  docs/plans/yasc_errors.md has the design.
 *
 * Replaying tokens at another place has three traps, handled here:
 *  - names must bind where the defer was written, not to a local declared
 *    later that shadows them: the locals pushed since are hidden;
 *  - the body must not leave itself (return, break or continue out of it,
 *    goto, labels): it is compiled once per exit, and would jump from one
 *    copy into the code around another;
 *  - its warnings would repeat once per exit: only the first copy reports. */

#include "gen_priv.h"

/* inline_expansion_depth + 1 while a defer body is compiled; an inline
 * expansion inside the body (its own returns are fine) raises the depth. */
int defer_body_level; /* error recovery puts it back (core/recover.c) */

/* Set while the exits of a function's error return are compiled: the
 * errdefers run only then (try_call_scope_cleanup). */
int cleanup_error_exit;

int defer_body_active(void)
{
  return defer_body_level && defer_body_level == tcc_state->inline_expansion_depth + 1;
}

/* Save the statement after `defer`: a braced block, or one expression
 * statement up to its ';'.  The lookahead after it is read, as block()
 * expects. */
static TokenString *defer_capture(void)
{
  TokenString *saved_capture = pp_pragma_capture;
  TokenString *str = tok_str_alloc();
  const int braces = tok == '{';
  int level = 0, recorded_len = 0;

  if (!braces && (tok == TOK_IF || tok == TOK_FOR || tok == TOK_WHILE || tok == TOK_DO || tok == TOK_SWITCH))
    tcc_error("a defer takes an expression or a block: put the statement in braces");
  /* #pragma pack inside the body travels with the tokens, as for a saved
   * function body (skip_or_save_block) */
  pp_pragma_capture = str;
  for (;;)
  {
    const int t = tok;
    if (t == TOK_EOF)
    {
      pp_pragma_capture = saved_capture;
      tcc_error("unexpected end of file");
    }
    if (!braces && level == 0 && t == '}')
    {
      pp_pragma_capture = saved_capture;
      expect("';'");
    }
    tok_str_add_tok(str);
    recorded_len = str->len;
    next();
    if (t == '{' || t == '(' || t == '[')
      level++;
    else if (t == '}' || t == ')' || t == ']')
    {
      if (--level == 0 && braces && t == '}')
        break;
    }
    else if (t == ';' && level == 0 && !braces)
      break;
  }
  pp_pragma_capture = saved_capture;
  if (str->len > recorded_len)
  {
    /* a #pragma pack read with the lookahead is outside the body */
    const int *buf = tok_str_buf(str);
    pp_apply_pack_replays(tcc_state, buf + recorded_len, buf + str->len);
    str->len = recorded_len;
  }
  tok_str_add(str, TOK_EOF);
  return str;
}

/* `defer` / `errdefer` was read; tok is the statement's first token. */
void defer_stmt(int kind)
{
  Sym *cls;

  /* A case label after it would jump past the defer, and the scope's end
   * would still run it. */
  if (cur_switch && cur_scope->prev == cur_switch->scope)
    tcc_error("defer directly in a switch body: put the case in braces");
  if (defer_body_active() && kind == CLEANUP_ERRDEFER)
    tcc_error("errdefer in a defer body");

  cls = sym_push2(&all_cleanups, SYM_FIELD | ++cur_scope->cl.n, kind, 0);
  cls->defer_str = defer_capture();
  /* -Wunused-*: what the body names is used, replayed or not */
  if (tcc_state->unused_on)
    unused_scan_tokens(cls->defer_str, NULL, 1);
  cls->prev_tok = local_stack;
  cls->vreg = 0;
  cls->next = cur_scope->cl.s;
  cur_scope->cl.s = cls;
}

/* Local names pushed since the defer was written, unbound for its replay. */
typedef struct
{
  int v;     /* token, with SYM_STRUCT for a tag */
  Sym *bound; /* what it was bound to before the replay */
} DeferHidden;

static int defer_hide_later_names(Sym *since, DeferHidden **out)
{
  DeferHidden *h = NULL;
  int n = 0, cap = 0;
  Sym *s;

  for (s = local_stack; s && s != since; s = s->prev)
  {
    const int v = s->v;
    if ((v & SYM_FIELD) || (v & ~SYM_STRUCT) >= SYM_FIRST_ANOM)
      continue;
    if (n == cap)
    {
      cap = cap ? cap * 2 : 8;
      h = tcc_realloc(h, cap * sizeof *h);
    }
    h[n].v = v;
    if (v & SYM_STRUCT)
    {
      h[n].bound = sym_tag_head(v & ~SYM_STRUCT);
      sym_set_tag_head(v & ~SYM_STRUCT, s->prev_tok);
    }
    else
    {
      h[n].bound = table_ident[v - TOK_IDENT]->sym_identifier;
      table_ident[v - TOK_IDENT]->sym_identifier = s->prev_tok;
    }
    n++;
  }
  *out = h;
  return n;
}

static void defer_restore_names(DeferHidden *h, int n)
{
  /* the newest first was saved first: restore backwards so it wins */
  while (n-- > 0)
  {
    if (h[n].v & SYM_STRUCT)
      sym_set_tag_head(h[n].v & ~SYM_STRUCT, h[n].bound);
    else
      table_ident[h[n].v - TOK_IDENT]->sym_identifier = h[n].bound;
  }
  tcc_free(h);
}

/* Compile one copy of the body of cleanup entry CLS here. */
void defer_replay(Sym *cls)
{
  TokenString *body = cls->defer_str;
  DeferHidden *hidden;
  const int nhidden = defer_hide_later_names(cls->prev_tok, &hidden);
  const int saved_tok = tok;
  const CValue saved_tokc = tokc;
  /* Exits are compiled in CODE_OFF regions too (try_call_scope_cleanup); the
   * body's own branches must not change the reachability around it. */
  const int saved_nocode_wanted = nocode_wanted;
  int *const saved_bsym = cur_scope->bsym, *const saved_csym = cur_scope->csym;
  const int saved_level = defer_body_level;
  const int saved_error_exit = cleanup_error_exit;
  const unsigned char saved_warn_none = tcc_state->warn_none;

  cur_scope->bsym = cur_scope->csym = NULL; /* break / continue out: an error */
  cleanup_error_exit = 0; /* the body's own scopes end normally */
  defer_body_level = tcc_state->inline_expansion_depth + 1;
  if (cls->vreg++ > 0)
    tcc_state->warn_none = 1; /* the first copy reported them */

  /* an error in it ends this copy (core/recover.c): the names and the
     scope's break / continue are put back all the same */
  replay_body_recovering(tok_str_borrow(body, body->len), NULL);

  tcc_state->warn_none = saved_warn_none;
  cleanup_error_exit = saved_error_exit;
  defer_body_level = saved_level;
  cur_scope->bsym = saved_bsym;
  cur_scope->csym = saved_csym;
  nocode_wanted = saved_nocode_wanted;
  tok = saved_tok;
  tokc = saved_tokc;
  defer_restore_names(hidden, nhidden);
}

/* Free the bodies of the entries FROM .. TO (exclusive): their scope ended.
 * An errdefer no error exit replayed is compiled once without code, so its
 * errors are still reported. */
void defer_free_entries(Sym *from, Sym *to)
{
  for (; from && from != to; from = from->next)
    if (from->type.t && from->defer_str)
    {
      if (from->type.t == CLEANUP_ERRDEFER && from->vreg == 0)
      {
        nocode_wanted++;
        defer_replay(from);
        nocode_wanted--;
      }
      tok_str_free(from->defer_str);
      from->defer_str = NULL;
    }
}

/* `return EXPR;` with defers to run: EXPR is the value before they run, as
 * in Zig.  Load an lvalue and settle compare flags into a temporary first;
 * a struct is copied to one. */
void defer_settle_return_value(void)
{
  Sym *cls;
  int pending = 0;

  for (cls = cur_scope->cl.s; cls && cls != root_scope->cl.s; cls = cls->next)
    if (cls->type.t)
      pending = 1;
  if (!pending || !tcc_state->ir)
    return;
  if ((vtop->type.t & VT_BTYPE) == VT_STRUCT)
  {
    if (vtop->r & VT_LVAL)
    {
      int align, vr = -1;
      const int size = type_size(&vtop->type, &align);
      CValue cv;
      cv.i = get_temp_local_var(size, align, &vr);
      vsetc(&vtop->type, VT_LOCAL | VT_LVAL, &cv);
      vtop->vr = vr;
      vswap();
      vstore();
    }
    return;
  }
  tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
  if (vtop->r & VT_LVAL)
  {
    SValue load_dst;
    svalue_init(&load_dst);
    load_dst.type = vtop->type;
    load_dst.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    load_dst.r = 0;
    load_dst.c.i = 0;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &load_dst);
    vtop->vr = load_dst.vr;
    vtop->r = 0;
  }
}
