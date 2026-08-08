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

/* fp2.c -- Floating-point builtins, second dispatch group.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Extracted from unary_builtin_fp() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_fp2(void)
{
  switch (tok)
  {
  /* __builtin_fabs / __builtin_fabsf / __builtin_fabsl */
  case TOK_builtin_fabs:
  case TOK_builtin_fabsf:
  case TOK_builtin_fabsl:
  {
    int tok1 = tok;
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    /* Check if argument is a compile-time constant */
    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        u.i &= 0x7FFFFFFFU;
        vtop->c.f = u.f;
      }
      else
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = (bt == VT_LDOUBLE) ? (double)vtop->c.ld : vtop->c.d;
        u.i &= 0x7FFFFFFFFFFFFFFFULL;
        vtop->c.d = u.d;
        if (bt == VT_LDOUBLE)
          vtop->c.ld = (long double)u.d;
      }
    }
    else
    {
      /* Runtime: generate call to fabs/fabsf */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT) || (tok1 == TOK_builtin_fabsf);

      if (tok1 == TOK_builtin_fabsf && arg_bt != VT_FLOAT)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        gen_cast(&ft);
      }
      else if (tok1 != TOK_builtin_fabsf && arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
        is_float = 0;
      }

      gen_builtin_libcall(is_float ? TOK___fabsf : TOK___fabs, 1, is_float ? VT_FLOAT : VT_DOUBLE);
    }
    break;
  }

  /* __builtin_copysignl — long double variant (on ARM, same as double) */
  case TOK_builtin_copysignl:
  {
    parse_builtin_params(0, "ee");

    if (is_const_for_folding(&vtop[-1]) && is_const_for_folding(&vtop[0]))
    {
      double mag = get_const_double(&vtop[-1]);
      double sgn = get_const_double(&vtop[0]);
      double res = copysign(mag, sgn);
      vtop--;
      vtop->c.ld = res;
      vtop->type.t = VT_LDOUBLE;
      vtop->r = VT_CONST;
      break;
    }

    /* On ARM, long double == double, so just call copysign */

    /* Ensure both args are doubles */
    if ((vtop[-1].type.t & VT_BTYPE) == VT_FLOAT)
    {
      SValue tmp = vtop[0];
      vtop[0] = vtop[-1];
      CType dt;
      dt.t = VT_DOUBLE;
      dt.ref = NULL;
      gen_cast(&dt);
      vtop[-1] = vtop[0];
      vtop[0] = tmp;
    }
    if ((vtop[0].type.t & VT_BTYPE) == VT_FLOAT)
    {
      CType dt;
      dt.t = VT_DOUBLE;
      dt.ref = NULL;
      gen_cast(&dt);
    }

    gen_builtin_libcall(TOK___copysign, 2, VT_LDOUBLE);
    break;
  }

  /* __builtin_isfinite / __builtin_isfinitef — true if not NaN and not Inf */
  case TOK_builtin_isfinite:
  case TOK_builtin_isfinitef:
  {
    int tok1 = tok;
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      int result;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        uint32_t exp = (u.i >> 23) & 0xFF;
        result = (exp != 0xFF);
      }
      else
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = (bt == VT_LDOUBLE) ? (double)vtop->c.ld : vtop->c.d;
        uint64_t exp = (u.i >> 52) & 0x7FF;
        result = (exp != 0x7FF);
      }
      vtop--;
      vpushi(result);
    }
    else
    {
      /* Runtime: finite(x) or finitef(x) — returns non-zero if finite */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT) || (tok1 == TOK_builtin_isfinitef);

      if (!is_float && arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
        is_float = 0;
      }
      else if (is_float && arg_bt != VT_FLOAT)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        gen_cast(&ft);
      }

      gen_builtin_libcall(is_float ? TOK___finitef : TOK___finite, 1, VT_INT);
    }
    break;
  }

  /* __builtin_isinf_sign — returns +1 for +Inf, -1 for -Inf, 0 otherwise */
  case TOK_builtin_isinf_sign:
  {
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      int result = 0;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        if ((u.i & 0x7FFFFFFF) == 0x7F800000)
          result = (u.i & 0x80000000) ? -1 : 1;
      }
      else
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = (bt == VT_LDOUBLE) ? (double)vtop->c.ld : vtop->c.d;
        if ((u.i & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL)
          result = (u.i & 0x8000000000000000ULL) ? -1 : 1;
      }
      vtop--;
      vpushi(result);
    }
    else
    {
      /* Runtime: call isinf then check sign.
       * isinf returns non-zero if infinite. We need +1/-1/0.
       * Implement as: isinf(x) ? (signbit(x) ? -1 : 1) : 0
       * For simplicity, call isinf and multiply by sign. Actually,
       * just call isinf() which on newlib returns +1/-1/0 already. */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT);

      if (arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
        is_float = 0;
      }

      gen_builtin_libcall(is_float ? TOK___isinff : TOK___isinf, 1, VT_INT);
    }
    break;
  }

  /* __builtin_fmax / __builtin_fmaxf / __builtin_fmaxl / __builtin_fmin / __builtin_fminf / __builtin_fminl */
  case TOK_builtin_fmax:
  case TOK_builtin_fmaxf:
  case TOK_builtin_fmaxl:
  case TOK_builtin_fmin:
  case TOK_builtin_fminf:
  case TOK_builtin_fminl:
  {
    int tok1 = tok;
    parse_builtin_params(0, "ee");

    /* See inlined parameters that were bound to constant args through. */
    inline_subst_const_arg(&vtop[-1]);
    inline_subst_const_arg(&vtop[0]);

    int is_float = (tok1 == TOK_builtin_fmaxf || tok1 == TOK_builtin_fminf);
    int is_max = (tok1 == TOK_builtin_fmax || tok1 == TOK_builtin_fmaxf || tok1 == TOK_builtin_fmaxl);

    /* Check if both arguments are constants */
    int bt_x = vtop[-1].type.t & VT_BTYPE;
    int bt_y = vtop[0].type.t & VT_BTYPE;
    int x_is_const = (vtop[-1].r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop[-1].r & VT_SYM);
    int y_is_const = (vtop[0].r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop[0].r & VT_SYM);
    int x_is_fp = (bt_x == VT_FLOAT || bt_x == VT_DOUBLE || bt_x == VT_LDOUBLE);
    int y_is_fp = (bt_y == VT_FLOAT || bt_y == VT_DOUBLE || bt_y == VT_LDOUBLE);
    if (x_is_const && y_is_const && x_is_fp && y_is_fp)
    {
      double x = (bt_x == VT_FLOAT) ? (double)vtop[-1].c.f : vtop[-1].c.d;
      double y = (bt_y == VT_FLOAT) ? (double)vtop[0].c.f : vtop[0].c.d;
      double result;
      /* fmax: if either is NaN, return the other. If both NaN, return NaN */
      if (x != x)
        result = y;
      else if (y != y)
        result = x;
      else
        result = is_max ? (x > y ? x : y) : (x < y ? x : y);

      vtop -= 2;
      if (is_float)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        vpush(&ft);
        vtop->r = VT_CONST;
        vtop->c.f = (float)result;
      }
      else
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        vpush(&dt);
        vtop->r = VT_CONST;
        vtop->c.d = result;
      }
    }
    else
    {
      /* Runtime: call fmax/fmaxf/fmin/fminf */
      /* Ensure type consistency */
      if (is_float)
      {
        if ((vtop[-1].type.t & VT_BTYPE) != VT_FLOAT)
        {
          SValue tmp = vtop[0];
          vtop[0] = vtop[-1];
          CType ft;
          ft.t = VT_FLOAT;
          ft.ref = NULL;
          gen_cast(&ft);
          vtop[-1] = vtop[0];
          vtop[0] = tmp;
        }
        if ((vtop[0].type.t & VT_BTYPE) != VT_FLOAT)
        {
          CType ft;
          ft.t = VT_FLOAT;
          ft.ref = NULL;
          gen_cast(&ft);
        }
      }
      else
      {
        if ((vtop[-1].type.t & VT_BTYPE) == VT_FLOAT)
        {
          SValue tmp = vtop[0];
          vtop[0] = vtop[-1];
          CType dt;
          dt.t = VT_DOUBLE;
          dt.ref = NULL;
          gen_cast(&dt);
          vtop[-1] = vtop[0];
          vtop[0] = tmp;
        }
        if ((vtop[0].type.t & VT_BTYPE) == VT_FLOAT)
        {
          CType dt;
          dt.t = VT_DOUBLE;
          dt.ref = NULL;
          gen_cast(&dt);
        }
      }

      int func_tok;
      if (is_max)
        func_tok = is_float ? TOK___fmaxf : TOK___fmax;
      else
        func_tok = is_float ? TOK___fminf : TOK___fmin;
      /* For long double variants, use the 'l' runtime functions.
       * On ARM (long double == double), these are equivalent to double versions. */
      if (tok1 == TOK_builtin_fmaxl)
        func_tok = TOK___fmaxl;
      else if (tok1 == TOK_builtin_fminl)
        func_tok = TOK___fminl;

      gen_builtin_libcall(func_tok, 2, is_float ? VT_FLOAT : VT_DOUBLE);
    }
    break;
  }

  /* __builtin_isnormal — true if value is a normal (not zero, subnormal, inf, or NaN) */
  case TOK_builtin_isnormal:
  {
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      int result;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        uint32_t exp = (u.i >> 23) & 0xFF;
        result = (exp != 0 && exp != 0xFF);
      }
      else
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = (bt == VT_LDOUBLE) ? (double)vtop->c.ld : vtop->c.d;
        uint64_t exp = (u.i >> 52) & 0x7FF;
        result = (exp != 0 && exp != 0x7FF);
      }
      vtop--;
      vpushi(result);
    }
    else
    {
      /* Runtime: isfinite(x) && x != 0.0 && !issubnormal(x)
       * Simplify: call finite(x), then check exponent is non-zero.
       * For soft-float, we can use: finite(x) && (bits & exp_mask) != 0
       * Easiest approach: call finite(x), then compare x != 0 and check
       * But that's complex. Just use: !isnan(x) && !isinf(x) && x != 0 && exp != 0
       * For simplicity, call finite(x) as first check, and generate comparison != 0 */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT);

      if (!is_float && arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
      }

      /* Save the value for subnormal check */
      SValue val_save = *vtop;

      /* Call finite(x) */
      gen_builtin_libcall(is_float ? TOK___finitef : TOK___finite, 1, VT_INT);

      /* Now we need: finite_result && x != 0.0 (approximately, ignoring subnormals for now)
       * Actually, isnormal is: exponent != 0 && exponent != all-1s.
       * finite checks exponent != all-1s. We still need exponent != 0.
       * Compare x with 0: won't work for subnormals (they compare != 0).
       * For a proper implementation we'd need bit manipulation, which is complex in this IR.
       * For now: finite(x) && fabs(x) >= FLT_MIN (or DBL_MIN) */

      /* Simpler approach: call fabs, compare with minimum normal */
      SValue finite_result = *vtop--;

      vpushv(&val_save);

      /* Call fabs on the saved value */
      gen_builtin_libcall(is_float ? TOK___fabsf : TOK___fabs, 1, is_float ? VT_FLOAT : VT_DOUBLE);

      /* Compare fabs(x) >= min_normal */
      if (is_float)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        vpush(&ft);
        vtop->r = VT_CONST;
        vtop->c.f = 1.17549435e-38f; /* FLT_MIN */
      }
      else
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        vpush(&dt);
        vtop->r = VT_CONST;
        vtop->c.d = 2.2250738585072014e-308; /* DBL_MIN */
      }
      gen_op(TOK_GE); /* fabs(x) >= min_normal */

      /* AND with finite result */
      vpushv(&finite_result);
      vswap();
      gen_op('&');
    }
    break;
  }

  /* __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x) */
  case TOK_builtin_fpclassify:
  {
    next();
    skip('(');
    /* Parse 5 integer constants and 1 floating-point expression */
    int fp_nan_val = expr_const();
    skip(',');
    int fp_inf_val = expr_const();
    skip(',');
    int fp_normal_val = expr_const();
    skip(',');
    int fp_subnormal_val = expr_const();
    skip(',');
    int fp_zero_val = expr_const();
    skip(',');
    expr_eq(); /* the floating-point value */
    skip(')');

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      int result;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        uint32_t exp = (u.i >> 23) & 0xFF;
        uint32_t man = u.i & 0x7FFFFF;
        if (exp == 0xFF && man != 0)
          result = fp_nan_val;
        else if (exp == 0xFF && man == 0)
          result = fp_inf_val;
        else if (exp == 0 && man == 0)
          result = fp_zero_val;
        else if (exp == 0)
          result = fp_subnormal_val;
        else
          result = fp_normal_val;
      }
      else
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = (bt == VT_LDOUBLE) ? (double)vtop->c.ld : vtop->c.d;
        uint64_t exp = (u.i >> 52) & 0x7FF;
        uint64_t man = u.i & 0xFFFFFFFFFFFFFULL;
        if (exp == 0x7FF && man != 0)
          result = fp_nan_val;
        else if (exp == 0x7FF && man == 0)
          result = fp_inf_val;
        else if (exp == 0 && man == 0)
          result = fp_zero_val;
        else if (exp == 0)
          result = fp_subnormal_val;
        else
          result = fp_normal_val;
      }
      vtop--;
      vpushi(result);
    }
    else
    {
      /* Runtime: use a series of calls: isnan, isinf, finite, then classify.
       * This is complex at runtime. For now, just call __fpclassifyf/__fpclassifyd
       * which returns FP_NAN=0, FP_INFINITE=1, FP_NORMAL=4, FP_SUBNORMAL=3, FP_ZERO=2
       * and then map via a lookup. But there's no standard __fpclassify on newlib.
       *
       * Alternative: emit isnan(x) ? nan_val : isinf(x) ? inf_val : x == 0 ? zero_val : isnormal(x) ? normal_val :
       * subnormal_val This is very complex for the vstack. For now, just emit 0 as a fallback. */
      tcc_warning("__builtin_fpclassify with non-constant argument not fully supported");
      vtop--;
      vpushi(0);
    }
    break;
  }

  case TOK_builtin_bswap16:
  case TOK_builtin_bswap32:
  case TOK_builtin_bswap64:
  {
    int tok1 = tok;
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    /* Get the swap size based on builtin type */
    int size = 8; /* default to 64-bit for bswap64 */
    if (tok1 == TOK_builtin_bswap16)
      size = 2;
    else if (tok1 == TOK_builtin_bswap32)
      size = 4;

    /* Check if argument is a compile-time constant */
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM))
    {
      uint64_t val;
      int bt = vtop->type.t & VT_BTYPE;

      /* Extract the constant value based on type */
      if (bt == VT_LLONG)
      {
        val = vtop->c.i;
      }
      else if (bt == VT_INT)
      {
        val = (uint32_t)vtop->c.i;
      }
      else if (bt == VT_SHORT)
      {
        val = (uint16_t)vtop->c.i;
      }
      else
      {
        val = (uint64_t)vtop->c.i;
      }

      /* Perform byte swap */
      uint64_t result = 0;
      if (size == 2)
      {
        result = ((val & 0x00FF) << 8) | ((val & 0xFF00) >> 8);
        result = (uint16_t)result;
      }
      else if (size == 4)
      {
        result = ((val & 0x000000FF) << 24) | ((val & 0x0000FF00) << 8) | ((val & 0x00FF0000) >> 8) |
                 ((val & 0xFF000000) >> 24);
        result = (uint32_t)result;
      }
      else
      {
        result = ((val & 0x00000000000000FFULL) << 56) | ((val & 0x000000000000FF00ULL) << 40) |
                 ((val & 0x0000000000FF0000ULL) << 24) | ((val & 0x00000000FF000000ULL) << 8) |
                 ((val & 0x000000FF00000000ULL) >> 8) | ((val & 0x0000FF0000000000ULL) >> 24) |
                 ((val & 0x00FF000000000000ULL) >> 40) | ((val & 0xFF00000000000000ULL) >> 56);
      }

      vtop--;

      /* Push result with appropriate type */
      CType result_type;
      result_type.t = (size == 2)   ? (VT_SHORT | VT_UNSIGNED)
                      : (size == 4) ? (VT_INT | VT_UNSIGNED)
                                    : (VT_LLONG | VT_UNSIGNED);
      result_type.ref = NULL;
      vpush(&result_type);
      vtop->r = VT_CONST;
      vtop->c.i = result;
    }
    else
    {
      /* For runtime values, generate inline byte swap using shifts and ORs */
      CType result_type;
      if (size == 2)
      {
        result_type.t = VT_SHORT | VT_UNSIGNED;
      }
      else if (size == 4)
      {
        result_type.t = VT_INT | VT_UNSIGNED;
      }
      else
      {
        result_type.t = VT_LLONG | VT_UNSIGNED;
      }
      result_type.ref = NULL;

      /* For bswap64 on 32-bit target with unsigned ≤32-bit argument:
       * bswap64(zext(x32)) = bswap32(x32) << 32.
       * Decompose to avoid __bswapdi3 call and expose the zero low-word
       * to the optimizer. */
      int bswap64_from_small = 0;
#if PTR_SIZE == 4
      if (size == 8 && (vtop->type.t & VT_BTYPE) != VT_LLONG && (vtop->type.t & VT_UNSIGNED))
        bswap64_from_small = 1;
#endif

      if (bswap64_from_small)
      {
        CType uint32_type;
        uint32_type.t = VT_INT | VT_UNSIGNED;
        uint32_type.ref = NULL;
        gen_cast(&uint32_type);
        if (tcc_machine_has_bit_ops())
          gen_bitop1(TCCIR_OP_REV);
        else
          gen_builtin_libcall(TOK___bswapsi2, 1, VT_INT | VT_UNSIGNED);
        vpushi(0);
        vswap();
        lbuild(VT_LLONG | VT_UNSIGNED);
      }
      else
      {
        /* Cast to appropriate unsigned type */
        gen_cast(&result_type);

        if (size == 2)
        {
          /* bswap16: widen to 32 bits first, then swap. */
          CType uint32_type;
          uint32_type.t = VT_INT | VT_UNSIGNED;
          uint32_type.ref = NULL;
          gen_cast(&uint32_type);

          if (tcc_machine_has_bit_ops())
          {
            /* REV16 swaps the bytes inside each halfword, so the swapped value
               of the zero-extended input already sits in the low halfword. */
            gen_bitop1(TCCIR_OP_REV16);
          }
          else
          {
            /* Call __bswapsi2 library function using IR */
            gen_builtin_libcall(TOK___bswapsi2, 1, VT_INT | VT_UNSIGNED);

            /* Shift right by 16 to get the swapped 16-bit value in the low bits */
            /* Actually, for a 16-bit value 0xABCD, bswap32 gives 0xCDAB0000,
               so we need to shift right by 16 to get 0x0000CDAB */
            vpushi(16);
            gen_op(TOK_SHR);
          }

          /* Cast back to uint16 */
          gen_cast(&result_type);
        }
        else if (size == 4)
        {
          if (tcc_machine_has_bit_ops())
            gen_bitop1(TCCIR_OP_REV);
          else /* bswap32: call __bswapsi2 library function */
            gen_builtin_libcall(TOK___bswapsi2, 1, VT_INT | VT_UNSIGNED);
        }
        else
        {
          /* bswap64: emit as library call (complex on 32-bit ARM) */
          /* Call __bswapdi3 library function using IR */
          gen_builtin_libcall(TOK___bswapdi3, 1, VT_LLONG | VT_UNSIGNED);
        }
      }
    }
    break;
  }
  }
}
