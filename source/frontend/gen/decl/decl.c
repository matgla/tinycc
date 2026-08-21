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

/* 'l' is VT_LOCAL or VT_CONST to define default storage type
   or VT_CMP if parsing old style parameter list
   or VT_JMP if parsing c99 for decl: for (int i = 0, ...) */
int decl(int l)
{
  int v, has_init, r, oldint, align;
  CType type, btype;
  Sym *sym;
  AttributeDef ad, adbase;

  while (1)
  {

    oldint = 0;
    if (!parse_btype(&btype, &adbase, l == VT_LOCAL))
    {
      if (l == VT_JMP)
        return 0;
      /* skip redundant ';' if not in old parameter decl scope */
      if (tok == ';' && l != VT_CMP)
      {
        next();
        continue;
      }
      if (tok == TOK_STATIC_ASSERT)
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

    while (1)
    { /* iterate thru each declaration */
      type = btype;
      ad = adbase;
      type_decl(&type, &ad, &v, TYPE_DIRECT);
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
        tcc_warning("type defaults to int");
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
        if ((type.t & VT_BTYPE) != VT_FUNC)
          expect("function definition");

        /* reject abstract declarators in old-style function definition
           make old style params without decl have int type.
           New-style (FUNC_NEW/FUNC_ELLIPSIS) unnamed params are valid
           in GNU C and C23. */
        sym = type.ref;
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
            tcc_state->nested_funcs_capacity =
                tcc_state->nested_funcs_capacity ? tcc_state->nested_funcs_capacity * 2 : 4;
            tcc_state->nested_funcs =
                tcc_realloc(tcc_state->nested_funcs, tcc_state->nested_funcs_capacity * sizeof(NestedFunc));
          }

          /* Get pointer to new nested func slot */
          NestedFunc *nf = &tcc_state->nested_funcs[tcc_state->nb_nested_funcs];
          memset(nf, 0, sizeof(*nf));

          /* Store filename for later */
          pstrncpy(nf->filename, file->filename, sizeof(nf->filename));

          /* Push symbol into global scope, bypassing external_sym to avoid
           * redefinition errors when multiple parent functions each define
           * a nested function with the same name (e.g. "nested" in foo and bar). */
          type.t &= ~VT_EXTERN;
          type.t |= VT_STATIC; /* nested functions are always local */
          nf->sym = global_identifier_push(v, type.t, 0);
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
          {
            static int nested_func_uid = 0;
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s.%d", get_tok_str(v, NULL), nested_func_uid++);
            nf->sym->asm_label = tok_alloc(mangled, strlen(mangled))->tok;
          }

          /* Create placeholder address for the function */
          put_extern_sym(nf->sym, cur_text_section, 0, 0);

          /* Save the token stream (function body only, not parameters) */
          skip_or_save_block(&nf->func_str);

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
              (tcc_state->opt_inline_functions || tcc_state->opt_inline_small) &&
              auto_inline_sig_ok(nf->sym) && nf->nb_nlgotos == 0 &&
              nf->nb_addr_labels == 0 &&
              (!nested_has_genuine_capture(nf) || nested_capture_is_read_only(nf)))
          {
            int body_len = nf->func_str->len;
            int threshold = tcc_state->opt_inline_limit > 0 ? tcc_state->opt_inline_limit
                                                            : (tcc_state->opt_inline_functions ? 60 : 30);
            if (body_len <= threshold && !inline_body_has_apply_args(nf->func_str) &&
                !inline_body_has_static_local(nf->func_str) && !inline_body_has_unsafe_loops(nf->func_str))
            {
              nf->sym->type.ref->f.func_auto_inline = 1;
              struct InlineFunc *fn = tcc_mallocz(sizeof *fn + strlen(file->filename));
              strcpy(fn->filename, file->filename);
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
              dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);

            }
          }

          /* Capture parent-scope typedefs and struct/union/enum tags so the
           * nested function body can reference them.  Walk the local_stack
           * which is still live at this point (before pop_local_syms). */
          nf->nb_parent_typedefs = 0;
          nf->nb_parent_struct_tags = 0;
          for (Sym *ts = local_stack; ts; ts = ts->prev)
          {
            if (ts->type.t & VT_TYPEDEF)
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
              for (Sym *f = ct->ref->next; f; f = f->next)
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
              for (Sym *f = ss->next; f; f = f->next)
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
        sym = external_sym(v, &type, 0, &ad);

        /* static inline functions are just recorded as a kind
           of macro. Their code will be emitted at the end of
           the compilation unit only if they are used */
        if (sym->type.t & VT_INLINE)
        {
          struct InlineFunc *fn;
          fn = tcc_mallocz(sizeof *fn + strlen(file->filename));
          strcpy(fn->filename, file->filename);
          fn->sym = sym;
          dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
          skip_or_save_block(&fn->func_str);

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
              (tcc_state->opt_inline_functions || tcc_state->opt_inline_small) &&
              tcc_state->nb_vla_param_exprs == 0 && fn->func_str)
          {
            int sig = auto_inline_sig_ok(sym);
            int thr = tcc_state->opt_inline_limit > 0 ? tcc_state->opt_inline_limit
                                                      : (tcc_state->opt_inline_functions ? 60 : 30);
            /* Void-returning with 64-bit params: same narrow cap as the auto
             * path (longer bodies trip an IR coalescing bug on narrowed locals). */
            if (sig == 2)
              thr = 15;
            if (sig && fn->func_str->len <= thr && !inline_body_has_apply_args(fn->func_str) &&
                !inline_body_has_unsafe_loops(fn->func_str) && !inline_body_has_static_local(fn->func_str))
            {
              sym->type.ref->f.func_auto_inline = 1;
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
                 auto_inline_sig_ok(sym) && (tcc_state->opt_inline_functions || tcc_state->opt_inline_small) &&
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
          fn = tcc_mallocz(sizeof *fn + strlen(file->filename));
          strcpy(fn->filename, file->filename);
          fn->sym = sym;
          fn->func_str = NULL;
          skip_or_save_block(&fn->func_str);

          int threshold = tcc_state->opt_inline_limit > 0 ? tcc_state->opt_inline_limit
                                                          : (tcc_state->opt_inline_functions ? 60 : 30);
          int is_static = !!(sym->type.t & VT_STATIC);
          int body_len = fn->func_str ? fn->func_str->len : 0;

          if (TCC_LOG_INLINE_STRUCT)
            fprintf(stderr, "[auto-inline] candidate: %s  static=%d  len=%d  threshold=%d\n",
                    get_tok_str(sym->v & ~SYM_FIELD, NULL), is_static, body_len, threshold);
          LOG_INLINE_STRUCT("[auto-inline] candidate: %s  static=%d  len=%d  threshold=%d  ret_btype=%d",
                            get_tok_str(sym->v & ~SYM_FIELD, NULL), is_static, body_len, threshold,
                            sym->type.ref ? (sym->type.ref->type.t & VT_BTYPE) : -1);

          Section *saved_text = cur_text_section;
          cur_text_section = ad.section ? ad.section : function_text_section(tcc_state, sym);
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

            TokenString *compile_ts = tok_str_alloc();
            if (body_len > 0)
            {
              int *buf = tcc_malloc(body_len * sizeof(int));
              memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
              compile_ts->data.str = buf;
              compile_ts->allocated_len = body_len;
              compile_ts->len = body_len;
            }
            int saved_outer_tok = tok;
            CValue saved_outer_tokc = tokc;
            if (TCC_LOG_INLINE_STRUCT)
              fprintf(stderr, "[auto-inline] SMALL: compiling standalone for %s\n",
                      get_tok_str(sym->v & ~SYM_FIELD, NULL));
            tcc_state->had_nested_funcs = 0;
            begin_macro(compile_ts, 1); /* owning: compile_ts freed on end_macro */
            next();
            gen_function(sym);
            end_macro();
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
                  tok_str_free(fn->func_str);
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

              TokenString *compile_ts = tok_str_alloc();
              int *buf = tcc_malloc(body_len * sizeof(int));
              memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
              compile_ts->data.str = buf;
              compile_ts->allocated_len = body_len;
              compile_ts->len = body_len;

              int saved_outer_tok = tok;
              CValue saved_outer_tokc = tokc;
              begin_macro(compile_ts, 1);
              next();
              gen_function(sym);
              end_macro();
              tok = saved_outer_tok;
              tokc = saved_outer_tokc;
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
                TokenString *compile_ts = tok_str_alloc();
                int *buf = tcc_malloc(body_len * sizeof(int));
                memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
                compile_ts->data.str = buf;
                compile_ts->allocated_len = body_len;
                compile_ts->len = body_len;
                begin_macro(compile_ts, 1);
                next();
                gen_function(sym);
                end_macro();
                if (sym->type.ref->f.func_auto_inline) {
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
                  tok_str_free(fn->func_str);
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
                if (tcc_state->opt_dead_store) {
                  TokenString *compile_ts = tok_str_alloc();
                  int *buf = tcc_malloc(body_len * sizeof(int));
                  memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
                  compile_ts->data.str = buf;
                  compile_ts->allocated_len = body_len;
                  compile_ts->len = body_len;
                  dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
                  begin_macro(compile_ts, 1);
                  next();
                  gen_function(sym);
                  end_macro();
                  if (!sym->type.ref->f.tu_static_writer &&
                      !sym->type.ref->f.func_late_reopt &&
                      !sym->type.ref->f.func_keep_tokens_for_noreturn) {
                    /* Not a static writer — discard tokens to save memory. */
                    tok_str_free(fn->func_str);
                    fn->func_str = NULL;
                    fn->sym = NULL;
                  }
                } else {
                  TokenString *ts = fn->func_str;
                  fn->func_str = NULL;
                  begin_macro(ts, 1);
                  next();
                  gen_function(sym);
                  end_macro();
                  tcc_free(fn);
                }
              }
              tok = saved_outer_tok;
              tokc = saved_outer_tokc;
            }
            else
            {
              tcc_free(fn);
            }
          }

          cur_text_section = saved_text;
        }
        else
        {
          /* compute text section */
          cur_text_section = ad.section;
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
          if (tcc_state->opt_dead_store)
          {
            struct InlineFunc *fn = tcc_mallocz(sizeof *fn + strlen(file->filename));
            strcpy(fn->filename, file->filename);
            fn->sym = sym;
            fn->func_str = NULL;
            skip_or_save_block(&fn->func_str);
            int body_len = fn->func_str ? fn->func_str->len : 0;
            if (fn->func_str && body_len > 0 && body_len <= late_reopt_cap)
            {
              dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
              TokenString *compile_ts = tok_str_alloc();
              int *buf = tcc_malloc(body_len * sizeof(int));
              memcpy(buf, tok_str_buf(fn->func_str), body_len * sizeof(int));
              compile_ts->data.str = buf;
              compile_ts->allocated_len = body_len;
              compile_ts->len = body_len;
              int saved_outer_tok = tok;
              CValue saved_outer_tokc = tokc;
              tcc_state->had_nested_funcs = 0;
              begin_macro(compile_ts, 1);
              next();
              gen_function(sym);
              end_macro();
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
                tok_str_free(fn->func_str);
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
                begin_macro(ts, 1);
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
        break;
      }
      else
      {
        if (l == VT_CMP)
        {
          /* find parameter in function parameter list */
          for (sym = func_vt.ref->next; sym; sym = sym->next)
            if ((sym->v & ~SYM_FIELD) == v)
              goto found;
          tcc_error("declaration for parameter '%s' but no such parameter", get_tok_str(v, NULL));
        found:
          if (type.t & VT_STORAGE) /* 'register' is okay */
            tcc_error("storage class specified for '%s'", get_tok_str(v, NULL));
          if (sym->type.t != VT_VOID)
            tcc_error("redefinition of parameter '%s'", get_tok_str(v, NULL));
          convert_parameter_type(&type);
          /* K&R default argument promotion: float parameters are received
             as double because callers apply default argument promotions
             (C89 6.5.4.2).  Without a prototype, float is promoted to
             double at the call site, so the function must receive the
             parameter as double. */
          if ((type.t & VT_BTYPE) == VT_FLOAT)
            type.t = (type.t & ~VT_BTYPE) | VT_DOUBLE;
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
          }
          sym->a = ad.a;
          if ((type.t & VT_BTYPE) == VT_FUNC)
            merge_funcattr(&sym->type.ref->f, &ad.f);
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
            type.t |= VT_EXTERN;
            external_sym(v, &type, r, &ad);
          }
          else
          {
            if (l == VT_CONST || (type.t & VT_STATIC))
              r |= VT_CONST;
            else
              r |= VT_LOCAL;
            if (has_init)
              next();
            else if (l == VT_CONST)
              /* uninitialized global variables may be overridden */
              type.t |= VT_EXTERN;
            decl_initializer_alloc(&type, &ad, r, has_init, v, l == VT_CONST);
          }

          if (ad.alias_target && l == VT_CONST)
            apply_alias_attribute(sym_find(v), ad.alias_target);
        }
        if (tok != ',')
        {
          if (l == VT_JMP)
            return 1;
          skip(';');
          break;
        }
        next();
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
#undef gjmp_addr
#undef gjmp
/* ------------------------------------------------------------------------- */
