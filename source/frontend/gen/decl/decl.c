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

/* decl.c -- Top-level declaration loop and _Static_assert.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* `_Static_assert`, or C23's `static_assert` keyword.  Before C23 the latter
 * is an ordinary identifier (<assert.h> defines it as a macro). */
int tok_is_static_assert(void)
{
  return tok == TOK_STATIC_ASSERT || (tok == TOK_static_assert && TCC_C23(tcc_state));
}

void do_Static_assert(void)
{
  int c;
  const char *msg;

  next();
  skip('(');
  c = expr_const();
  msg = "_Static_assert fail";
  if (tok == ',')
  {
    next();
    msg = parse_mult_str("string constant")->data;
  }
  skip(')');
  if (c == 0)
    tcc_error("%s", msg);
  skip(';');
}

/* Save a function body for replay away from its definition, together with
 * the #pragma pack state it is defined under (see pp_pack_snapshot).  When
 * nothing compiles the body here (!COMPILED_NOW), its own pack directives are
 * applied to the live state now too: they sit at this point of the file, and
 * the declarations after the body must see them. */
/* FN's tokens are no longer wanted.  If they are the deferred body still being
 * replayed (adopted by save_function_body), define_deferred_function frees them
 * once its replay ends: the stream reads past the body's end from them. */
static void inline_fn_drop_tokens(InlineFunc *fn)
{
  if (fn->func_str && fn->func_str == tcc_state->deferred_body && tcc_state->deferred_adopted)
    tcc_state->deferred_adopted = 2; /* adopted, then dropped */
  else
    tok_str_free(fn->func_str);
}

/* An error the lexer raised while a body was saved (error1): kept for the
   parse of the saved part to raise where it reads the marker. */
ST_FUNC void save_body_error(TCCState *s1, int line, const char *msg)
{
  tcc_free(s1->save_body_msg);
  s1->save_body_msg = (char *)msg;
  s1->save_body_line = line;
  longjmp(*s1->save_body_jmp, 1);
}

/* next() read the marker save_body_cut left: the lexer's error, here, and
   the end of the compile as it was at -O0 */
ST_FUNC void save_body_raise(TCCState *s1)
{
  if (!s1->save_body_msg)
    return;
  s1->error_fatal = 1;
  tcc_error("%i:%s", s1->save_body_line, s1->save_body_msg); /* tccgen_finish frees it */
}

/* The save of SYM's body STR stopped: STOP 1 at an error of the lexer, 2 at
   the end of the input (no '}').  At -O0 the body is parsed as it is read:
   the errors before that point come first, then the lexer's own or "...
   before '<eof>'".  So the part saved is parsed now (no code: the compile
   fails), the lexer's error raised where the parse reaches its marker.
   Never returns: the end of the input or that error ends the compile. */
static void save_body_cut(TokenString *str, int stop, Sym *sym)
{
  TCCState *s1 = tcc_state;
  CValue cv;
  if (stop == 2)
  {
    cv.i = file->line_num; /* where -O0 reads the end of the input */
    tok_str_add2(str, TOK_LINENUM, &cv);
  }
  else
  {
    cv.i = TCC_PCH_REPLAY_ERROR << 16;
    tok_str_add2(str, TOK_PACK_REPLAY, &cv);
  }
  tok_str_add(str, TOK_EOF);
  tcc_check_scratch_begin(s1);
  begin_macro(str, 1);
  next();
  gen_function_check_only(sym);
  save_body_raise(s1);
  tcc_error("unexpected end of file");
}

static void save_function_body(TokenString **str, int **pack, int compiled_now, Sym *sym)
{
  TCCState *s1 = tcc_state;
  int captures = pp_pack_captures;
  jmp_buf jb, *const saved_jmp = s1->save_body_jmp;
  const int saved_parse_flags = parse_flags, saved_subst = pp_subst_depth, saved_pp_expr = pp_expr;
  const int *const saved_macro_ptr = macro_ptr;
  TokenString *const saved_macro_stack = tok_macro_stack();
  int stop;
  tcc_diag_queue_begin(s1); /* the body may speak away from its place */
  *pack = pp_pack_snapshot(tcc_state);
  if ((stop = setjmp(jb)) != 0)
  {
    /* what the lexer was in the middle of is abandoned with the compile */
    s1->save_body_jmp = saved_jmp;
    pp_pragma_capture = NULL;
    parse_flags = saved_parse_flags;
    pp_subst_depth = saved_subst;
    pp_expr = saved_pp_expr;
    tok_macro_restore(saved_macro_stack, saved_macro_ptr);
    /* the replay owns the tokens now: the function's record lets them go
       (the compile ends, its records are freed after the macro stack) */
    TokenString *cut = *str;
    *str = NULL;
    save_body_cut(cut, stop, sym);
  }
  s1->save_body_jmp = &jb;
  /* a body a regular-C macro defines in yasc code: its expansion's "yasc off"
     came before the '{', and a replay starts in the file's dialect */
  const int dialect = s1->yasc;
  skip_or_save_block(str);
  s1->save_body_jmp = saved_jmp;
  if (file && dialect != file->yasc)
    tok_str_lead_yasc(*str, dialect);
  /* what the parse says after the body comes after what the body says,
     wherever it is parsed (driver/diag_out.c) */
  if (s1->diag_in_parse && !s1->in_inline_expansion)
    s1->diag_anchor = ++s1->diag_decl_seq;
  /* Replayed after the TU is parsed, its asm must not name an r0/r1 function. */
  gfunc_regpair_note_asm_names(*str);
  if (!compiled_now && pp_pack_captures != captures)
  {
    const int *buf = tok_str_buf(*str);
    pp_apply_pack_replays(tcc_state, buf, buf + (*str)->len);
  }
  /* A deferred body compiled now (define_deferred_function) records what it
   * replays: the same tokens, already kept and spilled.  Take that one and
   * drop the copy -- it would be written to the spill file a second time and
   * read back beside the first while the function compiles. */
  TokenString *src = tcc_state->deferred_body;
  if (compiled_now && src && !tcc_state->deferred_adopted && src->len == (*str)->len &&
      !memcmp(tok_str_buf(src), tok_str_buf(*str), sizeof(int) * src->len))
  {
    tok_str_free(*str);
    *str = src;
    tcc_state->deferred_adopted = 1;
    return;
  }
  tok_str_fit(*str);
  *str = tok_str_keep(*str);
  tok_str_spill(*str); /* internal spill: out of the heap until replayed */
}

/* A file-scope function definition's body, from its opening '{' in the
 * current token stream: record a static inline as a deferred body, register an
 * auto-inline candidate, or generate the code.  Called at the definition
 * itself, or at the end of the TU for a body gen_deferred_function_bodies
 * replays. */
static void define_global_function(Sym *sym, Section *ad_section)
{
  /* static inline functions are just recorded as a kind
     of macro. Their code will be emitted at the end of
     the compilation unit only if they are used */
  if (sym->type.t & VT_INLINE)
  {
    struct InlineFunc *fn;
    fn = tcc_mallocz(sizeof *fn);
    fn->filename = tcc_filename_intern(file->filename);
    fn->anchor = tcc_state->diag_anchor;
    fn->sym = sym;
    dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
    save_function_body(&fn->func_str, &fn->pack, 0, sym);
    /* -Wunused-function/-variable: the statics it names are used, whether
       or not anything ever parses the body */
    if (tcc_state->unused_on)
      unused_scan_tokens(fn->func_str, sym, 0);

    /* An explicit `inline` used to *disable* the inliner: this branch
     * saved the body and deferred emission, but never set
     * func_auto_inline, so every call site emitted a plain call.  Only
     * functions the user did NOT mark inline reached the auto-inline
     * path below.  That is backwards, and it costs the most in exactly
     * the code that relies on the idiom -- header-defined `static
     * inline` helpers.  lib/fp/soft is the extreme case: every helper in
     * soft_common.h is `static inline`, so __aeabi_dadd made 18 calls to
     * one-shift accessors that gcc folds into the body (3.76x on the
     * double benchmarks).
     *
     * Apply the same eligibility test the auto path uses and set the
     * flag so call sites replay the body.  Deferred emission is a
     * strict win over the auto path here: if every call site inlines
     * and the address is never taken, gen_inline_functions emits no
     * standalone copy at all.
     *
     * The gate must be token-length based.  gen_function's post-opt
     * revoke (IR > 8 / call-heavy) cannot help a deferred body -- it
     * compiles after the call sites have already been decided.
     *
     * STATIC ONLY.  A non-static `inline` is a C99 inline *definition*
     * (and gnu89 `extern inline` another rule again): the standalone
     * body is emitted on different terms, and marking it auto_inline
     * makes gen_inline_functions skip emission while some call site
     * still needs the symbol -- "undefined symbol 'add1_inline'".
     * Static inline has internal linkage, so dropping the standalone
     * copy once every site inlined is safe, and it is the idiom that
     * matters here (all of soft_common.h). */
    if (sym->type.ref && (sym->type.t & VT_STATIC) &&
        sym->type.ref->f.func_type != FUNC_ELLIPSIS &&
        !sym->type.ref->f.func_alwinl && !sym->type.ref->f.func_noinline &&
        (TCC_OPT(tcc_state, opt_inline_functions) || TCC_OPT(tcc_state, opt_inline_small)) &&
        tcc_state->nb_vla_param_exprs == 0 && fn->func_str)
    {
      int sig = auto_inline_sig_ok(sym);
      int thr = TCC_OPT(tcc_state, opt_inline_limit) > 0 ? TCC_OPT(tcc_state, opt_inline_limit)
                                                : (TCC_OPT(tcc_state, opt_inline_functions) ? 60 : 30);
      /* Void-returning with 64-bit params: same narrow cap as the auto
       * path (longer bodies trip an IR coalescing bug on narrowed locals). */
      if (sig == 2)
        thr = 15;
      /* The body compiles after its call sites have decided, so the class a plain
       * static gets from what it compiled to has to be judged from its tokens. */
      int small_leaf = 0;
      if (sig == 1 && fn->func_str->len > thr && fn->func_str->len <= STATIC_INLINE_LEAF_TOKENS &&
          !tcc_ir_opt_pass_disabled("inline:static_inline_leaf"))
      {
        const int ops = inline_body_leaf_ops(fn->func_str, 0);
        small_leaf = ops >= 0 && ops <= STATIC_INLINE_LEAF_EST_OPS;
      }
      if (sig && (fn->func_str->len <= thr || small_leaf) && !inline_body_has_apply_args(fn->func_str) &&
          !inline_body_has_unsafe_loops(fn->func_str) && !inline_body_has_static_local(fn->func_str))
      {
        sym->type.ref->f.func_auto_inline = 1;
        sym->type.ref->f.func_small_leaf = small_leaf;
        /* Body is still owed to gen_inline_functions, unlike the auto
         * path below which compiles the standalone copy right here. */
        sym->type.ref->f.func_deferred_inline = 1;
      }
    }

    /* Scan saved token stream for __builtin_va_arg_pack() usage.
     * If found, mark the function so call sites can expand it. */
    if (fn->func_str && sym->type.ref)
    {
      const int *p = tok_str_buf(fn->func_str);
      while (*p != TOK_EOF && *p != 0)
      {
        if (*p == TOK_builtin_va_arg_pack)
        {
          sym->type.ref->f.func_va_arg_pack = 1;
          break;
        }
        /* Skip token payload */
        int t = *p++;
        switch (t)
        {
        case TOK_CINT:
        case TOK_CUINT:
        case TOK_CCHAR:
        case TOK_LCHAR:
        case TOK_CFLOAT:
        case TOK_CFLOAT_I:
        case TOK_CINT_I:
        case TOK_LINENUM:
        case TOK_PACK_REPLAY:
#if LONG_SIZE == 4
        case TOK_CLONG:
        case TOK_CULONG:
#endif
          p++;
          break;
        case TOK_CDOUBLE:
        case TOK_CDOUBLE_I:
        case TOK_CLLONG:
        case TOK_CULLONG:
#if LONG_SIZE == 8
        case TOK_CLONG:
        case TOK_CULONG:
#endif
          p += 2;
          break;
        case TOK_CLDOUBLE:
        case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
          p += 2;
#elif LDOUBLE_SIZE == 12
          p += 3;
#elif LDOUBLE_SIZE == 16
          p += 4;
#endif
          break;
        case TOK_STR:
        case TOK_LSTR:
        case TOK_PPNUM:
        case TOK_PPSTR:
        {
          int sz = *p++;
          p += (sz + sizeof(int) - 1) / sizeof(int);
          break;
        }
        default:
          break;
        }
      }
    }
  }
  else if (sym->type.ref && sym->type.ref->f.func_type != FUNC_ELLIPSIS && !sym->type.ref->f.func_alwinl &&
           !sym->type.ref->f.func_noinline &&
           /* Only auto-inline functions whose signature is safe: scalar/pointer
            * params that fit in 32-bit registers, and scalar or struct return
            * types.  64-bit types and struct *parameters* are not handled.
            * Returns 2 for void+llong signatures (body-length gated below). */
           auto_inline_sig_ok(sym) && (TCC_OPT(tcc_state, opt_inline_functions) || TCC_OPT(tcc_state, opt_inline_small)) &&
           /* Don't auto-inline functions with VLA parameters: the VLA size
            * expressions (which may have side effects like i++) are evaluated
            * during function prolog, outside the saved body token stream.
            * Inlining would replay only the body, losing those side effects. */
           tcc_state->nb_vla_param_exprs == 0)
  {
    /* Auto-inline candidate: save the body as a token stream so call
     * sites within this TU can replay it.
     *
     * Static functions: defer standalone compilation; suppress it entirely
     *   if all call sites are inlined and address not taken. We set
     *   VT_INLINE so gen_inline_functions handles deferred emission.
     *
     * Non-static functions: MUST always have a globally-visible symbol for
     *   other TUs. We compile the standalone definition immediately via
     *   token-stream replay (same mechanism as gen_inline_functions), then
     *   keep the token stream in inline_fns for call-site inlining within
     *   this TU. VT_INLINE is NOT set so ELF linkage stays global. */
    struct InlineFunc *fn;
    fn = tcc_mallocz(sizeof *fn);
    fn->filename = tcc_filename_intern(file->filename);
    fn->anchor = tcc_state->diag_anchor;
    fn->sym = sym;
    fn->func_str = NULL;
    save_function_body(&fn->func_str, &fn->pack, 1, sym);

    int threshold = TCC_OPT(tcc_state, opt_inline_limit) > 0 ? TCC_OPT(tcc_state, opt_inline_limit)
                                                    : (TCC_OPT(tcc_state, opt_inline_functions) ? 60 : 30);
    int is_static = !!(sym->type.t & VT_STATIC);
    int body_len = fn->func_str ? fn->func_str->len : 0;

    if (TCC_LOG_INLINE_STRUCT)
      fprintf(stderr, "[auto-inline] candidate: %s  static=%d  len=%d  threshold=%d\n",
              get_tok_str(sym->v & ~SYM_FIELD, NULL), is_static, body_len, threshold);
    LOG_INLINE_STRUCT("[auto-inline] candidate: %s  static=%d  len=%d  threshold=%d  ret_btype=%d",
                      get_tok_str(sym->v & ~SYM_FIELD, NULL), is_static, body_len, threshold,
                      sym->type.ref ? (sym->type.ref->type.t & VT_BTYPE) : -1);

    Section *saved_text = cur_text_section;
    cur_text_section = ad_section ? ad_section : function_text_section(tcc_state, sym);
    if (cur_text_section->sh_num > bss_section->sh_num)
      cur_text_section->sh_flags = text_section->sh_flags;

    /* Void-returning functions with 64-bit params: only inline very
     * short bodies (≤ 15 tokens) — longer bodies may trigger an IR
     * coalescing bug with narrowed locals. */
    int void_llong_limit = (auto_inline_sig_ok(sym) == 2) ? 15 : threshold;
    if (fn->func_str && body_len <= void_llong_limit && !inline_body_has_apply_args(fn->func_str) &&
        !inline_body_has_unsafe_loops(fn->func_str))
    {
      if (TCC_LOG_INLINE_STRUCT)
        fprintf(stderr, "[auto-inline] SMALL: registering %s as inline candidate\n",
                get_tok_str(sym->v & ~SYM_FIELD, NULL));

      /* Small enough: register as inline candidate for call-site replay.
       *
       * We compile the standalone definition immediately for BOTH static
       * and non-static functions.  We deliberately do NOT set VT_INLINE:
       *   - Setting VT_INLINE would defer compilation to gen_inline_functions,
       *     but alias attributes and other code may reference sym->c before
       *     gen_inline_functions runs, causing "aliased to undefined symbol"
       *     errors and similar failures.
       *   - The standalone definition is always emitted.  Under
       *     -ffunction-sections it lands in its own .text.<name>
       *     section, and the linker's gc_sections() drops it when
       *     every call site was inlined and the address never taken.
       *     (Before function-sections was real, this sentence claimed
       *     --gc-sections would collect it out of the monolithic
       *     .text, which was never true -- section-granularity GC
       *     cannot drop a function from a section it shares.)
       *
       * fn->func_str is preserved (not consumed) so call-site replay
       * can still inline the body later.  The compilation uses an owning
       * COPY of the token stream.
       *
       * Save/restore tok+tokc: the replay leaves tok=TOK_EOF which would
       * cause the outer decl() loop to stop parsing prematurely. */
      sym->type.ref->f.func_auto_inline = 1;
      /* Set VT_INLINE for static functions so can_inline_eval=1 at call sites.
       * Non-static functions must NOT get VT_INLINE — their standalone definition
       * must remain globally visible for other translation units. */
      if (is_static)
        sym->type.t |= VT_INLINE;
      dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);

      TokenString *compile_ts = tok_str_borrow(fn->func_str, body_len);
      compile_ts->owner = fn->func_str; /* for tok_str_release_parsed */
      int saved_outer_tok = tok;
      CValue saved_outer_tokc = tokc;
      if (TCC_LOG_INLINE_STRUCT)
        fprintf(stderr, "[auto-inline] SMALL: compiling standalone for %s\n",
                get_tok_str(sym->v & ~SYM_FIELD, NULL));
      tcc_state->had_nested_funcs = 0;
      TokenString *saved_replay = tcc_state->compile_replay;
      tcc_state->compile_replay = compile_ts;
      begin_macro(compile_ts, 2); /* borrowed tokens: only the header is freed */
      next();
      gen_function(sym);
      end_macro();
      tcc_state->compile_replay = saved_replay;
      tok = saved_outer_tok;
      tokc = saved_outer_tokc;
      /* Revoke auto-inline for functions that contain nested function
       * definitions — their closure/trampoline semantics cannot be
       * replicated by token-replay inline expansion. */
      if (tcc_state->had_nested_funcs)
      {
        sym->type.ref->f.func_auto_inline = 0;
        if (is_static)
          sym->type.t &= ~VT_INLINE;
      }
      /* gen_function's post-opt check will revoke auto_inline
       * if the compiled IR exceeds 8 instructions. */
      /* If auto_inline was revoked (either by had_nested_funcs above or
       * by gen_function's post-opt size/call-count check), the function
       * is already compiled standalone and won't be inlined at any
       * callsite.  If the body is pure and within the eval-only cap,
       * promote to func_eval_only_inline so try_inline_const_eval can
       * still fold all-constant calls (mirrors the TOO-LARGE branch's
       * retroactive promotion below).  Otherwise free the token stream
       * so gen_inline_functions doesn't re-emit a duplicate body.
       * Skip the promotion path when nested funcs revoked: token-replay
       * cannot reproduce closure/trampoline semantics. */
      if (!sym->type.ref->f.func_auto_inline)
      {
        /* A register-only loop body (func_const_arg_loop, set from the
         * optimized IR) is exempt from the no-side-effects rule: the
         * counter and accumulator updates that trip it (`i++`, `n += x`)
         * are exactly what a loop is made of, and the loop is why the
         * body was demoted.  CTFE re-checks side effects before it
         * evaluates, so only the all-constant-argument replay path gains
         * these — where ssa:loop_const_sim collapses the expanded loop to
         * its final value. */
        int loop_helper = sym->type.ref->f.func_const_arg_loop;
        int promote_eval_only = !tcc_state->had_nested_funcs && fn->func_str && body_len <= 160 &&
                                !inline_body_has_apply_args(fn->func_str) &&
                                (loop_helper || !inline_body_has_side_effects(fn->func_str));
        if (promote_eval_only)
        {
          sym->type.ref->f.func_eval_only_inline = 1;
          /* VT_INLINE already set above for static; keep it set so
           * gen_inline_functions' eval-only skip branch catches us. */
        }
        else if (sym->type.ref->f.func_late_reopt ||
                 sym->type.ref->f.func_keep_tokens_for_noreturn)
        {
          /* Keep tokens — end-of-TU late_reopt may re-compile (either
           * already flagged, or pending noreturn-propagation decision). */
        }
        else if (sym->type.ref->f.tu_static_writer)
        {
          /* Keep tokens — function writes >=1 non-const static global;
           * end-of-TU TU-wide DSE analysis may decide to re-compile via
           * late_reopt to eliminate dead static stores. */
        }
        else
        {
          if (is_static)
            sym->type.t &= ~VT_INLINE;
          if (fn->func_str)
          {
            inline_fn_drop_tokens(fn);
            fn->func_str = NULL;
          }
          fn->sym = NULL;
        }
      }
      if (TCC_LOG_INLINE_STRUCT)
        fprintf(stderr, "[auto-inline] SMALL: done compiling %s sym->c=%d\n",
                get_tok_str(sym->v & ~SYM_FIELD, NULL), sym->c);
    }
    else
    {
      /* Too large to inline-expand, but if the body is pure and under
       * a higher eval-only cap, keep the token stream so
       * try_inline_const_eval can still fold all-constant calls.
       * Regular inline-expansion paths skip functions tagged
       * func_eval_only_inline. */
      const int eval_only_cap = 160;
      int has_apply = fn->func_str ? inline_body_has_apply_args(fn->func_str) : 0;
      int has_side = fn->func_str ? inline_body_has_side_effects(fn->func_str) : 1;
      int eval_only_candidate = fn->func_str && body_len <= eval_only_cap && !has_apply && !has_side;

      if (TCC_LOG_INLINE_STRUCT)
        fprintf(
            stderr,
            "[auto-inline] TOO LARGE: compiling %s normally (len=%d > threshold=%d) cap=%d apply=%d side=%d%s\n",
            get_tok_str(sym->v & ~SYM_FIELD, NULL), body_len, threshold, eval_only_cap, has_apply, has_side,
            eval_only_candidate ? " (eval-only retained)" : "");

      if (eval_only_candidate)
      {
        sym->type.ref->f.func_eval_only_inline = 1;
        if (is_static)
          sym->type.t |= VT_INLINE;
        dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);

        TokenString *compile_ts = tok_str_borrow(fn->func_str, body_len);
        compile_ts->owner = fn->func_str; /* for tok_str_release_parsed */

        int saved_outer_tok = tok;
        CValue saved_outer_tokc = tokc;
        TokenString *saved_replay = tcc_state->compile_replay;
        tcc_state->compile_replay = compile_ts;
        begin_macro(compile_ts, 2); /* borrowed tokens: only the header is freed */
        next();
        gen_function(sym);
        end_macro();
        tcc_state->compile_replay = saved_replay;
        tok = saved_outer_tok;
        tokc = saved_outer_tokc;
        /* Promoted as a small straight-line leaf: inline it at every call
         * site, not only the all-constant ones. */
        if (sym->type.ref->f.func_small_leaf)
          sym->type.ref->f.func_eval_only_inline = 0;
      }
      else if (fn->func_str)
      {
        int saved_outer_tok = tok;
        CValue saved_outer_tokc = tokc;
        const int post_opt_inline_cap = 512;
        if (body_len <= post_opt_inline_cap)
        {
          /* Preserve token stream for post-optimization re-inlining.
           * Compile from a copy (same pattern as the small-function path). */
          dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
          TokenString *compile_ts = tok_str_borrow(fn->func_str, body_len);
          compile_ts->owner = fn->func_str; /* for tok_str_release_parsed */
          TokenString *saved_replay = tcc_state->compile_replay;
          tcc_state->compile_replay = compile_ts;
          begin_macro(compile_ts, 2); /* borrowed tokens: only the header is freed */
          next();
          gen_function(sym);
          end_macro();
          tcc_state->compile_replay = saved_replay;
          if (sym->type.ref->f.func_small_leaf || sym->type.ref->f.func_inl_few_callers) {
            /* Promoted as a small straight-line leaf (see gen_function):
             * its optimized body is what gets duplicated, not its tokens,
             * so it inlines at every call site.  A few-callers leaf inlines
             * at each of its few sites. */
          } else if (sym->type.ref->f.func_auto_inline) {
            /* Retroactively promoted: convert to eval-only so callsite
             * inlining only fires when ALL args are compile-time constants.
             * The original body is too large for unconditional inlining. */
            sym->type.ref->f.func_auto_inline = 0;
            sym->type.ref->f.func_eval_only_inline = 1;
          } else if (sym->type.ref->f.func_late_reopt ||
                     sym->type.ref->f.func_keep_tokens_for_noreturn) {
            /* Keep tokens — end-of-TU late_reopt may re-compile. */
          } else if (sym->type.ref->f.tu_static_writer) {
            /* Keep tokens — TU-wide DSE may flag this for re-compile. */
          } else {
            /* Not promoted: prevent gen_inline_functions re-compilation. */
            inline_fn_drop_tokens(fn);
            fn->func_str = NULL;
            fn->sym = NULL;
          }
        }
        else
        {
          /* Body exceeds post_opt_inline_cap.  If the function writes a
           * non-const static, we still need its tokens for end-of-TU
           * re-compilation; preserve them by going through the same
           * inline_fns path used for the smaller-body case. */
          if (TCC_OPT(tcc_state, opt_dead_store)) {
            TokenString *compile_ts = tok_str_borrow(fn->func_str, body_len);
            compile_ts->owner = fn->func_str; /* for tok_str_release_parsed */
            dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
            TokenString *saved_replay = tcc_state->compile_replay;
            tcc_state->compile_replay = compile_ts;
            begin_macro(compile_ts, 2); /* borrowed tokens: only the header is freed */
            next();
            gen_function(sym);
            end_macro();
            tcc_state->compile_replay = saved_replay;
            if (!sym->type.ref->f.tu_static_writer &&
                !sym->type.ref->f.func_late_reopt &&
                !sym->type.ref->f.func_keep_tokens_for_noreturn &&
                /* A small straight-line leaf inlines at every call site
                 * whatever its source length: its optimized size (a few
                 * ops) is what gets duplicated.  Zig's MultiArrayList.slice
                 * is 536 tokens and 8 ops once its loop unrolls. */
                !sym->type.ref->f.func_small_leaf) {
              /* Not a static writer — discard tokens to save memory. */
              inline_fn_drop_tokens(fn);
              fn->func_str = NULL;
              fn->sym = NULL;
            }
          } else {
            TokenString *ts = fn->func_str;
            fn->func_str = NULL;
            begin_macro_kept(ts);
            next();
            gen_function(sym);
            end_macro();
            tcc_free(fn->pack);
            inline_func_free_rare(fn);
            tcc_free(fn);
          }
        }
        tok = saved_outer_tok;
        tokc = saved_outer_tokc;
      }
      else
      {
        tcc_free(fn->pack);
        inline_func_free_rare(fn);
        tcc_free(fn);
      }
    }

    cur_text_section = saved_text;
  }
  else
  {
    /* compute text section */
    cur_text_section = ad_section;
    if (!cur_text_section)
      cur_text_section = function_text_section(tcc_state, sym);
    else if (cur_text_section->sh_num > bss_section->sh_num)
      cur_text_section->sh_flags = text_section->sh_flags;
    /* When -fdead-store-elimination is enabled, save the body as a
     * token stream so the end-of-TU late_reopt pass can re-compile
     * this function if TU-wide analysis flags it as a writer of a
     * static global with no reachable readers.  This path covers
     * functions that auto_inline_sig_ok rejects (e.g., double/long
     * double params), which would otherwise never reach gen_late_reopt
     * because they are not in inline_fns.  Bound the saved body
     * length so very large functions don't pin extra memory. */
    const int late_reopt_cap = 512;
    if (TCC_OPT(tcc_state, opt_dead_store))
    {
      struct InlineFunc *fn = tcc_mallocz(sizeof *fn);
      fn->filename = tcc_filename_intern(file->filename);
    fn->anchor = tcc_state->diag_anchor;
      fn->sym = sym;
      fn->func_str = NULL;
      save_function_body(&fn->func_str, &fn->pack, 1, sym);
      int body_len = fn->func_str ? fn->func_str->len : 0;
      if (fn->func_str && body_len > 0 && body_len <= late_reopt_cap)
      {
        dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
        TokenString *compile_ts = tok_str_borrow(fn->func_str, body_len);
        compile_ts->owner = fn->func_str; /* for tok_str_release_parsed */
        int saved_outer_tok = tok;
        CValue saved_outer_tokc = tokc;
        tcc_state->had_nested_funcs = 0;
        TokenString *saved_replay = tcc_state->compile_replay;
        tcc_state->compile_replay = compile_ts;
        begin_macro(compile_ts, 2); /* borrowed tokens: only the header is freed */
        next();
        gen_function(sym);
        end_macro();
        tcc_state->compile_replay = saved_replay;
        tok = saved_outer_tok;
        tokc = saved_outer_tokc;
        /* Token-replay cannot reproduce nested function closure
         * semantics during re-compile; drop tokens in that case.
         * Otherwise keep only bodies that either write statics for
         * TU-wide DSE or were speculatively preserved for call-fact
         * late reopt. */
        if (tcc_state->had_nested_funcs ||
            (!sym->type.ref->f.tu_static_writer &&
             !sym->type.ref->f.func_keep_tokens_for_noreturn &&
             !sym->type.ref->f.func_late_reopt))
        {
          inline_fn_drop_tokens(fn);
          fn->func_str = NULL;
          fn->sym = NULL;
        }
      }
      else
      {
        /* Body empty or exceeds the cap: compile via replay (if we
         * have a saved stream) without inline_fns preservation. */
        if (fn->func_str)
        {
          int saved_outer_tok2 = tok;
          CValue saved_outer_tokc2 = tokc;
          TokenString *ts = fn->func_str;
          fn->func_str = NULL;
          begin_macro_kept(ts);
          next();
          gen_function(sym);
          end_macro();
          tok = saved_outer_tok2;
          tokc = saved_outer_tokc2;
        }
        else
        {
          gen_function(sym);
        }
        tcc_free(fn->pack);
        inline_func_free_rare(fn);
        tcc_free(fn);
      }
    }
    else
    {
      gen_function(sym);
    }
    /* Nested functions are now compiled inside gen_function,
     * before pop_local_syms, so parent locals are still accessible. */
  }
}

/* -finline-functions-called-once: save a function's body instead of
 * generating it, so the end of the TU (gen_deferred_function_bodies) knows
 * every body -- and every call site -- before it generates any of them.  A
 * non-static function is deferred too: it is always emitted, but a caller
 * compiled at its definition could inline nothing defined further down.  A
 * static inline, an old-style or variadic definition, and one whose VLA
 * parameters evaluate expressions at the definition are left alone. */
/* Whether T, the token before a name followed by '(', ends the declaration
 * specifiers of a block-scope declaration (`zig_extern zig_i128 __divti3(...);`
 * in zig.h): a type keyword or a typedef name.  The name is being declared,
 * with its prototype, not called. */
static int tok_ends_decl_specifiers(int t)
{
  switch (t)
  {
  case TOK_VOID: case TOK_CHAR: case TOK_INT: case TOK_FLOAT: case TOK_DOUBLE: case TOK_BOOL:
  case TOK_SHORT: case TOK_LONG: case TOK_UNSIGNED: case TOK_SIGNED1: case TOK_SIGNED2: case TOK_SIGNED3:
  case TOK_CONST1: case TOK_CONST2: case TOK_CONST3:
  case TOK_VOLATILE1: case TOK_VOLATILE2: case TOK_VOLATILE3:
    return 1;
  }
  if (t < TOK_UIDENT)
    return 0;
  Sym *s = sym_find(t);
  return s && (s->type.t & VT_TYPEDEF);
}

/* Whether BODY calls a function that has no prototype here: an identifier
 * with no declaration in scope (an implicit declaration), or one declared
 * without a prototype.  Replayed at the end of the TU, the call would be
 * checked against a prototype that only appears further down -- pr47428's
 * `fn (0)` became "too few arguments".  Token replay cannot restore the scope
 * as of the definition, so such a body is generated here instead.  An
 * identifier that is a parameter, or that the body mentioned before (a local
 * function pointer), is not a file-scope reference; nor is a predefined token
 * (a builtin unary() expands itself, or a library name like memcpy), nor a
 * struct/union/enum tag. */
static int body_calls_unprototyped(Sym *sym, TokenString *body)
{
  const int *start = tok_str_buf(body), *p = start, *end = start + body->len;
  int prev = 0;
  CValue cv;
  while (p < end)
  {
    int t;
    const int *at = p, *next_tok;
    tok_get(&t, &p, &cv);
    for (next_tok = p; next_tok < end && *next_tok == TOK_LINENUM;)
      next_tok += 2;
    /* A tag after struct/union/enum names a type: `struct T (*fp)(void)`. */
    if (t >= TOK_UIDENT && prev != '.' && prev != TOK_ARROW && prev != TOK_STRUCT && prev != TOK_UNION &&
        prev != TOK_ENUM && next_tok < end && *next_tok == '(')
    {
      Sym *s = sym_find(t);
      if (s && !IS_ASM_SYM(s))
      {
        if ((s->type.t & VT_BTYPE) == VT_FUNC && s->type.ref->f.func_type == FUNC_OLD)
          return 1;
      }
      else if (t >= TOK_BUILTIN_END)
      {
        int local = tok_ends_decl_specifiers(prev);
        for (Sym *param = sym_first(sym->type.ref); param && !local; param = param->next)
          local = (param->v & ~SYM_FIELD) == t;
        for (const int *q = start; q < at && !local;)
        {
          int u;
          CValue ucv;
          tok_get(&u, &q, &ucv);
          local = u == t;
        }
        if (!local)
          return 1;
      }
    }
    if (t != TOK_LINENUM)
      prev = t;
  }
  return 0;
}

/* At a function definition, before it merges into the declarations before
 * it: -Wstrict-prototypes, a definition that is not a prototype (an old-style
 * one, or `()` before C23) with no prototype before it; -Wmissing-prototypes,
 * a global function (not main, not inline) with no prototype before it --
 * how a header and its source drift apart.  A yasc module's export is its
 * declaration (the interface is made from it). */
static void definition_prototype_checks(int v, CType *type, int line, int in_module)
{
  Sym *prev = sym_find(v);
  int proto_before;
  while (prev && prev->sym_scope)
    prev = prev->prev_tok;
  proto_before = prev && (prev->type.t & VT_BTYPE) == VT_FUNC && prev->type.ref->f.func_type != FUNC_OLD;
  if (proto_before)
    return;
  if (type->ref->f.func_type == FUNC_OLD)
    tcc_warning_once_at(warn_strict_prototypes, line, "function declaration isn't a prototype");
  if (!in_module && !(type->t & (VT_STATIC | VT_INLINE)) && !type->ref->f.func_rewritten_extern_inline &&
      strcmp(get_tok_str(v, NULL), "main"))
    tcc_warning_once_at(warn_missing_prototypes, line, "no previous prototype for '%s'", get_tok_str(v, NULL));
}

static int defer_function_body(Sym *sym, Section *section)
{
  if (!(TCC_OPT(tcc_state, opt_inline_called_once) || TCC_OPT(tcc_state, opt_drop_unused_statics)) || tcc_state->nb_vla_param_exprs ||
      (sym->type.t & VT_INLINE) || !sym->type.ref || sym->type.ref->f.func_type != FUNC_NEW || sym->a.nested_func)
    return 0;
  DeferredFunc *d = tcc_mallocz(sizeof *d);
  d->filename = tcc_filename_intern(file->filename);
  d->anchor = tcc_state->diag_anchor;
  d->sym = sym;
  d->section = section;
  tcc_state->save_body_deferred = d;
  save_function_body(&d->body, &d->pack, 0, sym);
  tcc_state->save_body_deferred = NULL;
  int unprototyped = body_calls_unprototyped(sym, d->body);
  tok_str_release(d->body);
  if (unprototyped)
  {
    /* Generate it now, from the saved tokens, as if never deferred. */
    int saved_tok = tok, saved_line = file->line_num;
    CValue saved_tokc = tokc;
    define_deferred_function(d);
    tok = saved_tok;
    tokc = saved_tokc;
    file->line_num = saved_line;
    tcc_free(d->pack);
    tcc_free(d);
    return 1;
  }
  dynarray_add(&tcc_state->deferred_fns, &tcc_state->nb_deferred_fns, d);
  return 1;
}

/* Generate a body defer_function_body saved: replay its tokens through the
 * definition path it skipped. */
void define_deferred_function(DeferredFunc *d)
{
  TokenString *body = d->body;
  int saved_pack[PACK_STACK_SIZE + 1];
  int saved_budget = called_once_budget_begin(body->len);
  Sym *saved_sym = tcc_state->deferred_sym;
  int saved_sites = tcc_state->deferred_call_sites;
  const int saved_anchor = tcc_diag_anchor_enter(tcc_state, d->anchor);
  tcc_state->deferred_sym = d->sym;
  tcc_state->deferred_call_sites = d->call_sites;
  d->body = NULL;
  tccpp_setfile_replay(d->filename);
  pp_pack_enter(tcc_state, d->pack, saved_pack);
  /* Replayed through a borrowed header, so save_function_body can adopt the
   * body itself; if nothing does, it goes here as before. */
  TokenString *saved_deferred = tcc_state->deferred_body;
  int saved_adopted = tcc_state->deferred_adopted;
  tcc_state->deferred_body = body;
  tcc_state->deferred_adopted = 0;
  begin_macro(tok_str_borrow(body, body->len), 2);
  next();
  define_global_function(d->sym, d->section);
  end_macro();
  if (tcc_state->deferred_adopted != 1)
    tok_str_free(body); /* not adopted, or adopted and dropped */
  tcc_state->deferred_body = saved_deferred;
  tcc_state->deferred_adopted = saved_adopted;
  pp_pack_leave(tcc_state, saved_pack);
  tcc_state->deferred_sym = saved_sym;
  tcc_state->deferred_call_sites = saved_sites;
  tcc_state->diag_anchor = saved_anchor;
  called_once_budget_end(saved_budget);
}

/* A copy of STR, to replay without consuming it. */
TokenString *tok_str_clone(TokenString *str)
{
  TokenString *copy = tok_str_alloc();
  for (const int *p = tok_str_buf(str), *end = p + str->len; p < end; p++)
    tok_str_add(copy, *p);
  tok_str_sys_copy(str, copy);
  return copy;
}

/* Complete TYPE, an array whose size its initializer decides, from the saved
 * tokens INIT: the size-only pass decl_initializer_alloc runs first, over a
 * copy, so INIT stays whole for the replay. */
static void size_array_from_saved_init(CType *type, TokenString *init)
{
  TokenString *copy = tok_str_clone(init);
  int saved_tok = tok, saved_line = file->line_num, saved_nocode_wanted = nocode_wanted;
  CValue saved_tokc = tokc;
  init_params ip = {0};
  /* Unshare the size-holding ref, as decl_initializer_alloc does: a typedef
   * of an unsized array keeps its own. */
  type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
  ip.flex_array_ref = type->ref;
  nocode_wanted |= DATA_ONLY_WANTED;
  begin_macro(copy, 1);
  next();
  decl_initializer(&ip, type, 0, DIF_FIRST | DIF_SIZE_ONLY, -1);
  end_macro();
  nocode_wanted = saved_nocode_wanted;
  tok = saved_tok;
  tokc = saved_tokc;
  file->line_num = saved_line;
}

/* -fdrop-unused-statics: save the initializer of a file-scope static object
 * instead of parsing it, so the end of the TU (prune_unused_statics) emits it
 * only if something live refers to it.  The symbol is declared now, with its
 * complete type, so every later mention resolves; until it is defined it is
 * undefined (or still tentative), which no initializer-reading fold trusts.
 * An object whose size its initializer decides, or that carries attributes
 * about where or whether it is emitted, is defined here as before. */
static int defer_static_data(CType *type, AttributeDef *ad, int r, int v)
{
  int align;
  if (!TCC_OPT(tcc_state, opt_drop_unused_statics) || !(type->t & VT_STATIC) || (type->t & VT_EXTERN) || ad->section ||
      ad->a.used || ad->alias_target || ad->asm_label || ad->cleanup_func)
    return 0;
  const int unsized = type_size(type, &align) < 0;
  if (unsized && (!(type->t & VT_ARRAY) || type_size(&type->ref->type, &align) < 0))
    return 0;
#ifdef CONFIG_TCC_BCHECK
  if (tcc_state->do_bounds_check)
    return 0;
#endif
  if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    Sym *field = sym_first(type->ref);
    while (field && field->next)
      field = field->next;
    if (field && (field->type.t & VT_ARRAY) && field->type.ref->c < 0)
      return 0; /* a flexible array member: the initializer sizes it */
  }
  Sym *sym = sym_find(v);
  if (sym)
  {
    ElfSym *esym = sym->c ? elfsym(sym) : NULL;
    if (!(sym->type.t & VT_EXTERN) || (esym && esym->st_shndx != SHN_UNDEF && esym->st_shndx != SHN_COMMON))
      return 0; /* already defined: let the definition path report it */
  }
  /* an aggregate's initializer not in braces (nor a string, nor a compound
     literal) is an error: said now, as the definition path says it (saved,
     the ',' after its first element would end it) */
  if ((type->t & VT_ARRAY || (type->t & VT_BTYPE) == VT_STRUCT) && !(type->t & VT_VECTOR) && tok != '{' &&
      tok != TOK_STR && tok != TOK_LSTR && tok != '(')
    return 0;
  TokenString *init;
  skip_save_stop_semicolon = 1; /* a ';' right in its braces ends it: no initializer holds one */
  skip_or_save_block(&init);
  skip_save_stop_semicolon = 0;
  if (unsized)
  {
    size_array_from_saved_init(type, init);
    if (type_size(type, &align) < 0)
      tcc_error("unknown type size");
  }
  /* Declared, not defined, until the replay: the way a tentative static
   * is (decl() marks an uninitialized one VT_EXTERN). */
  CType declared = *type;
  declared.t |= VT_EXTERN;
  if (sym)
    patch_storage(sym, ad, &declared);
  else
  {
    sym = sym_push(v, &declared, r | VT_SYM, 0);
    patch_storage(sym, ad, NULL);
  }
  DeferredData *d = tcc_mallocz(sizeof *d);
  d->filename = tcc_filename_intern(file->filename);
  d->anchor = tcc_state->diag_anchor;
  d->sym = sym;
  d->type = *type;
  d->ad = *ad;
  d->r = r;
  /* As a saved body is: a 16-byte header, the tokens in the spill file. */
  init = tok_str_keep(init);
  tok_str_spill(init);
  d->init = init;
  dynarray_add(&tcc_state->deferred_data, &tcc_state->nb_deferred_data, d);
  tcc_diag_queue_begin(tcc_state); /* its initializer speaks at the end of the unit */
  if (tcc_state->diag_in_parse) /* before what the parse says after it (driver/diag_out.c) */
    tcc_state->diag_anchor = ++tcc_state->diag_decl_seq;
  return 1;
}

/* Define an object defer_static_data saved: replay its initializer through
 * the allocation it skipped. */
static void define_deferred_data_replay(void *arg)
{
  DeferredData *d = arg;
  TokenString *init = d->init;
  d->init = NULL;
  begin_macro_kept(init);
  next();
  decl_initializer_alloc(&d->type, &d->ad, d->r, 1, d->sym->v, 1);
  if (tok != TOK_EOF)
    expect("';'");
  end_macro();
}

/* An error in the initializer ends its replay; the next one goes on
   (core/recover.c). */
void define_deferred_data(DeferredData *d)
{
  const int anchor = tcc_diag_anchor_enter(tcc_state, d->anchor);
  tccpp_setfile_replay(d->filename);
  replay_recovering(define_deferred_data_replay, d);
  tcc_state->diag_anchor = anchor;
}

/* Parse the initializer of an object prune_unused_statics dropped, for its
 * diagnostics only: an invalid initializer is an error whether or not the
 * object is used (96_nodata_wanted).  Under check_only nothing reaches the
 * object file; the initializer is laid out in check_scratch. */
typedef struct
{
  DeferredData *d;
  TokenString *init;
} DroppedDataReplay;

static void check_dropped_deferred_data_replay(void *arg)
{
  DroppedDataReplay *a = arg;
  int align, size = type_size(&a->d->type, &align);
  init_params p = {0};
  p.sec = tcc_state->check_scratch;
  const unsigned long addr = section_add(p.sec, size > 0 ? size : 0, align > 0 ? align : 1);
  begin_macro_kept(a->init);
  next();
  decl_initializer(&p, &a->d->type, addr, DIF_FIRST, -1);
  init_ovr_free(&p);
  if (tok != TOK_EOF)
    expect("';'");
  end_macro();
}

void check_dropped_deferred_data(DeferredData *d, TokenString *init)
{
  DroppedDataReplay a = {d, init};
  const int saved_nocode_wanted = nocode_wanted;
  nocode_wanted = DATA_ONLY_WANTED;
  const int anchor = tcc_diag_anchor_enter(tcc_state, d->anchor);
  tcc_state->check_only++;
  tccpp_setfile_replay(d->filename);
  replay_recovering(check_dropped_deferred_data_replay, &a); /* goes on after an error */
  tcc_state->check_only--;
  tcc_state->diag_anchor = anchor;
  nocode_wanted = saved_nocode_wanted;
}

/* The same for a function body (gen_function_check_only). */
void check_dropped_function(DeferredFunc *d, TokenString *body)
{
  int saved_pack[PACK_STACK_SIZE + 1];
  const int anchor = tcc_diag_anchor_enter(tcc_state, d->anchor);
  tccpp_setfile_replay(d->filename);
  pp_pack_enter(tcc_state, d->pack, saved_pack);
  begin_macro_kept(body);
  next();
  gen_function_check_only(d->sym);
  end_macro();
  pp_pack_leave(tcc_state, saved_pack);
  tcc_state->diag_anchor = anchor;
}

/* -Wunused-*: the variable V just declared, at LINE: a local (or static
   local) followed to its scope's end, a file-scope static to the unit's */
static void unused_decl_var(int l, CType *type, AttributeDef *ad, int v, int line)
{
  Sym *sym = sym_find(v);
  if (!sym)
    return;
  if (l != VT_CONST)
    unused_declare_local(sym, line, ad->attr_unused || ad->a.used || ad->cleanup_func);
  else if (l == VT_CONST && (type->t & VT_STATIC))
  {
    if (ad->attr_unused)
      sym->ymod |= SYM_UNUSED_OK;
    if (!ad->a.used && !ad->section && !ad->alias_target)
      unused_declare_static(sym, line, 0);
  }
}

/* C23 `auto` / GNU `__auto_type` (ad->auto_tok): TYPE becomes the type of
   the initializer after lvalue conversion -- arrays and functions decay,
   top-level qualifiers go -- with the qualifiers and storage class written
   in the declaration specifiers.  The initializer is read once unevaluated
   for its type, as typeof reads its operand, and the declaration then goes
   on as if the type had been written: tok is '=' again, its tokens follow,
   and the ',' or ';' after them.  In yasc the pointer's qualifiers (owned,
   maybe, view) come with the type; a copy of an owned variable, member or
   dereference is an error, since nothing says whether it moves or is
   borrowed.  Any other owned value (a call's result, a cast of one) is taken
   as `T * owned q = ...` takes it. */
static void decl_auto_infer(CType *type, CType *btype, AttributeDef *ad, int v, int first)
{
  const char *kw = get_tok_str(ad->auto_tok, NULL);
  TokenString *init, *view;
  CType it;
  Sym *src;
  int q, lval, *buf;

  if (type->t & VT_TYPEDEF)
    tcc_error("'%s' declares a variable, not a typedef", kw);
  if (!v || type->t != btype->t || type->ref != btype->ref)
    tcc_error("'%s' declares a plain name: the type comes from the initializer", kw);
  if (!first)
    tcc_error("'%s' declares one variable at a time", kw);
  if (tok != '=')
    tcc_error("'%s' needs an initializer to take the type from", kw);
  next();
  if (tok == '{')
    tcc_error("'%s' takes the type of an expression, not of a braced initializer", kw);
  skip_or_save_block(&init); /* up to the ',' or ';', which is kept for later */
  unget_tok(0);

  view = tok_str_borrow(init, init->len);
  begin_macro(view, 2);
  next();
  nocode_wanted++;
  unevaluated_operand++;
  expr_eq();
  if (tok != TOK_EOF)
    expect("',' or ';'");
  it = vtop->type;
  src = yasc_vtop_var();
  lval = vtop->r & VT_LVAL;
  vpop();
  unevaluated_operand--;
  nocode_wanted--;
  end_macro_to(view);

  if (it.t & VT_ARRAY)
  {
    it = it.ref->type;
    mk_pointer(&it);
  }
  else if ((it.t & VT_BTYPE) == VT_FUNC)
    mk_pointer(&it);
  else if (it.t & VT_BITFIELD)
    it.t &= ~VT_STRUCT_MASK;
  if ((it.t & VT_BTYPE) == VT_VOID)
    tcc_error("'%s' cannot take the type void", kw);
  it.t &= ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE | VT_RODATA_REL);

  q = type_yasc(&it);
  if ((q & YASC_OWNED) && !(q & (YASC_FRESH | YASC_UNCHECKED)) && lval)
  {
    if (src)
      tcc_error("'%s' would copy the owned pointer '%s': write move(%s) or borrow(%s)", kw, get_tok_str(src->v, NULL),
                get_tok_str(src->v, NULL), get_tok_str(src->v, NULL));
    tcc_error("'%s' would copy an owned pointer: write move(...) or borrow(...)", kw);
  }
  if (q & YASC_FRESH)
    yasc_requalify(&it, q & ~YASC_FRESH);

  if (btype->t & (VT_CONSTANT | VT_VOLATILE))
    parse_btype_qualify(&it, btype->t & (VT_CONSTANT | VT_VOLATILE));
  it.t |= btype->t & VT_STORAGE;
  *type = it;

  /* the initializer again, now for real: the replay ends where the saved
     tokens do (0 instead of TOK_EOF) and the ',' or ';' ungot above follows */
  buf = (int *)tok_str_buf(init);
  buf[init->len - 1] = 0;
  begin_macro(init, 1);
  tok = '=';
}

/* 'l' is VT_LOCAL or VT_CONST to define default storage type
   or VT_CMP if parsing old style parameter list
   or VT_JMP if parsing c99 for decl: for (int i = 0, ...) */
/* A nested function's assembler name, "name.N" (N counts every nested
   function of the unit).  Out of line: the buffer would sit in decl()'s
   frame, which a declaration's initializer puts on the parser's recursive
   path (calls in it, inline expansions). */
static __attribute__((noinline)) int nested_func_label(int v)
{
  static int nested_func_uid = 0;
  char mangled[256];
  snprintf(mangled, sizeof(mangled), "%s.%d", get_tok_str(v, NULL), nested_func_uid++);
  return tok_alloc(mangled, strlen(mangled))->tok;
}

RecoverDeclList *recover_decl_list;
RecoverDeclResume recover_decl_resume;

/* The declaration lists being parsed (an error in one may go on with its
   next declarator: core/recover.c), innermost first.  Their records are
   kept off decl()'s frame, which an initializer's calls and inline
   expansions put on the parser's recursive path: taken from a free list,
   given back when decl() returns (DECL_RETURN) or an error's recovery
   unwinds past them (decl_list_unwind). */
static RecoverDeclList *decl_list_free;

static __attribute__((noinline)) RecoverDeclList *decl_list_push(CType *btype, AttributeDef *adbase, int l)
{
  RecoverDeclList *dl = decl_list_free;
  if (dl)
    decl_list_free = dl->outer;
  else
    dl = tcc_malloc(sizeof *dl);
  memset(dl, 0, sizeof *dl);
  dl->outer = recover_decl_list;
  dl->btype = btype;
  dl->adbase = adbase;
  dl->l = l;
  recover_decl_list = dl;
  return dl;
}

/* back to MARK (a recovery point's), NULL: all of them */
void decl_list_unwind(RecoverDeclList *mark)
{
  while (recover_decl_list && recover_decl_list != mark)
  {
    RecoverDeclList *dl = recover_decl_list;
    recover_decl_list = dl->outer;
    dl->outer = decl_list_free;
    decl_list_free = dl;
  }
}

/* the end of the unit */
void decl_list_reset(void)
{
  decl_list_unwind(NULL);
  while (decl_list_free)
  {
    RecoverDeclList *dl = decl_list_free;
    decl_list_free = dl->outer;
    tcc_free(dl);
  }
}

/* decl()'s way out: a file-scope or block-scope list's record goes */
#define DECL_RETURN(x)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
    if (dl)                                                                                                            \
      decl_list_unwind(dl->outer);                                                                                     \
    return (x);                                                                                                        \
  } while (0)

int decl(int l)
{
  int v, has_init, r, oldint, align, exported, this_exported, in_module, ndecl;
  CType type, btype;
  Sym *sym;
  AttributeDef ad, adbase;
  /* the declaration list an error goes on in (core/recover.c) */
  RecoverDeclList *dl = l == VT_CONST || l == VT_LOCAL ? decl_list_push(&btype, &adbase, l) : NULL;
  while (1)
  {
    if (yasc_ntagged | yasc_nreg)
      yasc_stmt_forget(); /* `int k = *p;` is no copy of a tag read before it */
    if (l == VT_CONST && tcc_state->diag_in_parse)
    {
      /* a fatal error or the -fmax-errors cap stopped the parse: the bodies
         saved before it are still parsed (core/recover.c tcc_recover_stop) */
      if (tcc_state->diag_stop)
        DECL_RETURN(0);
      /* the next file-scope declaration: its diagnostics' anchor */
      if (!recover_decl_resume.on)
        tcc_state->diag_anchor = ++tcc_state->diag_decl_seq;
    }
    /* an error in the list's last declarator: it goes on with the next one
       (core/recover.c) */
    if (recover_decl_resume.on && recover_decl_resume.l == l && dl)
    {
      recover_decl_resume.on = 0;
      btype = recover_decl_resume.btype;
      adbase = recover_decl_resume.adbase;
      oldint = recover_decl_resume.oldint;
      exported = recover_decl_resume.exported;
      ndecl = 1;
      goto resume_list;
    }
    /* Between file-scope declarations no saved body is being replayed. */
    if (l == VT_CONST)
      {
      tok_str_release_spilled();
      sym_params_release();
    }

    exported = 0;
    if (l == VT_CONST)
    {
      /* yasc: the methods of the struct just defined (decl/method.c) */
      if (yasc_methods_pending())
        yasc_methods_flush();
      yasc_exporting = 0;
      if (tcc_state->yasc)
      {
        exported = yasc_file_scope_decl();
        if (exported == -2)
          DECL_RETURN(0); /* the end of an imported interface's headers */
        if (exported < 0)
          continue;
      }
    }

    oldint = 0;
    if (!parse_btype(&btype, &adbase, l == VT_LOCAL))
    {
      if (l == VT_JMP)
        DECL_RETURN(0);
      /* skip redundant ';' if not in old parameter decl scope */
      if (tok == ';' && l != VT_CMP)
      {
        next();
        continue;
      }
      if (tok_is_static_assert())
      {
        do_Static_assert();
        continue;
      }
      if (l != VT_CONST)
        break;
      if (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3)
      {
        /* global asm block */
        asm_global_instr();
        continue;
      }
      if (tok >= TOK_UIDENT || tok == '*' || tok == '(')
      {
        /* special test for old K&R protos without explicit int
           type. Only accepted when defining global data, including
           pointer or parenthesized declarators such as '*p;' or
           '(*fp)();'. */
        btype.t = VT_INT;
        oldint = 1;
      }
      else
      {
        if (tok != TOK_EOF)
          expect("declaration");
        break;
      }
    }

    if (tok == ';')
    {
      if ((btype.t & VT_BTYPE) == VT_STRUCT)
      {
        v = btype.ref->v;
        if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) >= SYM_FIRST_ANOM)
          tcc_warning("unnamed struct/union that defines no instances");
        next();
        continue;
      }
      if (IS_ENUM(btype.t))
      {
        next();
        continue;
      }
    }

    ndecl = 0;
  resume_list:
    if (dl)
    {
      dl->oldint = oldint;
      dl->exported = exported;
      dl->scope = cur_scope;
      dl->struct_depth = struct_body_depth;
      dl->active = 1;
    }
    for (;; ndecl++)
    { /* iterate thru each declaration */
      Sym *stack_before = global_stack;
      type = btype;
      ad = adbase;
      declarator_last_functype = NULL;
      patch_own_functype = NULL;
      /* the declarator's line, for the definition checks */
      const int decl_line = file->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr);
      /* a deprecated declaration names deprecated things quietly */
      if (adbase.attr_deprecated)
        attr_deprecated_suppress++;
      /* yasc: a local's plain pointer waits for its body (plain_param.c) */
      const int pp_local = l == VT_LOCAL && tcc_state->yasc ? yasc_pp_local_begin() : -1;
      if (dl)
        dl->init_base = init_brace_depth;
      /* an error in the declarator still declares its name (core/recover.c) */
      recover_decl.l = l == VT_CONST || l == VT_LOCAL ? l : 0;
      recover_decl.v = 0;
      recover_decl.storage = type.t & VT_STORAGE;
      type_decl(&type, &ad, &v, TYPE_DIRECT);
      recover_decl.l = 0;
      if (pp_local >= 0)
      {
        const int keep = !(type.t & (VT_TYPEDEF | VT_EXTERN | VT_STATIC)) && (type.t & VT_BTYPE) != VT_FUNC;
        yasc_pp_local_end(pp_local, v, keep);
        yasc_pp_redundant_view(&type, keep);
      }
      if (adbase.attr_deprecated)
        attr_deprecated_suppress--;
      /* yasc: a plain pointer parameter waits for its definition's body */
      yasc_pp_declarator(l == VT_CONST && tok == '{' && (type.t & VT_BTYPE) == VT_FUNC ? type.ref : NULL);
      if (ad.auto_tok)
        decl_auto_infer(&type, &btype, &ad, v, ndecl == 0);
      /* this declarator built the function type itself, not a typedef or typeof */
      const int own_functype = (type.t & VT_BTYPE) == VT_FUNC && type.ref && type.ref == declarator_last_functype;
      /* #pragma GCC visibility covers every declaration it encloses, extern
       * ones too; an attribute on the declaration wins.  Not typedefs: their
       * attributes merge into every later declaration that uses them. */
      if (tcc_state->visibility_stack_depth && !ad.a.vis_explicit && !(type.t & VT_TYPEDEF))
      {
        ad.a.visibility = tcc_state->visibility_stack[tcc_state->visibility_stack_depth - 1];
        ad.a.vis_explicit = 1;
      }
      /* yasc module: private unless exported (decl/module.c) */
      in_module = l == VT_CONST && tcc_state->yasc_module && yasc_in_main_file();
      this_exported = in_module ? yasc_module_linkage(&type, v, exported) : 0;
      if (ad.attr_mode && !(type.t & VT_VECTOR) && (btype.t & (VT_BTYPE | VT_LONG)) != (ad.attr_mode - 1))
      {
        int u = ad.attr_mode - 1;
        type.t = (type.t & ~(VT_BTYPE | VT_LONG)) | u;
      }
      /* Apply __attribute__((vector_size(N))) if it appeared after the declarator
       * name (e.g. "typedef int V2SI __attribute__((vector_size(8)))").
       * decl_spec_type handles it when the attribute precedes the name;
       * this covers the post-declarator position. */
      if (ad.vector_size && !(btype.t & VT_VECTOR) && !(type.t & VT_VECTOR))
      {
        int storage = type.t & VT_STORAGE;
        CType elem = {type.t & ~VT_STORAGE, type.ref};
        make_vector_type(&type, &elem, ad.vector_size);
        type.t |= storage;
      }
#if 0
            {
                char buf[500];
                type_to_str(buf, sizeof(buf), &type, get_tok_str(v, NULL));
                printf("type = '%s'\n", buf);
            }
#endif
      if ((type.t & VT_BTYPE) == VT_FUNC)
      {
        if ((type.t & VT_STATIC) && (l != VT_CONST))
          tcc_error("function without file scope cannot be static");
        /* if old style function prototype, we accept a
           declaration list */
        sym = type.ref;
        if (sym->f.func_type == FUNC_OLD && l == VT_CONST)
        {
          CType saved_func_vt = func_vt;
          func_vt = type;
          decl(VT_CMP);
          func_vt = saved_func_vt;
        }
        else if (sym->f.func_type == FUNC_OLD && l == VT_LOCAL && tok != '{')
        {
          CType saved_func_vt = func_vt;
          func_vt = type;
          decl(VT_CMP);
          func_vt = saved_func_vt;
        }

        if ((type.t & (VT_EXTERN | VT_INLINE)) == (VT_EXTERN | VT_INLINE))
        {
          /* always_inline functions must be handled as if they
             don't generate multiple global defs, even if extern
             inline, i.e. GNU inline semantics for those.  Rewrite
             them into static inline.  */
          if (tcc_state->gnu89_inline || sym->f.func_alwinl)
          {
            type.t = (type.t & ~VT_EXTERN) | VT_STATIC;
            type.ref->f.func_rewritten_extern_inline = 1;
          }
          else
            type.t &= ~VT_INLINE; /* always compile otherwise */
        }
      }
      else if (oldint)
      {
        tcc_warning_once(warn_implicit_int, "type defaults to int");
      }

      if (gnu_ext && (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3))
      {
        ad.asm_label = asm_label_instr();
        /* parse one last attribute list, after asm label */
        parse_attribute(&ad);
#if 0
                /* gcc does not allow __asm__("label") with function definition,
                   but why not ... */
                if (tok == '{')
                    expect(";");
#endif
      }

      /* malloc, nonnull, returns_nonnull: into the types (attribute.c) */
      if (ad.attr_malloc || ad.attr_nonnull || ad.attr_nonnull_all)
        attr_apply_pointer_facts(&type, &ad, own_functype);
#ifdef TCC_TARGET_PE
      if (ad.a.dllimport || ad.a.dllexport)
      {
        if (type.t & VT_STATIC)
          tcc_error("cannot have dll linkage with static");
        if (type.t & VT_TYPEDEF)
        {
          tcc_warning("'%s' attribute ignored for typedef",
                      ad.a.dllimport ? (ad.a.dllimport = 0, "dllimport") : (ad.a.dllexport = 0, "dllexport"));
        }
        else if (ad.a.dllimport)
        {
          if ((type.t & VT_BTYPE) == VT_FUNC)
            ad.a.dllimport = 0;
          else
            type.t |= VT_EXTERN;
        }
      }
#endif
      if (tok == '{')
      {
        if (dl)
          dl->active = 0; /* a definition, no list */
        if ((type.t & VT_BTYPE) != VT_FUNC)
          expect("function definition");

        /* reject abstract declarators in old-style function definition
           make old style params without decl have int type.
           New-style (FUNC_NEW/FUNC_ELLIPSIS) unnamed params are valid
           in GNU C and C23. */
        sym = type.ref;
        sym_first(sym); /* the loop steps from the type Sym to its parameters */
        while ((sym = sym->next) != NULL)
        {
          if (!(sym->v & ~SYM_FIELD) && type.ref->f.func_type == FUNC_OLD)
            expect("identifier");
          if (sym->type.t == VT_VOID)
            sym->type = int_type;
        }

        /* apply post-declaraton attributes */
        merge_funcattr(&type.ref->f, &ad.f);

        if (l == VT_LOCAL)
        {
          /* ── nested function definition ── */

          /* Grow nested funcs array if needed */
          if (tcc_state->nb_nested_funcs >= tcc_state->nested_funcs_capacity)
          {
            int old_capacity = tcc_state->nested_funcs_capacity;
            tcc_state->nested_funcs_capacity = old_capacity ? old_capacity * 2 : 4;
            tcc_state->nested_funcs =
                tcc_realloc(tcc_state->nested_funcs, tcc_state->nested_funcs_capacity * sizeof(NestedFunc *));
            memset(tcc_state->nested_funcs + old_capacity, 0,
                   (tcc_state->nested_funcs_capacity - old_capacity) * sizeof(NestedFunc *));
          }

          /* Get pointer to new nested func slot (allocated once, reused by
           * later parents; the pointer never moves) */
          if (!tcc_state->nested_funcs[tcc_state->nb_nested_funcs])
            tcc_state->nested_funcs[tcc_state->nb_nested_funcs] = tcc_mallocz(sizeof(NestedFunc));
          NestedFunc *nf = tcc_state->nested_funcs[tcc_state->nb_nested_funcs];
          nested_func_release(nf);
          memset(nf, 0, sizeof(*nf));

          /* Store filename for later */
          pstrncpy(nf->filename, file->filename, sizeof(nf->filename));

          /* The function's Sym lives on global_stack, bypassing external_sym
           * (each parent may define a nested function of the same name); its
           * name binds in the defining block (nested_define, below). */
          type.t &= ~VT_EXTERN;
          type.t |= VT_STATIC; /* nested functions are always local */
          nf->sym = sym_push2(&global_stack, v, type.t, 0);
          nf->sym->r = VT_CONST | VT_SYM;
          nf->sym->a = ad.a;
          nf->sym->type.ref = type.ref;
          if (local_stack)
            sym_copy_ref(nf->sym, &global_stack);
          /* Mark as nested function for static chain handling.
           * Note: This flag MUST be set on the symbol so that sym_find
           * will identify it as a nested function when looking up the
           * function name in the parent body. */
          nf->sym->a.nested_func = 1;
          /* Name mangling: use "parent.nested.N" to ensure global uniqueness.
           * Use a persistent counter so names don't collide across parent functions
           * (nb_nested_funcs resets per parent, but this counter does not). */
          nf->sym->asm_label = nested_func_label(v);

          /* Create placeholder address for the function */
          put_extern_sym(nf->sym, cur_text_section, 0, 0);

          /* Save the token stream (function body only, not parameters) */
          skip_or_save_block(&nf->func_str);
          /* -Wunused-*: codegen parses the body, a check-only parse never */
          if (tcc_state->unused_on)
            unused_scan_tokens(nf->func_str, nf->sym, 1);
          nested_define(nf);

          /* Pre-scan to identify captured parent variables.
           * If we're inside a nested function's gen_function, current_nested_func
           * is the parent. Pass it explicitly for multi-level nesting. */
          prescan_captured_vars(nf, local_stack, tcc_state->current_nested_func);

          /* Also scan VLA parameter expressions for captured variables.
           * VLA expressions (vla_array_str) are not part of the function body
           * token stream, so prescan_captured_vars won't find them. */
          prescan_vla_param_captured_vars(nf, local_stack);

          /* Register small nested functions as auto-inline candidates so
           * call sites in the parent can inline them via token replay.
           * Safe when captures are either all shadowed (no genuine captures)
           * or all genuine captures are read-only (never written or
           * address-taken).  Write captures produce VAR-to-VAR IR patterns
           * the optimizer can mishandle after inlining. */
          if (tcc_state->ir && nf->func_str &&
              !nf->sym->type.ref->f.func_noinline &&
              (TCC_OPT(tcc_state, opt_inline_functions) || TCC_OPT(tcc_state, opt_inline_small)) &&
              auto_inline_sig_ok(nf->sym) && nf->nb_nlgotos == 0 &&
              nf->nb_addr_labels == 0 &&
              (!nested_has_genuine_capture(nf) || nested_capture_is_read_only(nf)))
          {
            int body_len = nf->func_str->len;
            int threshold = TCC_OPT(tcc_state, opt_inline_limit) > 0 ? TCC_OPT(tcc_state, opt_inline_limit)
                                                            : (TCC_OPT(tcc_state, opt_inline_functions) ? 60 : 30);
            if (body_len <= threshold && !inline_body_has_apply_args(nf->func_str) &&
                !inline_body_has_static_local(nf->func_str) && !inline_body_has_unsafe_loops(nf->func_str))
            {
              nf->sym->type.ref->f.func_auto_inline = 1;
              struct InlineFunc *fn = tcc_mallocz(sizeof *fn);
              fn->filename = tcc_filename_intern(file->filename);
    fn->anchor = tcc_state->diag_anchor;
              fn->sym = nf->sym;
              /* Copy the token stream — nf->func_str is freed by end_macro()
               * during compile_nested_functions, so InlineFunc needs its own. */
              fn->func_str = tok_str_alloc();
              if (body_len > 0)
              {
                int *buf = tcc_malloc(body_len * sizeof(int));
                memcpy(buf, tok_str_buf(nf->func_str), body_len * sizeof(int));
                fn->func_str->data.str = buf;
                fn->func_str->allocated_len = body_len;
                fn->func_str->len = body_len;
              }
              tok_str_sys_copy(nf->func_str, fn->func_str);
              dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
              inline_record_def_bindings(fn, nf);

            }
          }

          /* Capture parent-scope typedefs, enumeration constants and
           * struct/union/enum tags so the nested function body can reference
           * them.  Walk the local_stack which is still live at this point
           * (before pop_local_syms); only a name's innermost binding is
           * visible here, so a shadowed one is not recorded. */
          nf->nb_parent_typedefs = 0;
          nf->nb_parent_enums = 0;
          nf->nb_parent_statics = 0;
          nf->nb_parent_struct_tags = 0;
          for (Sym *ts = local_stack; ts; ts = ts->prev)
          {
            int named = !(ts->v & SYM_FIELD) && (ts->v & ~SYM_STRUCT) < SYM_FIRST_ANOM;
            if (named && !(ts->v & SYM_STRUCT) && sym_find(ts->v) != ts)
              continue; /* hidden by an inner binding of the name */
            if (named && (ts->v & SYM_STRUCT) && sym_tag_head(ts->v & ~SYM_STRUCT) != ts)
              continue;
            if (IS_ENUM_VAL(ts->type.t) && !(ts->v & SYM_STRUCT) && (ts->r & VT_VALMASK) == VT_CONST)
            {
              if (nf->nb_parent_enums >= MAX_CAPTURED_VARS)
                continue;
              nf->parent_enum_tokens[nf->nb_parent_enums] = ts->v;
              nf->parent_enum_types[nf->nb_parent_enums] = ts->type;
              nf->parent_enum_vals[nf->nb_parent_enums] = sym_enum_val(ts);
              nf->nb_parent_enums++;
            }
            else if (named && !(ts->v & SYM_STRUCT) && (ts->r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) &&
                     !(ts->type.t & VT_TYPEDEF) && ts->c > 0)
            {
              /* a static local or block-scope extern variable */
              if (nf->nb_parent_statics < MAX_CAPTURED_VARS)
                nf->parent_static_syms[nf->nb_parent_statics++] = ts;
            }
            else if (ts->type.t & VT_TYPEDEF)
            {
              if (nf->nb_parent_typedefs >= MAX_CAPTURED_VARS)
                continue;
              /* Save typedef: token id + full type */
              nf->parent_typedef_tokens[nf->nb_parent_typedefs] = ts->v;
              nf->parent_typedef_types[nf->nb_parent_typedefs] = ts->type;
              nf->nb_parent_typedefs++;
            }
            else if ((ts->v & SYM_STRUCT) && ts->c != 0)
            {
              if (nf->nb_parent_struct_tags >= MAX_CAPTURED_VARS)
                continue;
              /* Save pointer to original struct tag sym (survives pop_local_syms
               * because completed struct tags have c != 0 so sym_pop won't free them) */
              nf->parent_struct_tag_syms[nf->nb_parent_struct_tags] = ts;
              nf->nb_parent_struct_tags++;
            }
          }
          /* ...and those an enclosing nested function's body sees from its
           * own parent (not on local_stack). */
          nested_record_inherited(nf);

          /* Pin struct field Syms reachable from captured typedefs/struct tags.
           * pop_local_syms frees Syms with c==0 (which includes struct fields
           * at offset 0).  Setting VT_SYM in r prevents sym_pop from freeing
           * them, keeping the struct type's field chain valid for the nested
           * function.  Also pin VLA-related Syms in the field type chain.
           * For VLA-only structs, the struct tag itself has c==0 (compile-time
           * size is 0) and must also be pinned. */
          for (int ti = 0; ti < nf->nb_parent_typedefs; ti++)
          {
            CType *ct = &nf->parent_typedef_types[ti];
            if ((ct->t & VT_BTYPE) == VT_STRUCT && ct->ref)
            {
              /* Pin the struct tag if its compile-time size is 0 */
              if (ct->ref->c == 0)
                ct->ref->r |= VT_SYM;
              for (Sym *f = sym_first(ct->ref); f; f = f->next)
              {
                if (f->c == 0)
                  f->r |= VT_SYM;
                /* Also pin the VLA ref Sym if present */
                if ((f->type.t & VT_VLA) && f->type.ref && f->type.ref->c == 0)
                  f->type.ref->r |= VT_SYM;
              }
            }
          }
          for (int si = 0; si < nf->nb_parent_struct_tags; si++)
          {
            Sym *ss = nf->parent_struct_tag_syms[si];
            if (ss)
            {
              for (Sym *f = sym_first(ss); f; f = f->next)
              {
                if (f->c == 0)
                  f->r |= VT_SYM;
                if ((f->type.t & VT_VLA) && f->type.ref && f->type.ref->c == 0)
                  f->type.ref->r |= VT_SYM;
              }
            }
          }

          /* Non-local goto: emit setjmp for each __label__ targeted by this nested function.
           * For each target label, we:
           *   1. Emit SETJMP(buf) where buf is the 12-byte jmp_buf allocated during __label__ processing
           *   2. If setjmp returns nonzero (longjmp occurred), emit a forward goto to the label
           * This allows the nested function to longjmp back to the parent's label. */
          if (tcc_state->ir && nf->nb_nlgotos > 0)
          {
            /* Force a frame pointer since longjmp restores FP */
            tcc_state->force_frame_pointer = 1;

            /* Force all local variables and parameters to be stack-resident.
             * NL_SETJMP saves callee-saved registers at this point (the nested
             * function definition site), but locals may be modified between the
             * setjmp and the nested function call.  NL_LONGJMP restores registers
             * to the setjmp-time values, which would overwrite those modifications.
             * By marking every local/parameter as address-taken we ensure their
             * live values reside on the stack, where NL_LONGJMP cannot corrupt them. */
            for (Sym *s = local_stack; s; s = s->prev)
            {
              if ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM))
              {
                s->a.addrtaken = 1;
                if (s->vreg >= 0)
                  tcc_ir_set_addrtaken(tcc_state->ir, s->vreg);
              }
            }

            for (int ngi = 0; ngi < nf->nb_nlgotos; ngi++)
            {
              int lbl_tok = nf->nlgoto_label_tokens[ngi];
              int buf_off = nf->nlgoto_buf_offsets[ngi];

              /* Create SValue for buffer address (FP-relative, no deref = address) */
              SValue buf_sv;
              svalue_init(&buf_sv);
              buf_sv.type.t = VT_INT;
              buf_sv.type.ref = NULL;
              buf_sv.r = VT_LOCAL;
              buf_sv.c.i = buf_off;
              buf_sv.vr = -1;

              /* Create dest SValue for setjmp return value */
              SValue dest_sv;
              svalue_init(&dest_sv);
              dest_sv.type.t = VT_INT;
              dest_sv.type.ref = NULL;
              dest_sv.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
              dest_sv.r = 0;
              dest_sv.c.i = 0;

              /* Emit NL_SETJMP: saves r4-r11, SP, resume_addr into 40-byte buf */
              tcc_ir_put(tcc_state->ir, TCCIR_OP_NL_SETJMP, &buf_sv, NULL, &dest_sv);

              /* Push setjmp result on vstack and test if nonzero */
              vpushi(0);
              vtop->vr = dest_sv.vr;
              vtop->r = 0;
              vtop->type.t = VT_INT;
              vtop->type.ref = NULL;
              vtop->c.i = 0;

              /* Compare with 0: result != 0 means longjmp return */
              vpushi(0);
              gen_op(TOK_NE);

              /* Conditional forward jump to the label */
              int jump_chain = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);

              /* Find or create the label symbol for forward reference */
              Sym *lbl_s = label_find(lbl_tok);
              if (!lbl_s)
                lbl_s = label_push(&global_label_stack, lbl_tok, LABEL_FORWARD);
              else if (lbl_s->r == LABEL_DECLARED)
                lbl_s->r = LABEL_FORWARD;

              /* Chain the conditional jump to the label's forward chain */
              if (lbl_s->jnext >= 0)
                tcc_ir_backpatch_first(tcc_state->ir, lbl_s->jnext, jump_chain);
              lbl_s->jnext = jump_chain;
            }
          }

          /* Increment count */
          tcc_state->nb_nested_funcs++;
          tcc_state->had_nested_funcs = 1;

          /* Continue parsing parent body - nested func saved */
          break;
        }
        else if (l != VT_CONST)
        {
          tcc_error("cannot use local functions");
        }

        /* put function symbol */
        type.t &= ~VT_EXTERN;
        if (tcc_warn_wanted(warn_strict_prototypes) || tcc_warn_wanted(warn_missing_prototypes))
          definition_prototype_checks(v, &type, decl_line, in_module);
        patch_own_functype = own_functype ? type.ref : NULL;
        sym = external_sym(v, &type, 0, &ad);
        patch_own_functype = NULL;
        if (tcc_state->warn_cross_tu & WARN_ON)
          tcc_typesig_note(v, decl_line, 1); /* where, for the link's report */
        yasc_pp_rekey(sym->type.ref); /* the prototype's type, when one came first */
        if (TCC_SU_WANTED(tcc_state))
          tcc_stack_usage_declarator(tcc_state, sym, v, decl_line);
        attr_apply_decl_facts(sym, &ad);
        if (in_module)
          yasc_module_sym(sym, this_exported);
        yasc_exporting = 0; /* types defined in the body are the body's */
        if (tcc_state->unused_on)
        {
          if (ad.attr_unused)
            sym->ymod |= SYM_UNUSED_OK;
          if ((sym->type.t & VT_STATIC) && !(sym->type.t & VT_INLINE))
            unused_declare_static(sym, decl_line, 1);
          unused_params_bind(sym);
        }
        else if (tcc_warn_wanted(warn_shadow))
          unused_params_bind(sym); /* -Wshadow gives a parameter's own line */
        /* a plain pointer parameter waiting for this body: compiled now (an
           error comes, nothing is written), so its report keeps its place
           at every -O (decl/plain_param.c) */
        if (yasc_pp_waiting(sym->type.ref) || !defer_function_body(sym, ad.section))
          define_global_function(sym, ad.section);
        break;
      }
      else
      {
        if (l == VT_CMP)
        {
          /* find parameter in function parameter list */
          for (sym = sym_first(func_vt.ref); sym; sym = sym->next)
            if ((sym->v & ~SYM_FIELD) == v)
              goto found;
          tcc_error("declaration for parameter '%s' but no such parameter", get_tok_str(v, NULL));
        found:
          if (type.t & VT_STORAGE) /* 'register' is okay */
            tcc_error("storage class specified for '%s'", get_tok_str(v, NULL));
          if (sym->type.t != VT_VOID)
            tcc_error("redefinition of parameter '%s'", get_tok_str(v, NULL));
          /* A float keeps its type: the double its caller passes is
             converted on entry (knr_params_receive). */
          sym->a.param_volatile = (type.t & VT_VOLATILE) != 0;
          sym->a.param_array = (type.t & VT_ARRAY) != 0;
          convert_parameter_type(&type);
          sym->type = type;
        }
        else if (type.t & VT_TYPEDEF)
        {
          /* save typedefed type  */
          /* XXX: test storage specifiers ? */
          sym = sym_find(v);
          if (sym && sym->sym_scope == local_scope)
          {
            if (!is_compatible_types(&sym->type, &type) || !(sym->type.t & VT_TYPEDEF))
            {
              /* Fallback: structural comparison for identical struct/union
                 typedefs that have different Sym* pointers.  This happens
                 when multiple auto-PCH replays define the same typedef. */
              if (!(sym->type.t & VT_TYPEDEF) || !compare_types_structural(&sym->type, &type))
                tcc_error("incompatible redefinition of '%s'", get_tok_str(v, NULL));
              /* Structurally identical; keep existing type to preserve
                 Sym* identity for earlier references. */
            }
            else
            {
              sym->type = type;
            }
          }
          else
          {
            sym = sym_push(v, &type, 0, 0);
            if (local_stack && tcc_state->unused_on)
              unused_declare_typedef(sym, decl_line, ad.attr_unused || ad.a.used);
          }
          sym->a = ad.a;
          attr_apply_decl_facts(sym, &ad);
          if ((type.t & VT_BTYPE) == VT_FUNC)
            merge_funcattr(&sym->type.ref->f, &ad.f);
          if (in_module)
            yasc_module_sym(sym, this_exported);
          if (debug_modes)
            tcc_debug_typedef(tcc_state, sym);
        }
        else if ((type.t & VT_BTYPE) == VT_VOID && !(type.t & VT_EXTERN))
        {
          tcc_error("declaration of void object");
        }
        else
        {
          r = 0;
          if ((type.t & VT_BTYPE) == VT_FUNC)
          {
            /* external function definition */
            /* specific case for func_call attribute */
            merge_funcattr(&type.ref->f, &ad.f);
          }
          else if (!(type.t & VT_ARRAY))
          {
            /* not lvalue if array */
            r |= VT_LVAL;
          }
          has_init = (tok == '=');
          /* yasc `T x = undefined;`: an automatic local with no value on
             purpose (decl_initializer_alloc: one the flow check cannot
             follow needs it, as it needs an initializer) */
          yasc_decl_undefined = 0;
          if (has_init && tcc_state->yasc && l != VT_CONST && !(type.t & (VT_STATIC | VT_EXTERN)) &&
              (type.t & VT_BTYPE) != VT_FUNC && !sym_find(TOK_undefined))
          {
            next();
            if (tok == TOK_undefined)
            {
              next();
              if (tok != ',' && tok != ';')
                expect("',' or ';' after 'undefined'");
              has_init = 0;
              yasc_decl_undefined = 1;
            }
            else
              unget_tok('=');
          }
          if (has_init && (type.t & VT_VLA))
            tcc_error("variable length array cannot be initialized");

          if (((type.t & VT_EXTERN) && (!has_init || l != VT_CONST)) ||
              (type.t & VT_BTYPE) == VT_FUNC
              /* as with GCC, uninitialized global arrays with no size
                are considered extern: */
              || ((type.t & VT_ARRAY) && !has_init && l == VT_CONST && type.ref->c < 0)
              /* likewise, accept tentative file-scope declarations of
                incomplete struct/union objects and let a later complete
                definition provide the storage. */
              || (!has_init && l == VT_CONST && !(type.t & VT_STATIC) && (type.t & VT_BTYPE) == VT_STRUCT &&
                  type_size(&type, &align) < 0))
          {
            /* external variable or function */
            int fresh = l == VT_CONST && (type.t & VT_BTYPE) == VT_FUNC && !ad.asm_label && !ad.alias_target &&
                        !sym_find(v);
            type.t |= VT_EXTERN;
            patch_own_functype = own_functype ? type.ref : NULL;
            sym = external_sym(v, &type, r, &ad);
            patch_own_functype = NULL;
            if (tcc_state->warn_cross_tu & WARN_ON)
              tcc_typesig_note(v, decl_line, 0);
            attr_apply_decl_facts(sym, &ad); /* before parking: a Sym with facts is never parked */
            if (fresh && own_functype && !local_stack && sym->type.ref == type.ref)
              type.ref->a.proto_own = 1;
            if (l == VT_LOCAL && (type.t & VT_BTYPE) == VT_FUNC)
              nested_note_block_proto(v); /* `auto`: a nested function defined later */
            if (ad.attr_unused && tcc_state->unused_on)
              sym->ymod |= SYM_UNUSED_OK;
            /* a first prototype: kept compactly until something names it */
            if (fresh)
              sym_park_proto(sym, stack_before);
          }
          else
          {
            if (l == VT_CONST || (type.t & VT_STATIC))
              r |= VT_CONST;
            else
              r |= VT_LOCAL;
            /* yasc: a borrowing struct kept beyond a call borrows static storage */
            if (tcc_state->yasc && (r & VT_VALMASK) == VT_CONST && yasc_borrowing_struct(&type) &&
                !(ad.sbind_set && ad.sbind_static && !ad.sbind_nb))
              tcc_error("'%s' outlives every call: write 'struct %s borrows(static)'", get_tok_str(v, NULL),
                        get_tok_str(type.ref->v & ~SYM_STRUCT, NULL));
            if (has_init)
              next();
            else if (l == VT_CONST)
              /* uninitialized global variables may be overridden */
              type.t |= VT_EXTERN;
            if (l == VT_CONST && (tcc_state->warn_cross_tu & WARN_ON))
              tcc_typesig_note(v, decl_line, 1);
            if (!(has_init && l == VT_CONST && defer_static_data(&type, &ad, r, v)))
              decl_initializer_alloc(&type, &ad, r, has_init, v, l == VT_CONST);
            yasc_decl_undefined = 0;
            if (ad.attr_deprecated | ad.attr_unused_result | ad.attr_fmt_kind | ad.attr_fmt_arg)
              attr_apply_decl_facts(sym_find(v), &ad);
            if (tcc_state->unused_on)
              unused_decl_var(l, &type, &ad, v, decl_line);
          }

          if (ad.alias_target && l == VT_CONST)
            apply_alias_attribute(sym_find(v), ad.alias_target);
          if (in_module)
            yasc_module_sym(sym_find(v), this_exported);
        }
        if (tok != ',')
        {
          if (l == VT_JMP)
            DECL_RETURN(1);
          skip(';');
          break;
        }
        next();
      }
    }
    if (dl)
      dl->active = 0;
  }
  DECL_RETURN(0);
}

#undef DECL_RETURN

/* ------------------------------------------------------------------------- */
#undef gjmp_addr
#undef gjmp
/* ------------------------------------------------------------------------- */
