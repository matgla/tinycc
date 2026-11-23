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

/* chk.c -- _FORTIFY_SOURCE _chk builtins.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* "__tcc_strcpy" is strcpy to whoever reads a warning */
static const char *chk_base_name(const char *n)
{
  return strncmp(n, "__tcc_", 6) ? n : n + 6;
}

/* Extracted from unary() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_chk(void)
{
  switch (tok)
  {
  case TOK_builtin_object_size:
  case TOK_builtin_dynamic_object_size:
  {
    /* builtin/objsize.c: what the pointer points into, decided while the
       argument is parsed (without code) -- the same at every -O */
    const int dynamic = tok == TOK_builtin_dynamic_object_size;
    int obj_type_val, effects;
    ObjInfo oi;
    ObjRec saved;

    next();
    skip('(');
    nocode_wanted++;
    objsize_enter(&saved);
    effects = uv_effects;
    expr_eq();
    objsize_info(vtop, &oi);
    /* side effects in the argument (an assignment, ++, a call): unknown, as
       gcc says -- they are not evaluated */
    if (uv_effects != effects)
      memset(&oi, 0, sizeof oi);
    objsize_leave(&saved);
    vpop();
    nocode_wanted--;

    skip(',');

    /* Parse the type argument (0, 1, 2, or 3) — must be a constant */
    nocode_wanted++;
    expr_eq();
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
      obj_type_val = vtop->c.i & 3;
    else
      obj_type_val = 0;
    vpop();
    nocode_wanted--;

    skip(')');

    if (!dynamic || (obj_type_val & 1 ? oi.known1 : oi.known0) || !objsize_push_dynamic(&oi, obj_type_val))
      vpushs((addr_t)objsize_result(&oi, obj_type_val));
    break;
  }

  /* Memory allocation builtins - redirect to library functions */
  case TOK_builtin_abort:
  case TOK_builtin_malloc:
  case TOK_builtin_free:
  case TOK_builtin_calloc:
  case TOK_builtin_realloc:
  {
    const char *func_name;
    switch (tok)
    {
    case TOK_builtin_abort:
      func_name = "abort";
      break;
    case TOK_builtin_malloc:
      func_name = "malloc";
      break;
    case TOK_builtin_free:
      func_name = "free";
      break;
    case TOK_builtin_calloc:
      func_name = "calloc";
      break;
    case TOK_builtin_realloc:
      func_name = "realloc";
      break;
    default:
      func_name = NULL;
      break;
    }
    if (func_name)
    {
      int func_tok = tok_alloc_const(func_name);
      vpush_helper_func(func_tok);
    }
    next();
    break;
  }

  /* Bit manipulation builtins - map to library functions */
  case TOK_builtin_ffs:
  case TOK_builtin_ffsl:
  case TOK_builtin_ffsll:
  case TOK_builtin_clz:
  case TOK_builtin_clzl:
  case TOK_builtin_clzll:
  case TOK_builtin_ctz:
  case TOK_builtin_ctzl:
  case TOK_builtin_ctzll:
  case TOK_builtin_popcount:
  case TOK_builtin_popcountl:
  case TOK_builtin_popcountll:
  case TOK_builtin_parity:
  case TOK_builtin_parityl:
  case TOK_builtin_parityll:
  {
    const char *func_name;
    switch (tok)
    {
    case TOK_builtin_ffs:
      func_name = "ffs";
      break;
    case TOK_builtin_ffsl:
      func_name = "ffsl";
      break;
    case TOK_builtin_ffsll:
      func_name = "ffsll";
      break;
    case TOK_builtin_clz:
      func_name = "__clzsi2";
      break;
    case TOK_builtin_clzl:
      func_name = "__clzsi2";
      break;
    case TOK_builtin_clzll:
      func_name = "__clzdi2";
      break;
    case TOK_builtin_ctz:
      func_name = "__ctzsi2";
      break;
    case TOK_builtin_ctzl:
      func_name = "__ctzsi2";
      break;
    case TOK_builtin_ctzll:
      func_name = "__ctzdi2";
      break;
    case TOK_builtin_popcount:
      func_name = "__popcountsi2";
      break;
    case TOK_builtin_popcountl:
      func_name = "__popcountsi2";
      break;
    case TOK_builtin_popcountll:
      func_name = "__popcountdi2";
      break;
    case TOK_builtin_parity:
      func_name = "__paritysi2";
      break;
    case TOK_builtin_parityl:
      func_name = "__paritysi2";
      break;
    case TOK_builtin_parityll:
      func_name = "__paritydi2";
      break;
    default:
      func_name = NULL;
      break;
    }
    if (func_name && tcc_state->ir)
    {
      /* Convert the argument to the helper's parameter type ourselves: the
       * helpers are pushed unprototyped, so an int reaching a 64-bit helper
       * would leave r1 stale and a long would not be widened. */
      int wide = (tok == TOK_builtin_clzll || tok == TOK_builtin_ctzll || tok == TOK_builtin_popcountll ||
                  tok == TOK_builtin_parityll || tok == TOK_builtin_ffsll);
      int is_ffs = (tok == TOK_builtin_ffs || tok == TOK_builtin_ffsl || tok == TOK_builtin_ffsll);
      int func_tok = tok_alloc_const(func_name);
      CType arg_type;
      parse_builtin_params(0, "e");
      arg_type.ref = NULL;
      arg_type.t = (wide ? VT_LLONG : VT_INT) | (is_ffs ? 0 : VT_UNSIGNED);
      gen_cast(&arg_type);
      gen_builtin_libcall(func_tok, 1, VT_INT);
      break;
    }
    if (func_name)
    {
      int func_tok = tok_alloc_const(func_name);
      vpush_helper_func(func_tok);
    }
    next();
    break;
  }

  /* ================================================================
   * Fortified/chk builtins — table-driven handler.
   *
   * __builtin___memcpy_chk(dst, src, n, objsize) etc.
   *
   * Categories:
   *   SIMPLE  — n_prefix normal args, then 1 trailing objsize arg to drop
   *             e.g. memcpy_chk(d,s,n, SIZE) → memcpy(d,s,n) or __memcpy_chk(d,s,n,SIZE)
   *   FORMAT  — n_prefix normal args, then 2 args (flag, objsize) to drop,
   *             then format string + variadic args
   *             e.g. sprintf_chk(buf, FLAG, SIZE, fmt, ...) → sprintf(buf, fmt, ...)
   *
   * Decision logic after parsing:
   *   objsize == -1           → call base function (compiler can't check)
   *   objsize known, n const  → if n ≤ objsize: call base; else: call __*_chk
   *   objsize known, n runtime→ call __*_chk for runtime bounds check
   * ================================================================ */
  case TOK_builtin___memcpy_chk:
  case TOK_builtin___memmove_chk:
  case TOK_builtin___memset_chk:
  case TOK_builtin___mempcpy_chk:
  case TOK_builtin___strcpy_chk:
  case TOK_builtin___stpcpy_chk:
  case TOK_builtin___strcat_chk:
  case TOK_builtin___strncpy_chk:
  case TOK_builtin___stpncpy_chk:
  case TOK_builtin___strncat_chk:
  case TOK_builtin___sprintf_chk:
  case TOK_builtin___snprintf_chk:
  case TOK_builtin___vsprintf_chk:
  case TOK_builtin___vsnprintf_chk:
  {
    /* --- Descriptor table ---
     * base_func: function to call when objsize is -1 or statically safe
     * chk_func:  runtime checking function when objsize is known
     * n_prefix:  number of leading args kept in both base and chk calls
     * n_drop:    number of args after prefix to drop for base call (kept for chk)
     * has_varargs: 1 if format string + varargs follow the dropped args
     * returns_ptr: 1 if function returns a pointer (void*), 0 for int */
    struct chk_desc
    {
      int tok;
      const char *TCC_RODATA_REL base_func;
      const char *TCC_RODATA_REL chk_func;
      int n_prefix;
      int n_drop;
      int has_varargs;
      int returns_ptr;
    };
    static const struct chk_desc chk_table[] = {
        {TOK_builtin___memcpy_chk, "memcpy", "__memcpy_chk", 3, 1, 0, 1},
        {TOK_builtin___memmove_chk, "memmove", "__memmove_chk", 3, 1, 0, 1},
        {TOK_builtin___memset_chk, "memset", "__memset_chk", 3, 1, 0, 1},
        {TOK_builtin___mempcpy_chk, "mempcpy", "__mempcpy_chk", 3, 1, 0, 1},
        {TOK_builtin___strcpy_chk, "__tcc_strcpy", "__tcc_strcpy_chk", 2, 1, 0, 1},
        {TOK_builtin___stpcpy_chk, "__tcc_stpcpy", "__tcc_stpcpy_chk", 2, 1, 0, 1},
        {TOK_builtin___strcat_chk, "__tcc_strcat", "__tcc_strcat_chk", 2, 1, 0, 1},
        {TOK_builtin___strncpy_chk, "__tcc_strncpy", "__tcc_strncpy_chk", 3, 1, 0, 1},
        {TOK_builtin___stpncpy_chk, "__tcc_stpncpy", "__tcc_stpncpy_chk", 3, 1, 0, 1},
        {TOK_builtin___strncat_chk, "__tcc_strncat", "__tcc_strncat_chk", 3, 1, 0, 1},
        {TOK_builtin___sprintf_chk, "sprintf", "__sprintf_chk", 1, 2, 1, 0},
        {TOK_builtin___snprintf_chk, "snprintf", "__snprintf_chk", 2, 2, 1, 0},
        {TOK_builtin___vsprintf_chk, "vsprintf", "__vsprintf_chk", 1, 2, 1, 0},
        {TOK_builtin___vsnprintf_chk, "vsnprintf", "__vsnprintf_chk", 2, 2, 1, 0},
    };

    /* Look up descriptor */
    const struct chk_desc *desc = NULL;
    for (int ci = 0; ci < (int)(sizeof(chk_table) / sizeof(chk_table[0])); ci++)
    {
      if (chk_table[ci].tok == tok)
      {
        desc = &chk_table[ci];
        break;
      }
    }
    /* Shouldn't happen — the switch cases match the table exactly */
    if (!desc)
      tcc_error("internal: unhandled chk builtin");

    next(); /* consume __builtin___*_chk token */
    skip('(');

    /* Parse and save ALL arguments on the vstack.
     * Layout: prefix_args..., [varargs...] (dropped args stored separately) */
    int all_args_cap = 32;
    SValue *all_args = tcc_malloc(all_args_cap * sizeof(SValue));
    int total_args = 0;

    /* Parse prefix args */
    for (int i = 0; i < desc->n_prefix; i++)
    {
      if (i > 0)
        skip(',');
      expr_eq();
      convert_parameter_type(&vtop->type);
      if (!NOEVAL_WANTED)
        tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      if (total_args >= all_args_cap)
      {
        all_args_cap *= 2;
        all_args = tcc_realloc(all_args, all_args_cap * sizeof(SValue));
      }
      all_args[total_args] = *vtop;
      total_args++;
      vpop();
    }

    /* Parse dropped args (flag and/or objsize) */
    SValue dropped_args[2];
    for (int i = 0; i < desc->n_drop; i++)
    {
      skip(',');
      expr_eq();
      convert_parameter_type(&vtop->type);
      if (!NOEVAL_WANTED)
        tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      dropped_args[i] = *vtop;
      vpop();
    }

    /* The last dropped arg is always the objsize */
    SValue size_sv = dropped_args[desc->n_drop - 1];

    /* Parse remaining args (format string + varargs for format builtins, nothing for simple) */
    if (desc->has_varargs)
    {
      /* At least the format string follows */
      while (tok != ')')
      {
        skip(',');
        expr_eq();
        convert_parameter_type(&vtop->type);
        if (!NOEVAL_WANTED)
          tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
        if (total_args >= all_args_cap)
        {
          all_args_cap *= 2;
          all_args = tcc_realloc(all_args, all_args_cap * sizeof(SValue));
        }
        all_args[total_args] = *vtop;
        total_args++;
        vpop();
      }
    }

    skip(')');

    if (NOEVAL_WANTED)
    {
      /* In sizeof/typeof/nocode context, just push a dummy result */
      tcc_free(all_args);
      if (desc->returns_ptr)
      {
        vpushi(0);
        vtop->type = char_pointer_type;
      }
      else
      {
        vpushi(0);
      }
      break;
    }

    /* --- Decision logic ---
     * Determine whether to call the base function (stripped args) or
     * the runtime __*_chk function (all args including objsize). */
    int use_chk = 0; /* 0 = base func, 1 = __*_chk runtime func */
    int size_is_const = ((size_sv.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST);
    addr_t objsize = size_is_const ? (addr_t)size_sv.c.i : 0;

    if (size_is_const && objsize == (addr_t)-1)
    {
      /* objsize unknown — compiler can't check, call base function */
      use_chk = 0;
    }
    else if (size_is_const)
    {
      /* objsize known — check if we can resolve statically or need runtime check.
       * For simple builtins, the "n" (length) is the last prefix arg.
       * For str* builtins (strcpy, strcat, stpcpy), length is unknown. */
      if (!desc->has_varargs && desc->n_prefix >= 3)
      {
        /* Simple builtins with explicit length: n is last prefix arg */
        SValue *n_sv = &all_args[desc->n_prefix - 1];
        unsigned long long src_bytes;
        int n_is_const = ((n_sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST);

        if (desc->tok == TOK_builtin___strncat_chk &&
            ((svalue_get_conservative_string_bytes_u64(&all_args[1], &src_bytes) && src_bytes == 1) ||
             (n_is_const && (addr_t)n_sv->c.i == 0)))
        {
          use_chk = 0;
        }
        else if (desc->tok == TOK_builtin___strncat_chk)
        {
          use_chk = 1;
        }
        else if (n_is_const)
        {
          addr_t n_val = (addr_t)n_sv->c.i;
          if (n_val <= objsize)
            use_chk = 0; /* statically safe */
          else
          {
            use_chk = 1; /* will overflow — call __*_chk for runtime abort */
            objsize_chk_overflow(chk_base_name(desc->base_func), n_val, objsize);
          }
        }
        else
        {
          unsigned long long n_max;

          if (svalue_get_conservative_max_u64(n_sv, &n_max) && n_max <= (unsigned long long)objsize)
            use_chk = 0;
          else
            use_chk = 1; /* length unknown at compile time, need runtime check */
        }
      }
      else if (!desc->has_varargs && desc->n_prefix == 2)
      {
        unsigned long long src_bytes;

        switch (desc->tok)
        {
        case TOK_builtin___strcpy_chk:
        case TOK_builtin___stpcpy_chk:
          if (svalue_get_conservative_string_bytes_u64(&all_args[1], &src_bytes) &&
              src_bytes <= (unsigned long long)objsize)
            use_chk = 0;
          else
          {
            int sl;
            use_chk = 1;
            if (try_get_constant_string(&all_args[1], &sl))
              objsize_chk_overflow(chk_base_name(desc->base_func), (unsigned long long)sl + 1, objsize);
          }
          break;

        case TOK_builtin___strcat_chk:
          if (svalue_get_conservative_string_bytes_u64(&all_args[1], &src_bytes) && src_bytes == 1)
            use_chk = 0;
          else
            use_chk = 1;
          break;

        default:
          use_chk = 1;
          break;
        }
      }
      else
      {
        if (desc->tok == TOK_builtin___snprintf_chk || desc->tok == TOK_builtin___vsnprintf_chk)
        {
          SValue *len_sv = &all_args[1];
          unsigned long long len_max;
          int len_is_const = ((len_sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST);

          if (len_is_const)
          {
            addr_t len_val = (addr_t)len_sv->c.i;
            use_chk = len_val <= objsize ? 0 : 1;
          }
          else if (svalue_get_conservative_max_u64(len_sv, &len_max) && len_max <= (unsigned long long)objsize)
          {
            use_chk = 0;
          }
          else
          {
            use_chk = 1;
          }
        }
        else if (desc->tok == TOK_builtin___sprintf_chk || desc->tok == TOK_builtin___vsprintf_chk)
        {
          unsigned long long output_bytes;

          if (chk_get_conservative_sprintf_bytes(desc->tok, desc->n_prefix, all_args, total_args, &output_bytes) &&
              output_bytes <= (unsigned long long)objsize)
            use_chk = 0;
          else
            use_chk = 1;
        }
        else
        {
          use_chk = 1;
        }
      }
    }
    else
    {
      /* objsize is not a compile-time constant, but its run-time value is
       * still the destination size: pass it to the __*_chk function so the
       * check happens at run time, as gcc does.  Appending an empty string
       * writes nothing, so it needs no check. */
      unsigned long long src_bytes;

      use_chk = 1;
      if ((desc->tok == TOK_builtin___strcat_chk || desc->tok == TOK_builtin___strncat_chk) &&
          !desc->has_varargs && desc->n_prefix >= 2 &&
          svalue_get_conservative_string_bytes_u64(&all_args[1], &src_bytes) && src_bytes == 1)
        use_chk = 0;
    }

    /* --- Emit IR call --- */
    const char *call_func = use_chk ? desc->chk_func : desc->base_func;
    int call_id = tcc_state->ir->next_call_id++;
    SValue param_num;
    svalue_init(&param_num);
    param_num.vr = -1;
    param_num.r = VT_CONST;

    int out_param_idx = 0;

    if (use_chk)
    {
      /* Emit ALL original args in order: prefix, dropped (flag+objsize),
       * [varargs] */
      /* First: prefix args */
      for (int i = 0; i < desc->n_prefix && i < total_args; i++)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, out_param_idx);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &all_args[i], &param_num, NULL);
        out_param_idx++;
      }
      /* Then: dropped args (flag and objsize) */
      for (int i = 0; i < desc->n_drop; i++)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, out_param_idx);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &dropped_args[i], &param_num, NULL);
        out_param_idx++;
      }
      /* Then: remaining args (varargs) */
      for (int i = desc->n_prefix; i < total_args; i++)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, out_param_idx);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &all_args[i], &param_num, NULL);
        out_param_idx++;
      }
    }
    else
    {
      /* Emit only kept args: prefix + [varargs], dropping flag/objsize */
      for (int i = 0; i < desc->n_prefix && i < total_args; i++)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, out_param_idx);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &all_args[i], &param_num, NULL);
        out_param_idx++;
      }
      /* Remaining args (varargs) */
      for (int i = desc->n_prefix; i < total_args; i++)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, out_param_idx);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &all_args[i], &param_num, NULL);
        out_param_idx++;
      }
    }

    /* Push the target function and emit the call */
    vpush_helper_func(tok_alloc_const(call_func));

    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, out_param_idx);
    if (desc->returns_ptr)
    {
      SValue dest;
      svalue_init(&dest);
      dest.type.t = VT_PTR;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);
      --vtop; /* pop function symbol */
      vpushi(0);
      vtop->type = char_pointer_type;
      vtop->vr = dest.vr;
      vtop->r = TREG_R0;
    }
    else
    {
      SValue dest;
      svalue_init(&dest);
      dest.type.t = VT_INT;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);
      --vtop; /* pop function symbol */
      vpushi(0);
      vtop->type.t = VT_INT;
      vtop->vr = dest.vr;
      vtop->r = TREG_R0;
    }
    tcc_free(all_args);
    break;
  }
  }
}
