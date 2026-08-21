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

/* cleanup.c -- __attribute__((cleanup)) invocation on scope exit and goto.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* __attribute__((cleanup(fn))) */

/* Inline-expand a cleanup call if `fs` is an auto-inline candidate.
 *
 * Cleanup calls are emitted at scope-exit by try_call_scope_cleanup, which
 * bypasses unary_funcall's normal call-site path — and with it, the auto-
 * inline machinery.  For a function like 101_cleanup's INCR_GI macro
 * (`int i __attribute__((cleanup(incr_glob_i))) = 1;`) repeated 65k+ times,
 * that means each cleanup emits a BL to incr_glob_i instead of inlining
 * `glob_i += *i`, leaving tens of thousands of redundant call sequences in
 * the output.
 *
 * This helper mirrors the inline-expansion block in unary_funcall (around
 * the `(has_addr_of_label || force_always_inline)` branch), but trimmed for
 * the cleanup-call shape: one pointer argument, void return, no labels in
 * the body, no struct-return setup.  Returns 1 if the cleanup was inlined
 * (caller skips the PARAM/CALL emission), 0 otherwise. */
/* DBG_CLINL traces why a cleanup call was or was not inlined.  Every early
 * return below carries one, so this used to be 14 unlatched getenv calls on
 * the scope-exit path of any function with a cleanup attribute. */
TCC_DBG_ENV_FLAG(dbg_clinl, "DBG_CLINL")

/* Resolved lazily inside the traces: get_tok_str is a real lookup, and outside
 * a DBG_CLINL run nothing reads the name. */
static inline const char *clinl_name(Sym *fs)
{
  return fs ? get_tok_str(fs->v & ~SYM_FIELD, NULL) : "?";
}

static int try_inline_cleanup_call(Sym *fs, Sym *vs)
{
  if (!tcc_state->ir) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: no ir\n", clinl_name(fs)));
    return 0;
  }
  if (!fs || !vs || !fs->type.ref) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: missing type.ref\n", clinl_name(fs)));
    return 0;
  }
  if (!fs->type.ref->f.func_auto_inline || fs->type.ref->f.func_noinline) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: auto_inline=%d noinline=%d\n",
                                     clinl_name(fs), fs->type.ref->f.func_auto_inline, fs->type.ref->f.func_noinline));
    return 0;
  }
  if (!tcc_state->opt_inline_functions && !tcc_state->opt_inline_small) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: inline opts off\n", clinl_name(fs)));
    return 0;
  }
  /* Cleanup functions return void. */
  if ((fs->type.ref->type.t & VT_BTYPE) != VT_VOID) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: not void\n", clinl_name(fs)));
    return 0;
  }
  if (fs->a.nested_func) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: nested\n", clinl_name(fs)));
    return 0;
  }

  Sym *s = fs->type.ref;
  Sym *param_sym = s->next;
  if (!param_sym || param_sym->next != NULL) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: bad params\n", clinl_name(fs)));
    return 0;
  }
  if (!auto_inline_sig_ok(fs)) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: bad sig\n", clinl_name(fs)));
    return 0;
  }

  struct InlineFunc *inline_fn = NULL;
  for (int i = 0; i < tcc_state->nb_inline_fns; i++)
  {
    if (tcc_state->inline_fns[i] && tcc_state->inline_fns[i]->sym == fs)
    {
      inline_fn = tcc_state->inline_fns[i];
      break;
    }
  }
  if (!inline_fn || !inline_fn->func_str) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: no inline_fn (nb_fns=%d)\n",
                                     clinl_name(fs), tcc_state->nb_inline_fns));
    return 0;
  }

  if (inline_body_has_apply_args(inline_fn->func_str)) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: apply_args\n", clinl_name(fs)));
    return 0;
  }
  if (inline_body_has_shadowed_ident(inline_fn->func_str)) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: shadowed_ident\n", clinl_name(fs)));
    return 0;
  }
  if (inline_body_has_static_local(inline_fn->func_str)) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: static_local\n", clinl_name(fs)));
    return 0;
  }

  int *fsb = tok_str_buf(inline_fn->func_str);
  int fsl = inline_fn->func_str->len;
  if (macro_ptr && macro_ptr >= fsb && macro_ptr < fsb + fsl) {
    TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: own_macro\n", clinl_name(fs)));
    return 0;
  }
  TCC_DBG_TRACE(dbg_clinl, (stderr, "[CLINL] %s: INLINING\n", clinl_name(fs)));

  /* Build the argument SValue: &vs (address of the cleanup variable). */
  vset(&vs->type, vs->r, vs->c);
  vtop->sym = vs;
  vtop->vr = vs->vreg;
  mk_pointer(&vtop->type);
  gaddrof();
  SValue arg_val = *vtop;
  --vtop;

  /* --- Create parameter local and store the argument --- */
  Sym *saved_local = local_stack;
  int saved_local_scope = local_scope;
  int saved_inline_const_arg_count = tcc_state->inline_const_arg_count;
  tcc_state->inline_const_arg_count = 0;
  ++local_scope; /* shadow caller's same-named variables */

  int psize, palign;
  psize = type_size(&param_sym->type, &palign);
  if (psize < 4)
    psize = 4;
  if (palign < 4)
    palign = 4;
  loc = (loc - psize) & -palign;

  int pv = param_sym->v & ~SYM_FIELD;
  if (pv == 0)
    pv = anon_sym++;
  Sym *psym = sym_push(pv, &param_sym->type, VT_LOCAL | VT_LVAL, loc);

  SValue store_dst;
  svalue_init(&store_dst);
  store_dst.type = param_sym->type;
  store_dst.r = VT_LOCAL | VT_LVAL;
  store_dst.vr = psym->vreg;
  store_dst.c.i = loc;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &arg_val, NULL, &store_dst);

  /* --- Save parser/codegen state --- */
  CType saved_func_vt = func_vt;
  int saved_func_var = func_var;
  int saved_func_has_label_addr = func_has_label_addr;
  int saved_rsym = rsym;
  const char *saved_funcname = funcname;
  struct scope *saved_root_scope = root_scope;
  uint8_t saved_in_inline_expansion = tcc_state->in_inline_expansion;
  int saved_inline_return_loc = tcc_state->inline_return_loc;
  int saved_inline_return_vr = tcc_state->inline_return_vr;
  uint8_t saved_inline_return_redirected = tcc_state->inline_return_redirected;
  /* Scope-exit cleanups are emitted even in CODE_OFF regions (see
   * try_call_scope_cleanup), and the replayed body's internal control flow
   * flips CODE_OFF back on when a forward branch inside it is resolved.  The
   * call this expansion replaces leaves reachability untouched, so the state
   * must survive the replay -- without this, a cleanup body containing an
   * `if` cleared the CODE_OFF a final `return` had set, and the enclosing
   * function got a spurious "function might return no value". */
  int saved_nocode_wanted = nocode_wanted;

  func_vt = s->type; /* void */
  func_var = 0;
  rsym = -1;
  tcc_state->in_inline_expansion = local_scope;
  tcc_state->inline_return_loc = 0;
  /* -1, not 0: vreg 0 is a real vreg, and a cleanup body is void so nothing
   * should ever bind this one. */
  tcc_state->inline_return_vr = -1;
  tcc_state->inline_return_redirected = 0;
  tcc_state->inline_expansion_depth++;
  root_scope = cur_scope;

  /* --- Replay the function body --- */
  int saved_tok = tok;
  CValue saved_tokc = tokc;
  int *inline_label_tokens = NULL;
  int nb_inline_label_tokens = 0;
  Sym **saved_inline_labels =
      inline_hide_label_bindings(inline_fn->func_str, &inline_label_tokens, &nb_inline_label_tokens);

  TokenString *inline_ts = tok_str_alloc();
  inline_ts->data.str = tok_str_buf(inline_fn->func_str);
  inline_ts->allocated_len = 1;
  inline_ts->len = inline_fn->func_str->len;
  begin_macro(inline_ts, 2);
  next();
  block(0);
  end_macro();
  inline_restore_label_bindings(inline_label_tokens, saved_inline_labels, nb_inline_label_tokens);

  tok = saved_tok;
  tokc = saved_tokc;

  /* --- Backpatch return jumps --- */
  tcc_ir_backpatch_to_here(tcc_state->ir, rsym);

  /* --- Restore state --- */
  nocode_wanted = saved_nocode_wanted;
  tcc_state->in_inline_expansion = saved_in_inline_expansion;
  tcc_state->inline_return_loc = saved_inline_return_loc;
  tcc_state->inline_return_vr = saved_inline_return_vr;
  tcc_state->inline_return_redirected = saved_inline_return_redirected;
  tcc_state->inline_expansion_depth--;
  func_vt = saved_func_vt;
  func_var = saved_func_var;
  func_has_label_addr = saved_func_has_label_addr;
  rsym = saved_rsym;
  funcname = saved_funcname;
  root_scope = saved_root_scope;
  tcc_state->inline_const_arg_count = saved_inline_const_arg_count;
  sym_pop(&local_stack, saved_local, 0);
  local_scope = saved_local_scope;

  (void)psym;
  return 1;
}

void try_call_scope_cleanup(Sym *stop)
{
  Sym *cls = cur_scope->cl.s;

  /* Cleanups must still be emitted in CODE_OFF regions (unreachable by fallthrough)
   * because forward gotos can jump to cleanup landing pads.
   * Still suppress in true no-eval/const-expression contexts.
   */
  if (nocode_wanted & ~CODE_OFF_BIT)
    return;

  for (; cls != stop; cls = cls->next)
  {
    Sym *fs = cls->cleanup_func;
    Sym *vs = cls->prev_tok;

    /* Try to inline-expand the cleanup body in place; falls back to a normal
     * PARAM/CALL when the cleanup function is too large or otherwise unsafe
     * to inline.  Inlining is critical for tests like 101_cleanup where the
     * cleanup function is small (`glob_i += *i`) but called tens of
     * thousands of times. */
    if (try_inline_cleanup_call(fs, vs))
      continue;

    vpushsym(&fs->type, fs);
    vset(&vs->type, vs->r, vs->c);
    vtop->sym = vs;
    vtop->vr = vs->vreg; /* Set vreg so gaddrof() can compute correct address */
    mk_pointer(&vtop->type);
    gaddrof();
    // gfunc_call(1);
    SValue src1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    svalue_init(&src1);
    src1.vr = -1;
    src1.r = VT_CONST;
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    LOG_CODEGEN("FUNCPARAMVAL push: site=scope_cleanup call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop->r, vtop->vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, vtop, &src1, NULL);
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 1);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[-1], &call_id_sv, NULL);
    vtop -= 2;
  }
}

void try_call_cleanup_goto(Sym *cleanupstate)
{
  Sym *oc, *cc;
  int ocd, ccd;

  if (!cur_scope->cl.s)
    return;

  /* search NCA of both cleanup chains given parents and initial depth */
  ocd = cleanupstate ? cleanupstate->v & ~SYM_FIELD : 0;
  for (ccd = cur_scope->cl.n, oc = cleanupstate; ocd > ccd; --ocd, oc = oc->next)
    ;
  for (cc = cur_scope->cl.s; ccd > ocd; --ccd, cc = cc->next)
    ;
  for (; cc != oc; cc = cc->next, oc = oc->next, --ccd)
    ;

  try_call_scope_cleanup(cc);
}

/* call 'func' for each __attribute__((cleanup(func))) */
void block_cleanup(struct scope *o)
{
  int jmp = -1; /* -1 = no pending jump */
  Sym *g, **pg;
  for (pg = &pending_gotos; (g = *pg) && g->c > o->cl.n;)
  {
    if (g->prev_tok->r & LABEL_FORWARD)
    {
      Sym *pcl = g->next;
      if (jmp < 0)
        jmp = gjmp(-1); /* -1 = no chain */
      tcc_ir_backpatch_to_here(tcc_state->ir, pcl->jnext);
      try_call_scope_cleanup(o->cl.s);
      pcl->jnext = gjmp(-1); /* -1 = no chain */
      if (!o->cl.n)
        goto remove_pending;
      g->c = o->cl.n;
      pg = &g->prev;
    }
    else
    {
    remove_pending:
      *pg = g->prev;
      sym_free(g);
    }
  }
  tcc_ir_backpatch_to_here(tcc_state->ir, jmp);
  try_call_scope_cleanup(o->cl.s);
}
