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

/* string.c -- Constant string-builtin folding and inline abs expansion.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Try to extract a compile-time constant string from an SValue.
 * Returns the string pointer if the SValue refers to a constant string literal
 * in a data section, NULL otherwise. Sets *out_len to the string length. */
const char *try_get_constant_string(SValue *sv, int *out_len)
{
  ElfSym *esym;
  Section *sec;
  const char *str;
  const char *nul;
  addr_t offset;
  addr_t offset_in_sym;
  size_t remaining;

  /* Must be a constant symbol reference.  String literals and similar
   * symbol-backed references can still carry VT_LVAL before full decay. */
  if ((sv->r & (VT_VALMASK | VT_SYM | VT_LVAL)) != (VT_CONST | VT_SYM) &&
      (sv->r & (VT_VALMASK | VT_SYM | VT_LVAL)) != (VT_CONST | VT_SYM | VT_LVAL))
    return NULL;
  if (!sv->sym)
    return NULL;

  esym = elfsym(sv->sym);
  if (!esym)
    return NULL;

  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;

  if (esym->st_size == 0)
    return NULL;

  offset_in_sym = (addr_t)sv->c.i;
  if (offset_in_sym >= esym->st_size)
    return NULL;

  offset = esym->st_value + sv->c.i;
  if (offset >= sec->data_offset)
    return NULL;

  str = (const char *)(sec->data + offset);
  remaining = (size_t)(esym->st_size - offset_in_sym);
  nul = memchr(str, '\0', remaining);
  if (!nul)
    return NULL;
  if (out_len)
    *out_len = (int)(nul - str);
  return str;
}

static int is_zero_length_builtin_compare(SValue *sv)
{
  return ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) && sv->c.i == 0;
}

static int try_get_constant_size_t(SValue *sv, size_t *out)
{
  if (!sv || !out)
    return 0;

  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;

  *out = (size_t)sv->c.i;
  return 1;
}

static int try_get_constant_uchar(SValue *sv, unsigned char *out)
{
  if (!sv || !out)
    return 0;

  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;

  *out = (unsigned char)sv->c.i;
  return 1;
}

static int fold_builtin_strcmp_result(const char *s1, const char *s2)
{
  while ((unsigned char)*s1 == (unsigned char)*s2)
  {
    if (*s1 == '\0')
      return 0;
    ++s1;
    ++s2;
  }

  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static int fold_builtin_strncmp_result(const char *s1, const char *s2, size_t n)
{
  if (n == 0)
    return 0;

  while (n-- > 0)
  {
    unsigned char c1 = (unsigned char)*s1++;
    unsigned char c2 = (unsigned char)*s2++;
    if (c1 != c2 || c1 == '\0')
      return (int)c1 - (int)c2;
  }

  return 0;
}

static int fold_builtin_memcmp_result(const char *s1, const char *s2, size_t n)
{
  size_t i;

  for (i = 0; i < n; ++i)
  {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2)
      return (int)c1 - (int)c2;
  }

  return 0;
}

static int fold_builtin_memchr_offset(const char *s, unsigned char c, size_t n, int *out_offset)
{
  size_t i;

  if (!out_offset)
    return 0;

  for (i = 0; i < n; ++i)
  {
    if ((unsigned char)s[i] == c)
    {
      *out_offset = (int)i;
      return 1;
    }
  }

  *out_offset = -1;
  return 1;
}

int get_builtin_abs_info(const char *func_name, int *is_unsigned)
{
  *is_unsigned = 0;

  if (strcmp(func_name, "abs") == 0 || strcmp(func_name, "labs") == 0 || strcmp(func_name, "llabs") == 0 ||
      strcmp(func_name, "imaxabs") == 0)
    return 1;

  if (strcmp(func_name, "uabs") == 0 || strcmp(func_name, "ulabs") == 0 || strcmp(func_name, "ullabs") == 0 ||
      strcmp(func_name, "umaxabs") == 0)
  {
    *is_unsigned = 1;
    return 1;
  }

  return 0;
}

int builtin_abs_decl_matches(Sym *func_sym, const char *func_name)
{
  int expected_ret_t;
  int expected_param_t;
  CType ret_type;
  Sym *func_ref;
  Sym *param;

  if (!func_sym || !func_sym->type.ref || !func_name)
    return 1;

  if (strcmp(func_name, "abs") == 0)
  {
    expected_ret_t = VT_INT;
    expected_param_t = VT_INT;
  }
  else if (strcmp(func_name, "labs") == 0)
  {
    expected_ret_t = VT_INT | VT_LONG;
    expected_param_t = VT_INT | VT_LONG;
  }
  else if (strcmp(func_name, "llabs") == 0 || strcmp(func_name, "imaxabs") == 0)
  {
    expected_ret_t = VT_LLONG;
    expected_param_t = VT_LLONG;
  }
  else if (strcmp(func_name, "uabs") == 0)
  {
    expected_ret_t = VT_INT | VT_UNSIGNED;
    expected_param_t = VT_INT;
  }
  else if (strcmp(func_name, "ulabs") == 0)
  {
    expected_ret_t = VT_INT | VT_LONG | VT_UNSIGNED;
    expected_param_t = VT_INT | VT_LONG;
  }
  else if (strcmp(func_name, "ullabs") == 0 || strcmp(func_name, "umaxabs") == 0)
  {
    expected_ret_t = VT_LLONG | VT_UNSIGNED;
    expected_param_t = VT_LLONG;
  }
  else
    return 0;

  func_ref = func_sym->type.ref;
  ret_type = func_ref->type;
  if ((ret_type.t & (VT_BTYPE | VT_LONG | VT_UNSIGNED)) != expected_ret_t)
    return 0;

  if (func_ref->f.func_type == FUNC_OLD)
    return 1;

  param = func_ref->next;
  if (!param || param->next)
    return 0;

  return (param->type.t & (VT_BTYPE | VT_LONG | VT_UNSIGNED)) == expected_param_t;
}

/* Try to inline a builtin integer absolute value function.
 * Returns 1 if inlined, 0 otherwise.
 * On success, the result is pushed onto the value stack.
 * Uses the branchless formula: sign = x >> (N-1); result = (x ^ sign) - sign
 */
void gen_inline_abs_from_vtop(int shift_amount, int is_unsigned)
{
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    if (is_unsigned)
    {
      if (shift_amount == 63)
      {
        uint64_t ux = (uint64_t)(int64_t)vtop->c.i;
        vtop->c.i = ((int64_t)ux < 0) ? (uint64_t)(-(uint64_t)ux) : ux;
      }
      else
      {
        uint32_t ux = (uint32_t)vtop->c.i;
        vtop->c.i = ((int32_t)ux < 0) ? (uint32_t)(-(uint32_t)ux) : ux;
      }

      vtop->type.ref = NULL;
      vtop->type.t = (vtop->type.t & VT_BTYPE) | VT_UNSIGNED;
      return;
    }

    if (shift_amount == 63)
    {
      int64_t x = (int64_t)vtop->c.i;
      vtop->c.i = (x < 0) ? (uint64_t)(-x) : (uint64_t)x;
    }
    else
    {
      int32_t x = (int32_t)vtop->c.i;
      vtop->c.i = (x < 0) ? (int32_t)(-x) : x;
    }

    return;
  }

  if (shift_amount == 63)
  {
    /* The generic inline 64-bit bit-twiddling path is still unreliable for
     * runtime values on ARM.  Use a tiny runtime helper instead. */
    SValue param_num;
    SValue dest;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;

    vpush_helper_func(tok_alloc_const("__tcc_ullabsu"));
    vrott(2);

    svalue_init(&param_num);
    param_num.vr = -1;
    param_num.r = VT_CONST;
    param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);

    svalue_init(&dest);
    dest.type.t = VT_LLONG | (is_unsigned ? VT_UNSIGNED : 0);
    dest.type.ref = NULL;
    dest.r = 0;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    {
      SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 1);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-1], &call_id_sv, &dest);
    }

    vtop -= 2;
    vpushi(0);
    vtop->type.t = VT_LLONG | (is_unsigned ? VT_UNSIGNED : 0);
    vtop->type.ref = NULL;
    vtop->vr = dest.vr;
    vtop->r = TREG_R0;
    return;
  }

  if (is_unsigned)
  {
    SValue param_num;
    SValue dest;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    const char *helper_name;

    if (shift_amount == 63)
      helper_name = "__tcc_ullabsu";
    else if (vtop->type.t & VT_LONG)
      helper_name = "__tcc_ulabsu";
    else
      helper_name = "__tcc_uabsu";

    vpush_helper_func(tok_alloc_const(helper_name));
    vrott(2);

    svalue_init(&param_num);
    param_num.vr = -1;
    param_num.r = VT_CONST;
    param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);

    svalue_init(&dest);
    dest.type = vtop->type;
    dest.type.ref = NULL;
    dest.type.t |= VT_UNSIGNED;
    dest.r = 0;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    {
      SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 1);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-1], &call_id_sv, &dest);
    }

    vtop -= 2;
    vpushi(0);
    vtop->type = dest.type;
    vtop->vr = dest.vr;
    vtop->r = TREG_R0;
    return;
  }

  /* Generate: sign = x >> (N-1) */
  vdup();               /* Stack: ... x x */
  vpushi(shift_amount); /* Stack: ... x x shift */
  gen_op(TOK_SAR);      /* Stack: ... x sign */

  /* Generate: result = (x ^ sign) - sign */
  vdup();      /* Stack: ... x sign sign */
  vrott(3);    /* Stack: ... sign x sign */
  gen_op('^'); /* Stack: ... sign (x^sign) */
  vswap();     /* Stack: ... (x^sign) sign */
  gen_op('-'); /* Stack: ... result */
}

int try_inline_builtin_call(const char *func_name, SValue *args, int nb_args)
{
  int shift_amount, is_unsigned;
  int bt;

  if (nb_args != 1)
    return 0;

  if (!get_builtin_abs_info(func_name, &is_unsigned))
    return 0;

  bt = args[0].type.t & VT_BTYPE;
  shift_amount = (bt == VT_LLONG) ? 63 : 31;

  /* Push the argument value */
  vpushv(&args[0]); /* Stack: ... func_ptr x */
  gen_inline_abs_from_vtop(shift_amount, is_unsigned);

  return 1;
}
int __attribute__((noinline)) unary_funcall_opt_string_builtins(int func_tok, const char *func_name,
                                                                       SValue *saved_args, int nb_real_args,
                                                                       int call_id, int ir_idx_before_first_param,
                                                                       int ir_idx_before_args, const CType *ret_type)
{
  int optimized = 0;
  int folded_result = 0;
  int can_fold_result = 0;
  int lhs_len = 0;
  int rhs_len = 0;
  const char *lhs_str = NULL;
  const char *rhs_str = NULL;
  size_t n_const = 0;

  const int id = resolve_str_builtin_id(func_tok, func_name);
  if (id == STRBI_UNKNOWN)
    return 0;

  /* --- Constant folding: try to evaluate at compile time --- */

  if (nb_real_args == 1 && id == STRBI_STRLEN)
  {
    lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
    if (lhs_str)
    {
      folded_result = lhs_len;
      can_fold_result = 1;
    }
  }

  if (nb_real_args == 2 && id == STRBI_STRCMP)
  {
    lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
    rhs_str = try_get_constant_string(&saved_args[1], &rhs_len);
    if (lhs_str && rhs_str)
    {
      folded_result = fold_builtin_strcmp_result(lhs_str, rhs_str);
      can_fold_result = 1;
    }
  }

  if (!can_fold_result && nb_real_args == 3 && id == STRBI_STRNCMP && !is_zero_length_builtin_compare(&saved_args[2]))
  {
    lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
    rhs_str = try_get_constant_string(&saved_args[1], &rhs_len);
    if (lhs_str && rhs_str && try_get_constant_size_t(&saved_args[2], &n_const))
    {
      folded_result = fold_builtin_strncmp_result(lhs_str, rhs_str, n_const);
      can_fold_result = 1;
    }
  }

  if (!can_fold_result && nb_real_args == 3 && id == STRBI_MEMCMP)
  {
    lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
    rhs_str = try_get_constant_string(&saved_args[1], &rhs_len);
    if (lhs_str && rhs_str && try_get_constant_size_t(&saved_args[2], &n_const) && n_const <= (size_t)lhs_len + 1 &&
        n_const <= (size_t)rhs_len + 1)
    {
      folded_result = fold_builtin_memcmp_result(lhs_str, rhs_str, n_const);
      can_fold_result = 1;
    }
  }

  if (!can_fold_result && nb_real_args == 3 && id == STRBI_MEMCMP_EQ)
  {
    if (try_get_constant_size_t(&saved_args[2], &n_const))
    {
      if (n_const == 0 || (is_null_pointer(&saved_args[0]) && is_null_pointer(&saved_args[1])))
      {
        folded_result = 0;
        can_fold_result = 1;
      }
      else
      {
        lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
        rhs_str = try_get_constant_string(&saved_args[1], &rhs_len);
        if (lhs_str && rhs_str && n_const <= (size_t)lhs_len + 1 && n_const <= (size_t)rhs_len + 1)
        {
          folded_result = fold_builtin_memcmp_result(lhs_str, rhs_str, n_const) != 0;
          can_fold_result = 1;
        }
      }
    }
  }

  if (!can_fold_result && nb_real_args == 3 && id == STRBI_MEMCMP && try_get_constant_size_t(&saved_args[2], &n_const))
  {
    if (n_const == 0)
    {
      folded_result = 0;
      can_fold_result = 1;
    }
    else if (n_const == 1)
    {
      CType rt = {VT_INT, NULL};
      optimized = redirect_call_to_tcc_helper(saved_args, 2, "__tcc_memcmp1", &rt, call_id, ir_idx_before_first_param,
                                              ir_idx_before_args);
    }
  }

  if (!can_fold_result && nb_real_args == 3 && is_zero_length_builtin_compare(&saved_args[2]))
  {
    if (id == STRBI_STRNCMP || id == STRBI_MEMCMP)
    {
      folded_result = 0;
      can_fold_result = 1;
    }
  }

  if (!can_fold_result && nb_real_args == 3 && id == STRBI_MEMCHR)
  {
    unsigned char needle = 0;
    int match_offset = -1;
    lhs_str = try_get_constant_string(&saved_args[0], &lhs_len);
    if (lhs_str && try_get_constant_uchar(&saved_args[1], &needle) &&
        try_get_constant_size_t(&saved_args[2], &n_const) && n_const <= (size_t)lhs_len + 1 &&
        fold_builtin_memchr_offset(lhs_str, needle, n_const, &match_offset))
    {
      nop_or_rollback_call_params(call_id, ir_idx_before_first_param, ir_idx_before_args);

      if (match_offset >= 0)
      {
        SValue match_sv = saved_args[0];
        match_sv.c.i += match_offset;
        vpushv(&match_sv);
      }
      else
      {
        vpushi(0);
        vtop->type = saved_args[0].type;
      }

      vtop[-1] = vtop[0];
      --vtop;
      optimized = 1;
    }
  }

  /* --- Redirect to __tcc_* helpers (non-foldable cases) --- */

  if (!can_fold_result && !optimized && nb_real_args == 3 && (id == STRBI_MEMMOVE || id == STRBI_BCOPY))
  {
    nop_or_rollback_call_params(call_id, ir_idx_before_first_param, ir_idx_before_args);

    {
      SValue param_num;
      const int new_call_id = tcc_state->ir->next_call_id++;

      svalue_init(&param_num);
      param_num.vr = -1;
      param_num.r = VT_CONST;

      if (id == STRBI_BCOPY)
      {
        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 0);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[0], &param_num, NULL);

        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 1);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[1], &param_num, NULL);

        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 2);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[2], &param_num, NULL);

        vpush_typed_helper_func(tok_alloc_const("__tcc_bcopy"), &func_old_void_type);
        {
          SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, 3);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
        }
        --vtop;
        vpushi(0);
        vtop->type.t = VT_VOID;
        vtop->type.ref = NULL;
        vtop->r = VT_CONST;
        vtop->vr = -1;
        vtop->c.i = 0;
      }
      else
      {
        SValue dest;

        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 0);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[0], &param_num, NULL);

        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 1);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[1], &param_num, NULL);

        param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, 2);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &saved_args[2], &param_num, NULL);

        vpush_typed_helper_func(tok_alloc_const("__tcc_memmove"), &func_old_void_pointer_type);

        svalue_init(&dest);
        dest.type = saved_args[0].type;
        dest.r = 0;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        {
          SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, 3);
          tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);
        }

        --vtop;
        vpushi(0);
        vtop->type = dest.type;
        vtop->vr = dest.vr;
        vtop->r = TREG_R0;
      }

      vtop[-1] = vtop[0];
      --vtop;
      optimized = 1;
    }
  }

  /* Simple redirects: NOP old call, emit __tcc_* replacement */
  if (!can_fold_result && !optimized)
  {
    const char *helper = NULL;
    int nargs = 0;
    CType result_type = *ret_type;

    switch (id)
    {
    case STRBI_STRCMP:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strcmp";
        nargs = 2;
        result_type.t = VT_INT;
        result_type.ref = NULL;
      }
      break;
    case STRBI_STRCPY:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strcpy";
        nargs = 2;
        result_type = saved_args[0].type;
      }
      break;
    case STRBI_STPCPY:
      if (nb_real_args == 2)
      {
        helper = "__tcc_stpcpy";
        nargs = 2;
      }
      break;
    case STRBI_STRLEN:
      if (nb_real_args == 1)
      {
        helper = "__tcc_strlen";
        nargs = 1;
      }
      break;
    case STRBI_STRNLEN:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strnlen";
        nargs = 2;
      }
      break;
    case STRBI_STRPBRK:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strpbrk";
        nargs = 2;
      }
      break;
    case STRBI_STRRCHR:
    case STRBI_RINDEX:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strrchr";
        nargs = 2;
      }
      break;
    case STRBI_STRSTR:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strstr";
        nargs = 2;
      }
      break;
    case STRBI_STRCSPN:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strcspn";
        nargs = 2;
      }
      break;
    case STRBI_STRNCPY:
      if (nb_real_args == 3)
      {
        helper = "__tcc_strncpy";
        nargs = 3;
      }
      break;
    case STRBI_STRNCAT:
      if (nb_real_args == 3)
      {
        helper = "__tcc_strncat";
        nargs = 3;
      }
      break;
    case STRBI_STRNCMP:
      if (nb_real_args == 3)
      {
        helper = "__tcc_strncmp";
        nargs = 3;
        result_type.t = VT_INT;
        result_type.ref = NULL;
      }
      break;
    case STRBI_STRCHR:
    case STRBI_INDEX:
      if (nb_real_args == 2)
      {
        helper = "__tcc_strchr";
        nargs = 2;
        result_type = saved_args[0].type;
      }
      break;
    default:
      break;
    }

    if (helper)
      optimized = redirect_call_to_tcc_helper(saved_args, nargs, helper, &result_type, call_id,
                                              ir_idx_before_first_param, ir_idx_before_args);
  }

  /* --- Emit folded constant --- */

  if (can_fold_result)
  {
    nop_or_rollback_call_params(call_id, ir_idx_before_first_param, ir_idx_before_args);
    vpushi(folded_result);
    vtop[-1] = vtop[0];
    --vtop;
    optimized = 1;
  }
  return optimized;
}
