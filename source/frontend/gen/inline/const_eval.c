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

/* const_eval.c -- Compile-time evaluation of inlinable calls with constant arguments.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Suppress error output during speculative inline evaluation */
static void inline_eval_suppress_error(void *opaque, const char *msg)
{
  (void)opaque;
  (void)msg;
}

void inline_eval_cast_arg_to_param(SValue *sv, const CType *param_type)
{
  int pbt;
  int retag = 1;

  if (!sv || !param_type)
    return;

  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return;

  pbt = param_type->t & VT_BTYPE;
  switch (pbt)
  {
  case VT_BOOL:
    sv->c.i = sv->c.i != 0;
    break;
  case VT_BYTE:
    sv->c.i = (param_type->t & VT_UNSIGNED) ? (uint8_t)sv->c.i : (uint64_t)(int64_t)(int8_t)sv->c.i;
    break;
  case VT_SHORT:
    sv->c.i = (param_type->t & VT_UNSIGNED) ? (uint16_t)sv->c.i : (uint64_t)(int64_t)(int16_t)sv->c.i;
    break;
  case VT_INT:
    sv->c.i = (param_type->t & VT_UNSIGNED) ? (uint32_t)sv->c.i : (uint64_t)(int64_t)(int32_t)sv->c.i;
    break;
  case VT_LLONG:
  case VT_PTR:
    break;
  default:
    retag = 0;
    break;
  }
  if (retag)
    sv->type = *param_type;
}

/* Try to evaluate a small inline function at compile time with constant arguments.
 * Only handles trivial function bodies of the form: { return expr; }
 * This enables __builtin_constant_p to see through inlined calls, e.g.:
 *   inline int f(int x) { return __builtin_constant_p(x); }
 *   int g(void) { return f(1); } // should return 1 at -O1
 * Returns 1 on success (result pushed on vtop), 0 on failure.
 */
int try_inline_const_eval(Sym *func_sym, SValue *args, int nb_args)
{
  struct InlineFunc *fn;
  Sym *param, *func_type_ref;
  int i, param_count, saved_nocode_wanted, saved_tok, saved_local_scope;
  CValue saved_tokc;
  Sym *saved_local_stack;
  SValue *saved_vtop;
  SValue result;
  TokenString *ts;
  int success = 0;
  jmp_buf saved_jmp_buf;
  int saved_nb_errors;
  void (*saved_error_func)(void *opaque, const char *msg);
  void *saved_error_opaque;
  int saved_overlay_n;

  if (!tcc_state->optimize || !func_sym)
    return 0;
  /* Accept either an explicit `inline` function, a static auto-inline
   * candidate (selected by the body-size heuristic), or an eval-only
   * candidate (larger but pure — body saved solely for const-fold, not
   * regular inlining). */
  if (!(func_sym->type.t & VT_INLINE) &&
      !(func_sym->type.ref && (func_sym->type.ref->f.func_auto_inline || func_sym->type.ref->f.func_eval_only_inline)))
    return 0;
  if (TCC_LOG_INLINE_STRUCT)
    fprintf(stderr, "[inline-eval] TRY %s nb_args=%d\n", get_tok_str(func_sym->v & ~SYM_FIELD, NULL), nb_args);

  /* Work on a local copy of args[]: the caller's buffer (saved_args[]) is
   * reused by the non-CTE inlining fall-through path, so in-place mutation
   * here (e.g. pre-folding `*&g` to a constant) would corrupt that path. */
  SValue *local_args = tcc_malloc(nb_args * sizeof(SValue));
  for (i = 0; i < nb_args; i++)
    local_args[i] = args[i];
  args = local_args;

  /* All arguments must be compile-time constants. Symbol-valued pointer
   * constants (e.g. string literals) are permitted only for pointer-typed
   * params: if the body ever flows such a value into the return, the tag
   * survives via vtop->sym and the final VT_SYM check below rejects the
   * fold. Non-pointer args must be pure integer/float constants. */
  for (i = 0; i < nb_args; i++)
  {
    /* Pre-fold `*&g` args where g is a static scalar with an init and no
     * observed writes. Pointer globals are excluded: their section bytes
     * are typically zero with a pending relocation (e.g. static T *p = &x),
     * so reading raw bytes would yield a bogus null value. */
    if ((args[i].r & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM | VT_LVAL) && args[i].sym &&
        !args[i].sym->a.possibly_written && !(args[i].type.t & (VT_ARRAY | VT_VLA)))
    {
      int btype = args[i].type.t & VT_BTYPE;
      if (btype == VT_BYTE || btype == VT_SHORT || btype == VT_INT || btype == VT_LLONG || btype == VT_BOOL)
      {
        ElfSym *esym = elfsym(args[i].sym);
        if (esym && esym->st_shndx != SHN_UNDEF && esym->st_shndx != SHN_COMMON &&
            esym->st_shndx < tcc_state->nb_sections)
        {
          Section *sec = tcc_state->sections[esym->st_shndx];
          int align;
          int sz = type_size(&args[i].type, &align);
          unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)args[i].c.i);
          if (sec && sec->data && sz > 0 && off + (unsigned long)sz <= sec->data_offset)
          {
            const unsigned char *ptr = sec->data + off;
            int64_t val = 0;
            if (sz == 8)
              memcpy(&val, ptr, 8);
            else
            {
              memcpy(&val, ptr, sz);
              if (!(args[i].type.t & VT_UNSIGNED) && sz < 8)
              {
                int shift = (8 - sz) * 8;
                val = (int64_t)(val << shift) >> shift;
              }
            }
            args[i].c.i = val;
            args[i].r = VT_CONST;
            args[i].sym = NULL;
          }
        }
      }
    }
    if ((args[i].r & (VT_VALMASK | VT_LVAL)) != VT_CONST)
    {
      if (TCC_LOG_INLINE_STRUCT)
        fprintf(stderr, "[inline-eval] FAIL %s: arg[%d].r=0x%x not VT_CONST\n",
                get_tok_str(func_sym->v & ~SYM_FIELD, NULL), i, args[i].r);
      tcc_free(local_args);
      return 0;
    }
  }

  /* Find the InlineFunc for this symbol */
  fn = NULL;
  for (i = 0; i < tcc_state->nb_inline_fns; i++)
  {
    if (tcc_state->inline_fns[i]->sym == func_sym)
    {
      fn = tcc_state->inline_fns[i];
      break;
    }
  }
  if (!fn || !fn->func_str)
  {
    if (TCC_LOG_INLINE_STRUCT)
      fprintf(stderr, "[inline-eval] FAIL %s: no InlineFunc/func_str (fn=%p)\n",
              get_tok_str(func_sym->v & ~SYM_FIELD, NULL), (void *)fn);
    tcc_free(local_args);
    return 0;
  }

  /* Reject bodies with mutation ops, compound assignments, or function
   * calls — speculative evaluation with nocode_wanted silently drops such
   * side effects and would return a value inconsistent with real execution
   * (e.g. `w++` in the body must increment the global w at runtime). */
  if (inline_body_has_side_effects(fn->func_str))
  {
    if (TCC_LOG_INLINE_STRUCT)
      fprintf(stderr, "[inline-eval] FAIL %s: body has side effects\n", get_tok_str(func_sym->v & ~SYM_FIELD, NULL));
    tcc_free(local_args);
    return 0;
  }

  /* Get function parameter list */
  func_type_ref = func_sym->type.ref;
  if (!func_type_ref)
  {
    tcc_free(local_args);
    return 0;
  }

  /* Count and verify parameters */
  param_count = 0;
  for (param = func_type_ref->next; param; param = param->next)
    param_count++;
  if (param_count != nb_args)
  {
    tcc_free(local_args);
    return 0;
  }

  /* Verify all params have valid identifier names and are not vector/struct
   * /complex/floating types — we only fold scalar integer/pointer values.
   * FP is rejected because speculative evaluation under nocode_wanted does
   * not perform real FP arithmetic (int-to-double casts and FP division
   * lower to runtime calls that are suppressed), so results would diverge
   * silently from real execution.
   * A VT_SYM-tagged arg is only safe when bound to a pointer-typed param;
   * otherwise stripping VT_SYM would turn a symbol reference into a bogus
   * integer. */
  {
    int pi = 0;
    for (param = func_type_ref->next; param; param = param->next, pi++)
    {
      int pbt;
      if ((param->v & ~SYM_FIELD) < TOK_IDENT)
      {
        tcc_free(local_args);
        return 0;
      }
      pbt = param->type.t & VT_BTYPE;
      if (pbt == VT_STRUCT || (param->type.t & (VT_VECTOR | VT_COMPLEX)))
      {
        tcc_free(local_args);
        return 0;
      }
      if (is_float(param->type.t))
      {
        tcc_free(local_args);
        return 0;
      }
      if ((args[pi].r & VT_SYM) && pbt != VT_PTR)
      {
        tcc_free(local_args);
        return 0;
      }
    }
  }

  /* Reject non-scalar / floating return types: structs, complex, and vectors
   * all need real codegen (memcpy-style returns or composite construction)
   * that speculative const evaluation cannot produce; FP returns cannot be
   * trusted because runtime FP calls are suppressed under nocode_wanted. */
  {
    CType *rt = &func_type_ref->type;
    int rbt = rt->t & VT_BTYPE;
    if (rbt == VT_STRUCT || (rt->t & (VT_VECTOR | VT_COMPLEX)))
    {
      tcc_free(local_args);
      return 0;
    }
    if (is_float(rt->t))
    {
      tcc_free(local_args);
      return 0;
    }
  }

  /* Reject bodies that mention float/double anywhere — including local
   * variable declarations and explicit casts. Even with integer params
   * and return, an internal `(double) x / y` would fold to an int divide
   * because FP helper calls are suppressed under nocode_wanted. */
  {
    const int *tp2 = tok_str_buf(fn->func_str);
    while (*tp2)
    {
      int tv2;
      CValue tcv2;
      tok_get(&tv2, &tp2, &tcv2);
      if (tv2 == TOK_EOF || tv2 == 0)
        break;
      if (tv2 == TOK_FLOAT || tv2 == TOK_DOUBLE)
      {
        if (TCC_LOG_INLINE_STRUCT)
          fprintf(stderr, "[inline-eval] FAIL %s: body contains FP type\n",
                  get_tok_str(func_sym->v & ~SYM_FIELD, NULL));
        tcc_free(local_args);
        return 0;
      }
    }
  }

  /* Save state */
  saved_nocode_wanted = nocode_wanted;
  saved_local_stack = local_stack;
  saved_local_scope = local_scope;
  saved_tok = tok;
  saved_tokc = tokc;
  saved_vtop = vtop;
  saved_overlay_n = tcc_state->inline_eval_overlay_n;

  /* Populate the param-to-arg overlay. Identifier resolution (unary's
   * default branch) substitutes these SValues when a token matches, giving
   * the body direct access to VT_SYM pointer args so `*p` can later fold to
   * the underlying global's initializer. Overlay is a fixed-size cache
   * (up to 8 entries); remaining params are resolved via sym_push below. */
  {
    int oi = 0;
    Sym *p2 = func_type_ref->next;
    for (; oi < nb_args && p2 && oi < 8; oi++, p2 = p2->next)
    {
      tcc_state->inline_eval_overlay_tok[oi] = p2->v & ~SYM_FIELD;
      tcc_state->inline_eval_overlay_sv[oi] = args[oi];
      inline_eval_cast_arg_to_param(&tcc_state->inline_eval_overlay_sv[oi], &p2->type);
    }
    tcc_state->inline_eval_overlay_n = oi;
  }

  /* Evaluate in a nested local scope so inline parameters/body locals do not
   * conflict with caller locals that may share the same identifier names. */
  ++local_scope;

  /* Push parameter symbols as compile-time constants. For 64-bit args we
   * cannot fit the value in Sym::c (int). Piggy-back on the enum-constant
   * mechanism: VT_ENUM_VAL on the param type makes identifier lookup pull
   * the full 64-bit value from Sym::enum_val (see tccgen.c identifier
   * resolution path for IS_ENUM_VAL). */
  param = func_type_ref->next;
  for (i = 0; i < nb_args; i++, param = param->next)
  {
    SValue param_arg = args[i];
    int btype = param->type.t & VT_BTYPE;
    Sym *s;
    inline_eval_cast_arg_to_param(&param_arg, &param->type);
    if (btype == VT_LLONG)
    {
      CType et = param->type;
      et.t |= VT_ENUM_VAL;
      s = sym_push(param->v & ~SYM_FIELD, &et, VT_CONST, 0);
      s->enum_val = param_arg.c.i;
    }
    else if (param_arg.r & VT_SYM)
    {
      /* Pointer-typed VT_SYM arg: push the param with VT_SYM set so any
       * identifier lookup produces a symbol-tagged SValue. If the body
       * flows this param into the return, the top-level VT_SYM check
       * rejects the fold. Safe because we never emit code under
       * nocode_wanted. */
      s = sym_push(param->v & ~SYM_FIELD, &param->type, VT_CONST | VT_SYM, 0);
    }
    else
    {
      s = sym_push(param->v & ~SYM_FIELD, &param->type, VT_CONST, (int)param_arg.c.i);
    }
    s->vreg = -1;
  }

  /* Suppress code generation during evaluation */
  nocode_wanted++;

  /* Create a non-owning wrapper TokenString to replay the inline body.
   * Use alloc=2 so end_macro() nulls data.str without freeing the original. */
  ts = tok_str_alloc();
  ts->data.str = tok_str_buf(fn->func_str);
  ts->allocated_len = 1; /* pretend heap so tok_str_buf returns data.str */
  ts->len = fn->func_str->len;
  begin_macro(ts, 2);

  /* Set up error recovery: expressions like x++ on a constant parameter
   * will trigger tcc_error("lvalue expected"). Catch and treat as failure. */
  saved_nb_errors = tcc_state->nb_errors;
  saved_error_func = tcc_state->error_func;
  saved_error_opaque = tcc_state->error_opaque;
  memcpy(saved_jmp_buf, tcc_state->error_jmp_buf, sizeof(jmp_buf));
  tcc_state->error_func = inline_eval_suppress_error;
  tcc_state->error_opaque = NULL;

  if (setjmp(tcc_state->error_jmp_buf) != 0)
  {
    /* Error occurred during speculative evaluation — not a constant */
    success = 0;
    goto cleanup;
  }

  next();

  /* Expect: { [local-decl;]* return expr ; }
   * Local declarations must have compile-time-constant initializers; we
   * treat them like additional parameters so subsequent uses fold. */
  if (tok == '{')
  {
    next();

    /* Parse a sequence of local declarations and compile-time-dead
     * if-statements of the form
     *   T name = const-expr [, name = const-expr]* ;
     *   if (const-false-cond) stmt          // skipped entirely
     * Anything else breaks out to the return check. */
    while (tok != TOK_RETURN)
    {
      CType btype;
      AttributeDef ad;

      /* Handle `if (cond) then-stmt [else else-stmt]`.
       *   - cond must evaluate to a compile-time constant.
       *   - cond == 0: skip then-stmt (tokens only; no parsing of side-
       *     effecting statements under nocode_wanted). If there's an
       *     `else`, bail for now — handling it would require parsing the
       *     else-stmt as the taken path.
       *   - cond != 0: bail — parsing the then-stmt under nocode_wanted
       *     would silently drop any side effects it contains. */
      if (tok == TOK_IF)
      {
        int cond_val;
        next();
        if (tok != '(')
        {
          success = 0;
          goto cleanup;
        }
        next();
        expr_eq();
        if (tok != ')' || (vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST || (vtop->r & VT_SYM))
        {
          if (vtop >= saved_vtop + 1)
            vtop--;
          success = 0;
          goto cleanup;
        }
        cond_val = (vtop->c.i != 0);
        vtop--;
        next(); /* past ')' */

        if (cond_val)
        {
          success = 0;
          goto cleanup;
        }

        /* Skip the then-stmt without parsing, using brace/bracket/paren
         * depth so structure inside the block is respected. */
        if (tok == '{')
        {
          int bdepth = 0;
          do
          {
            if (tok == '{')
              bdepth++;
            else if (tok == '}')
              bdepth--;
            next();
          } while (bdepth > 0 && tok != TOK_EOF);
        }
        else
        {
          int pdepth = 0;
          while (!(pdepth == 0 && tok == ';') && tok != TOK_EOF)
          {
            if (tok == '(' || tok == '[' || tok == '{')
              pdepth++;
            else if (tok == ')' || tok == ']' || tok == '}')
            {
              if (pdepth > 0)
                pdepth--;
            }
            next();
          }
          if (tok == ';')
            next();
        }

        if (tok == TOK_ELSE)
        {
          success = 0;
          goto cleanup;
        }
        continue;
      }

      if (!parse_btype(&btype, &ad, 0))
        break; /* not a declaration — let the return check handle it */

      /* Storage classes inside a speculative body are not supported. */
      if (btype.t & (VT_EXTERN | VT_STATIC | VT_TYPEDEF))
      {
        success = 0;
        goto cleanup;
      }

      while (1)
      {
        CType type = btype;
        int name_tok = 0;
        type_decl(&type, &ad, &name_tok, TYPE_DIRECT);

        /* Must be a plain scalar with a name and a const initializer. */
        if (name_tok == 0 || (type.t & VT_BTYPE) == VT_FUNC || (type.t & VT_ARRAY))
        {
          success = 0;
          goto cleanup;
        }
        if (tok != '=')
        {
          success = 0;
          goto cleanup;
        }
        next();
        expr_eq();
        if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST || (vtop->r & VT_SYM))
        {
          vtop--;
          success = 0;
          goto cleanup;
        }

        {
          int ltype = type.t & VT_BTYPE;
          Sym *s;
          if (ltype == VT_LLONG)
          {
            CType et = type;
            et.t |= VT_ENUM_VAL;
            s = sym_push(name_tok & ~SYM_FIELD, &et, VT_CONST, 0);
            s->enum_val = vtop->c.i;
          }
          else
          {
            s = sym_push(name_tok & ~SYM_FIELD, &type, VT_CONST, (int)vtop->c.i);
          }
          s->vreg = -1;
        }
        vtop--;

        if (tok != ',')
          break;
        next();
      }

      if (tok != ';')
      {
        success = 0;
        goto cleanup;
      }
      next();
    }

    if (tok == TOK_RETURN)
    {
      next();
      expr_eq();
      /* Apply the implicit conversion to the function's declared return
       * type — gfunc_return does the same in a real epilogue, and without
       * it narrowing types (e.g. u8 mode(QI)) leak promoted int values out
       * of the inlined body. */
      if (func_sym->type.ref)
      {
        CType ret_type = func_sym->type.ref->type;
        if ((ret_type.t & VT_BTYPE) != VT_VOID && (ret_type.t & VT_BTYPE) != VT_STRUCT && !(ret_type.t & VT_COMPLEX))
          gen_cast(&ret_type);
      }
      /* Check if the result is a compile-time constant */
      if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM))
      {
        result = *vtop;
        /* Strip VT_ENUM_VAL inherited from 64-bit param lookups so the
         * caller doesn't see the return value tagged as an enum. */
        if ((result.type.t & VT_STRUCT_MASK) == VT_ENUM_VAL)
          result.type.t &= ~VT_STRUCT_MASK;
        success = 1;
      }
      else if (TCC_LOG_INLINE_STRUCT)
      {
        fprintf(stderr, "[inline-eval] FAIL %s: return not VT_CONST vtop->r=0x%x\n",
                get_tok_str(func_sym->v & ~SYM_FIELD, NULL), vtop->r);
      }
      vtop--; /* pop the result (or failed non-const) */
    }
  }

cleanup:
  /* Restore error handling */
  memcpy(tcc_state->error_jmp_buf, saved_jmp_buf, sizeof(jmp_buf));
  tcc_state->error_func = saved_error_func;
  tcc_state->error_opaque = saved_error_opaque;
  tcc_state->nb_errors = saved_nb_errors;

  /* Restore inline-eval overlay count (supports nested inline-eval). */
  tcc_state->inline_eval_overlay_n = saved_overlay_n;

  /* Clean up: end macro replay.
   * Use end_macro_to() instead of end_macro() because speculative parsing
   * (e.g. string literals via decl_initializer_alloc) may push extra macro
   * stack entries (unget_tok) that aren't popped before we reach cleanup. */
  end_macro_to(ts);

  /* Restore state */
  nocode_wanted = saved_nocode_wanted;
  tok = saved_tok;
  tokc = saved_tokc;

  /* Pop parameter symbols */
  sym_pop(&local_stack, saved_local_stack, 0);
  local_scope = saved_local_scope;

  /* Restore vtop to what it was before (in case partial parsing left junk) */
  vtop = saved_vtop;

  tcc_free(local_args);
  if (success)
  {
    vpushv(&result);
    if (TCC_LOG_INLINE_STRUCT)
      fprintf(stderr, "[inline-eval] OK %s\n", get_tok_str(func_sym->v & ~SYM_FIELD, NULL));
    return 1;
  }
  if (TCC_LOG_INLINE_STRUCT)
    fprintf(stderr, "[inline-eval] FAIL %s: cleanup (success=0)\n", get_tok_str(func_sym->v & ~SYM_FIELD, NULL));
  return 0;
}

int inline_arg_is_constant_like(const SValue *sv)
{
  return (sv->r & (VT_VALMASK | VT_LVAL)) == VT_CONST;
}
