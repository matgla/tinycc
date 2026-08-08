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

/* fp.c -- Floating-point builtins: alloca, classification/manipulation and modf.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Extracted from unary() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_alloca(void)
{
  CType type;
  switch (tok)
  {
/* TOK_alloca is an enum constant (tcctok.h), so it can't be tested with
 * #ifdef — guard on the same target condition that defines it. Routing the
 * plain `alloca` identifier here (instead of the lib/alloca.S call) is
 * required for correctness: the library alloca moves SP behind the
 * backend's back, so the SP-relative per-call R9/arg save area reads
 * garbage afterwards (func_dynamic_sp is only set for VLA_ALLOC). */
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM
  case TOK_alloca:
#endif
  case TOK_builtin_alloca:
  {
    /* __builtin_alloca(size) — allocate memory on the stack.
     * The allocation persists until function return (epilogue restores SP
     * from the frame pointer). */
    parse_builtin_params(0, "e"); /* size argument on vtop */
    if (tcc_state->ir)
    {
      tcc_state->force_frame_pointer = 1;

      /* Emit VLA_ALLOC: adjusts SP down by size and aligns to 8 bytes. */
      SValue size_sv = *vtop;
      tcc_ir_gen_vla_alloc(tcc_state->ir, &size_sv, 8 /* 8-byte alignment */);
      vpop(); /* pop size */

      /* Allocate a local slot to capture the resulting SP (= alloca pointer). */
      loc -= PTR_SIZE;
      int alloca_slot = loc;
      tcc_ir_gen_vla_sp_save(tcc_state->ir, alloca_slot);

      /* Push the saved pointer as the return value (void *). */
      type.t = VT_VOID;
      mk_pointer(&type);
      vset(&type, VT_LOCAL | VT_LVAL, alloca_slot);
      vtop->vr = -1;
    }
    break;
  }
  case TOK_builtin_apply_args:
  {
    /* __builtin_apply_args() — save incoming argument registers and return
     * a pointer to the saved block: [stack_args_ptr, r0, r1, r2, r3]. */
    parse_builtin_params(0, "");
    if (tcc_state->ir)
    {
      tcc_state->func_save_apply_args = 1;
      tcc_state->force_frame_pointer = 1;

      /* Allocate 20 bytes: [stack_args_ptr(4), r0(4), r1(4), r2(4), r3(4)] */
      loc = (loc - 20) & ~3;
      tcc_state->apply_args_offset = loc;

      /* Emit BUILTIN_APPLY_ARGS IR: dest vreg = address of saved block */
      SValue dest;
      memset(&dest, 0, sizeof(dest));
      dest.type.t = VT_PTR;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      dest.r = 0;
      dest.c.i = loc; /* encode stack offset for the backend */
      tcc_ir_put(tcc_state->ir, TCCIR_OP_BUILTIN_APPLY_ARGS, NULL, NULL, &dest);

      /* Push result as void* */
      type.t = VT_VOID;
      mk_pointer(&type);
      vpush(&type);
      vtop->vr = dest.vr;
      vtop->r = 0;
      vtop->c.i = 0;
    }
    break;
  }
  case TOK_builtin_apply:
  {
    /* __builtin_apply(fn, args, size) — call fn with saved argument block.
     * Restores r0-r3 from args, optionally copies stack args, calls fn. */
    parse_builtin_params(0, "eee");
    if (tcc_state->ir)
    {
      /* Stack: vtop[-2]=fn, vtop[-1]=args, vtop[0]=size */
      vpop(); /* pop size (stack copy not needed for register-only args) */

      /* Allocate 8 bytes for return value block (r0 + r1) */
      loc = (loc - 8) & ~3;
      int retval_slot = loc;

      /* Emit BUILTIN_APPLY: dest = temp vreg (call result r0),
       * src1 = fn, src2 = args */
      SValue dest;
      memset(&dest, 0, sizeof(dest));
      dest.type.t = VT_INT;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      dest.r = 0;
      dest.c.i = 0;

      tcc_ir_put(tcc_state->ir, TCCIR_OP_BUILTIN_APPLY, &vtop[-1], &vtop[0], &dest);
      vpop(); /* pop args */
      vpop(); /* pop fn */

      /* Store call result to retval block */
      SValue result_sv;
      memset(&result_sv, 0, sizeof(result_sv));
      result_sv.type.t = VT_INT;
      result_sv.vr = dest.vr;
      result_sv.r = 0;
      result_sv.c.i = 0;

      SValue store_dst;
      memset(&store_dst, 0, sizeof(store_dst));
      store_dst.type.t = VT_INT;
      store_dst.r = VT_LOCAL | VT_LVAL;
      store_dst.c.i = retval_slot;
      store_dst.vr = -1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &result_sv, NULL, &store_dst);

      /* Push address of retval block as void* */
      type.t = VT_VOID;
      mk_pointer(&type);
      vset(&type, VT_LOCAL, retval_slot);
    }
    break;
  }
  case TOK_builtin_return:
  {
    /* __builtin_return(result) — return from function with value from
     * the return-value block produced by __builtin_apply. */
    parse_builtin_params(0, "e");
    if (tcc_state->ir)
    {
      /* vtop = result (void* to return value block) */
      /* Cast to int*, dereference, and return the value */
      vtop->type.t = VT_INT;
      mk_pointer(&vtop->type);
      indir();
      tcc_ir_gen_return_value(tcc_state->ir, vtop);
      vpop();
    }
    type.t = VT_VOID;
    vpush(&type);
    CODE_OFF();
    break;
  }
  }
}

/* Extracted from unary() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_fp(void)
{
  switch (tok)
  {
  case TOK_builtin_signbit:
  case TOK_builtin_signbitf:
  {
    int tok1 = tok;
    parse_builtin_params(1, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    /* Check if argument is a compile-time constant floating point value */
    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      /* For constants, extract the sign bit from the raw representation */
      int sign_set = 0;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        sign_set = (u.i >> 31) & 1;
      }
      else if (bt == VT_DOUBLE)
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = vtop->c.d;
        sign_set = (u.i >> 63) & 1;
      }
      else /* VT_LDOUBLE */
      {
        /* For long double, check if value is negative (including -0.0) */
        sign_set = (vtop->c.ld < 0.0L) || (1.0L / vtop->c.ld < 0.0L);
      }
      vtop--;
      vpushi(sign_set);
    }
    else
    {
      /* For runtime values, extract the sign bit directly from the
       * IEEE 754 representation via type-punning through a stack temp.
       * A simple "x < 0.0" comparison would fail for -0.0 because
       * IEEE 754 says -0.0 == +0.0 numerically. */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int fp_size, fp_align, high_word_offset;

      if (tok1 == TOK_builtin_signbitf || arg_bt == VT_FLOAT)
      {
        fp_size = 4;
        fp_align = 4;
        high_word_offset = 0; /* sign bit is bit 31 of the only word */
      }
      else
      {
        /* double (or long double treated as double on ARM) */
        fp_size = 8;
        fp_align = 8;
        high_word_offset = 4; /* little-endian: sign bit is bit 31 of high word at +4 */
      }

      /* Ensure the value has the right floating-point type */
      if (tok1 == TOK_builtin_signbitf && arg_bt != VT_FLOAT)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        gen_cast(&ft);
      }
      else if (tok1 != TOK_builtin_signbitf && arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
        fp_size = 8;
        fp_align = 8;
        high_word_offset = 4;
      }

      /* Allocate a temp local to store the float/double */
      int vr_tmp;
      int tmp_loc = get_temp_local_var(fp_size, fp_align, &vr_tmp);

      /* Store the float/double to the temp local */
      SValue dst_sv;
      memset(&dst_sv, 0, sizeof(dst_sv));
      dst_sv.type = vtop->type;
      dst_sv.r = VT_LOCAL | VT_LVAL;
      dst_sv.vr = vr_tmp;
      dst_sv.c.i = tmp_loc;

      vpushv(&dst_sv);
      vswap();
      vstore();
      vtop--; /* pop the store result */

      /* Load the word containing the sign bit as an unsigned integer. */
      CType uint_type;
      uint_type.t = VT_INT | VT_UNSIGNED;
      uint_type.ref = NULL;
      vset(&uint_type, VT_LOCAL | VT_LVAL, tmp_loc + high_word_offset);
      vtop->vr = vr_tmp;

      /* Match arm-none-eabi-gcc runtime behaviour: it emits
       * `and r0, <high_word>, #0x80000000` for both signbitf and signbit,
       * returning the raw sign mask (0x80000000 = -2147483648 as signed int)
       * for negative values and 0 otherwise. */
      vpushi(0x80000000u);
      gen_op('&');
    }
    break;
  }
  case TOK_builtin_isinf:
  case TOK_builtin_isinff:
  case TOK_builtin_isinfl:
  {
    int tok1 = tok;
    parse_builtin_params(0, "e");

    /* See an inlined parameter that was bound to a constant arg through. */
    inline_subst_const_arg(vtop);

    /* Check if argument is a compile-time constant floating point value */
    int bt = vtop->type.t & VT_BTYPE;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop->r & VT_SYM) &&
        (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE))
    {
      /* For constants, check if value is infinity */
      int isinf_result = 0;
      if (bt == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t i;
        } u;
        u.f = vtop->c.f;
        uint32_t exponent = (u.i >> 23) & 0xFF;
        uint32_t mantissa = u.i & 0x7FFFFF;
        if (exponent == 0xFF && mantissa == 0)
          isinf_result = (u.i >> 31) ? -1 : 1;
      }
      else if (bt == VT_DOUBLE)
      {
        union
        {
          double d;
          uint64_t i;
        } u;
        u.d = vtop->c.d;
        uint64_t exponent = (u.i >> 52) & 0x7FF;
        uint64_t mantissa = u.i & 0xFFFFFFFFFFFFFLL;
        if (exponent == 0x7FF && mantissa == 0)
          isinf_result = (u.i >> 63) ? -1 : 1;
      }
      else /* VT_LDOUBLE */
      {
        /* For cross-compilation where host long double has more range than
         * target's (e.g. x86_64 host 80-bit -> ARM target 64-bit), convert
         * to the target representation first, then check IEEE 754 bits. */
        if (LDOUBLE_SIZE == 8)
        {
          /* Target long double is double-precision (64-bit) */
          union
          {
            double d;
            uint64_t i;
          } u;
          u.d = (double)vtop->c.ld;
          uint64_t exponent = (u.i >> 52) & 0x7FF;
          uint64_t mantissa = u.i & 0xFFFFFFFFFFFFFLL;
          if (exponent == 0x7FF && mantissa == 0)
            isinf_result = (u.i >> 63) ? -1 : 1;
        }
        else
        {
          /* Host and target long double are the same size */
          long double ld = vtop->c.ld;
          if (ld != 0.0L && ld == ld + ld)
            isinf_result = (ld < 0.0L) ? -1 : 1;
        }
      }
      vtop--;
      vpushi(isinf_result);
    }
    else
    {
      /* For runtime values, generate a call to isinf/isinff from libm.
       * Note: On ARM, long double is the same as double, so __builtin_isinfl
       * also calls isinf (not isinfl which may not be available). */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT) || (tok1 == TOK_builtin_isinff);
      const char *func_name = is_float ? "isinff" : "isinf";

      /* Ensure the argument type matches the helper we will call.
       * is_float already accounts for both the argument's type and the
       * specific builtin variant (__builtin_isinff forces float). */
      if (is_float && arg_bt != VT_FLOAT)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        gen_cast(&ft);
      }
      else if (!is_float && arg_bt == VT_FLOAT)
      {
        CType dt;
        dt.t = VT_DOUBLE;
        dt.ref = NULL;
        gen_cast(&dt);
      }

      gen_builtin_libcall(tok_alloc_const(func_name), 1, VT_INT);
    }
    break;
  }
  case TOK_builtin_copysign:
  case TOK_builtin_copysignf:
  {
    int tok1 = tok;
    parse_builtin_params(0, "ee");

    int arg_bt = vtop[-1].type.t & VT_BTYPE;
    int is_float = (arg_bt == VT_FLOAT) || (tok1 == TOK_builtin_copysignf);

    if (is_const_for_folding(&vtop[-1]) && is_const_for_folding(&vtop[0]))
    {
      if (is_float)
      {
        float mag = get_const_float(&vtop[-1]);
        float sgn = get_const_float(&vtop[0]);
        float res = copysignf(mag, sgn);
        vtop--;
        vtop->c.f = res;
        vtop->type.t = VT_FLOAT;
        vtop->r = VT_CONST;
      }
      else
      {
        double mag = get_const_double(&vtop[-1]);
        double sgn = get_const_double(&vtop[0]);
        double res = copysign(mag, sgn);
        vtop--;
        vtop->c.d = res;
        vtop->type.t = VT_DOUBLE;
        vtop->r = VT_CONST;
      }
      break;
    }

    /* Ensure both arguments match the target precision.  For
     * __builtin_copysignf the standard says the result is float, so both
     * operands must be narrowed to float before the call; without this,
     * a double argument (e.g. the literal 1.0) is passed with its raw
     * 64-bit representation and the 32-bit __copysignf helper produces
     * a wrong result. Similarly, widen float args to double for copysign. */
    if (is_float)
    {
      CType ft = {0};
      ft.t = VT_FLOAT;
      if ((vtop[-1].type.t & VT_BTYPE) != VT_FLOAT)
      {
        vswap();
        gen_cast(&ft);
        vswap();
      }
      if ((vtop[0].type.t & VT_BTYPE) != VT_FLOAT)
        gen_cast(&ft);
    }
    else
    {
      CType dt = {0};
      dt.t = VT_DOUBLE;
      if ((vtop[-1].type.t & VT_BTYPE) != VT_DOUBLE)
      {
        vswap();
        gen_cast(&dt);
        vswap();
      }
      if ((vtop[0].type.t & VT_BTYPE) != VT_DOUBLE)
        gen_cast(&dt);
    }

    gen_builtin_libcall(is_float ? TOK___copysignf : TOK___copysign, 2, is_float ? VT_FLOAT : VT_DOUBLE);
    break;
  }

  /* __builtin_isnan / __builtin_isnanf / __builtin_isnanl */
  case TOK_builtin_isnan:
  case TOK_builtin_isnanf:
  case TOK_builtin_isnanl:
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
      int isnan_result = 0;
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
        isnan_result = (exp == 0xFF && man != 0);
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
        isnan_result = (exp == 0x7FF && man != 0);
      }
      vtop--;
      vpushi(isnan_result);
    }
    else
    {
      /* Runtime: generate call to isnan/isnanf.
       *
       * A float argument keeps the FLOAT helper: widening it with
       * __aeabi_f2d just to call isnan(double) costs an extra libcall and
       * cannot change the answer (float->double is exact, and NaN stays
       * NaN).  This is the rule __builtin_isinf already follows with
       * isinff, so isnanf carries no new libm dependency. */
      int arg_bt = vtop->type.t & VT_BTYPE;
      int is_float = (arg_bt == VT_FLOAT) || (tok1 == TOK_builtin_isnanf);

      if (is_float && arg_bt != VT_FLOAT)
      {
        CType ft;
        ft.t = VT_FLOAT;
        ft.ref = NULL;
        gen_cast(&ft);
      }

      gen_builtin_libcall(is_float ? TOK___isnanf : TOK___isnan, 1, VT_INT);
    }
    break;
  }

  /* __builtin_inf / __builtin_inff / __builtin_infl — no-argument, return +Infinity */
  case TOK_builtin_inf:
  case TOK_builtin_inff:
  case TOK_builtin_infl:
  {
    int tok1 = tok;
    next();
    skip('(');
    skip(')');

    if (tok1 == TOK_builtin_inff)
    {
      union
      {
        float f;
        uint32_t i;
      } u;
      u.i = 0x7F800000U; /* +Inf float */
      CType ft;
      ft.t = VT_FLOAT;
      ft.ref = NULL;
      vpush(&ft);
      vtop->r = VT_CONST;
      vtop->c.f = u.f;
    }
    else
    {
      /* double or long double (same as double on ARM) */
      union
      {
        double d;
        uint64_t i;
      } u;
      u.i = 0x7FF0000000000000ULL; /* +Inf double */
      CType dt;
      dt.t = (tok1 == TOK_builtin_infl) ? VT_LDOUBLE : VT_DOUBLE;
      dt.ref = NULL;
      vpush(&dt);
      vtop->r = VT_CONST;
      vtop->c.d = u.d;
      if (tok1 == TOK_builtin_infl)
        vtop->c.ld = (long double)u.d;
    }
    break;
  }

  /* __builtin_nan / __builtin_nanf / __builtin_nanl — takes a string arg, return NaN */
  case TOK_builtin_nan:
  case TOK_builtin_nanf:
  case TOK_builtin_nanl:
  {
    int tok1 = tok;
    next();
    skip('(');
    /* Parse the string argument — payload is typically "" or "0x..." */
    uint64_t payload = 0;
    if (tok == TOK_STR)
    {
      const char *str = (const char *)tokc.str.data;
      if (str[0] != '\0')
      {
        char *endptr;
        payload = strtoull(str, &endptr, 0);
      }
      next();
    }
    else
    {
      expect("string constant");
    }
    skip(')');

    if (tok1 == TOK_builtin_nanf)
    {
      union
      {
        float f;
        uint32_t i;
      } u;
      /* Quiet NaN: exponent all 1s, mantissa MSB set */
      u.i = 0x7FC00000U | (uint32_t)(payload & 0x3FFFFF);
      CType ft;
      ft.t = VT_FLOAT;
      ft.ref = NULL;
      vpush(&ft);
      vtop->r = VT_CONST;
      vtop->c.f = u.f;
    }
    else
    {
      union
      {
        double d;
        uint64_t i;
      } u;
      /* Quiet NaN: exponent all 1s, mantissa MSB set */
      u.i = 0x7FF8000000000000ULL | (payload & 0x7FFFFFFFFFFFFULL);
      CType dt;
      dt.t = (tok1 == TOK_builtin_nanl) ? VT_LDOUBLE : VT_DOUBLE;
      dt.ref = NULL;
      vpush(&dt);
      vtop->r = VT_CONST;
      vtop->c.d = u.d;
      if (tok1 == TOK_builtin_nanl)
        vtop->c.ld = (long double)u.d;
    }
    break;
  }

  /* __builtin_huge_val / __builtin_huge_valf / __builtin_huge_vall — same as inf */
  case TOK_builtin_huge_val:
  case TOK_builtin_huge_valf:
  case TOK_builtin_huge_vall:
  {
    int tok1 = tok;
    next();
    skip('(');
    skip(')');

    if (tok1 == TOK_builtin_huge_valf)
    {
      union
      {
        float f;
        uint32_t i;
      } u;
      u.i = 0x7F800000U;
      CType ft;
      ft.t = VT_FLOAT;
      ft.ref = NULL;
      vpush(&ft);
      vtop->r = VT_CONST;
      vtop->c.f = u.f;
    }
    else
    {
      union
      {
        double d;
        uint64_t i;
      } u;
      u.i = 0x7FF0000000000000ULL;
      CType dt;
      dt.t = (tok1 == TOK_builtin_huge_vall) ? VT_LDOUBLE : VT_DOUBLE;
      dt.ref = NULL;
      vpush(&dt);
      vtop->r = VT_CONST;
      vtop->c.d = u.d;
      if (tok1 == TOK_builtin_huge_vall)
        vtop->c.ld = (long double)u.d;
    }
    break;
  }

  /* __builtin_isunordered(x, y) — true if either operand is NaN */
  case TOK_builtin_isunordered:
  {
    parse_builtin_params(0, "ee");

    /* See inlined parameters that were bound to constant args through. */
    inline_subst_const_arg(&vtop[-1]);
    inline_subst_const_arg(&vtop[0]);

    /* Check if both arguments are compile-time constants */
    int bt_x = vtop[-1].type.t & VT_BTYPE;
    int bt_y = vtop[0].type.t & VT_BTYPE;
    if ((vtop[-1].r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop[-1].r & VT_SYM) &&
        (vtop[0].r & (VT_VALMASK | VT_LVAL)) == VT_CONST && !(vtop[0].r & VT_SYM) &&
        (bt_x == VT_FLOAT || bt_x == VT_DOUBLE || bt_x == VT_LDOUBLE) &&
        (bt_y == VT_FLOAT || bt_y == VT_DOUBLE || bt_y == VT_LDOUBLE))
    {
      /* For constants, just check if either is NaN */
      double x = (bt_x == VT_FLOAT) ? (double)vtop[-1].c.f : vtop[-1].c.d;
      double y = (bt_y == VT_FLOAT) ? (double)vtop[0].c.f : vtop[0].c.d;
      int result = (x != x) || (y != y);
      vtop -= 2;
      vpushi(result);
    }
    else
    {
      /* Runtime: ONE AEABI unordered-compare call.  __aeabi_[df]cmpun
       * returns exactly 1 when either operand is NaN and 0 otherwise, which
       * is isunordered() by definition.
       *
       * The former lowering was `isnan(x) | isnan(y)`, which forced BOTH
       * operands through __aeabi_f2d first (isnan takes a double) and then
       * made two more calls — four libcalls where one suffices, and a float
       * pair now stays in float.  __builtin_islessgreater below already calls
       * these helpers, so this adds no new runtime dependency.
       *
       * NOTE: this makes the result an opaque CALL, so `isunordered(x,y) ||
       * !isunordered(x,y)` only stays provably true if the two calls are
       * value-numbered into one — see gvn_try_pure_call.  Without that, the
       * dead arm survives and gcc torture ieee/compare-fp-3 fails to LINK. */

      /* Take the float helper only when BOTH operands are already float.
       * Everything else — double, long double, or a mixed pair — goes through
       * the double helper, widening each side that is not already double.
       * Requiring float on BOTH sides is what keeps a non-floating operand
       * (which GCC rejects outright, but tcc does not diagnose here) from
       * reaching __aeabi_fcmpun as a raw integer bit pattern. */
      int un_is_double = !(((vtop[-1].type.t & VT_BTYPE) == VT_FLOAT) &&
                           ((vtop[0].type.t & VT_BTYPE) == VT_FLOAT));

      if (un_is_double)
      {
        if ((vtop[-1].type.t & VT_BTYPE) != VT_DOUBLE)
        {
          vswap();
          CType dt = {0};
          dt.t = VT_DOUBLE;
          gen_cast(&dt);
          vswap();
        }
        if ((vtop[0].type.t & VT_BTYPE) != VT_DOUBLE)
        {
          CType dt = {0};
          dt.t = VT_DOUBLE;
          gen_cast(&dt);
        }
      }

      gen_builtin_libcall(tok_alloc_const(un_is_double ? "__aeabi_dcmpun" : "__aeabi_fcmpun"), 2, VT_INT);
    }
    break;
  }

  /* __builtin_isless, __builtin_isgreater, __builtin_islessequal,
   * __builtin_isgreaterequal, __builtin_islessgreater
   * These are like comparison operators but do NOT raise FP exceptions on NaN.
   * For our soft-float implementation, they are equivalent to: !isunordered(x,y) && (x op y) */
  case TOK_builtin_isless:
  case TOK_builtin_isgreater:
  case TOK_builtin_islessequal:
  case TOK_builtin_isgreaterequal:
  case TOK_builtin_islessgreater:
  {
    int tok1 = tok;
    parse_builtin_params(0, "ee");

    /* Determine the comparison operator */
    int cmp_op;
    switch (tok1)
    {
    case TOK_builtin_isless:
      cmp_op = TOK_LT;
      break;
    case TOK_builtin_isgreater:
      cmp_op = TOK_GT;
      break;
    case TOK_builtin_islessequal:
      cmp_op = TOK_LE;
      break;
    case TOK_builtin_isgreaterequal:
      cmp_op = TOK_GE;
      break;
    case TOK_builtin_islessgreater:
    default:
      cmp_op = 0;
      break; /* special: x < y || x > y */
    }

    if (cmp_op != 0)
    {
      /* Simple case: x op y (returns 0 if unordered per IEEE soft-float) */
      gen_op(cmp_op);
    }
    else
    {
      /* islessgreater(x, y): true iff x < y or x > y — false if equal
       * or if either operand is NaN.
       *
       * Implement as: !(dcmpun(x,y) || dcmpeq(x,y))
       * i.e. the values are ordered AND not equal.
       *
       * Both __aeabi_dcmpun and __aeabi_dcmpeq return plain int 0/1,
       * so we OR them and invert, avoiding VT_CMP materialization
       * issues that arise from gen_op on floats. */

      int is_double = ((vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE) || ((vtop[-1].type.t & VT_BTYPE) == VT_LDOUBLE) ||
                      ((vtop[0].type.t & VT_BTYPE) == VT_DOUBLE) || ((vtop[0].type.t & VT_BTYPE) == VT_LDOUBLE);

      /* Promote float args to double if needed for consistent calling */
      if (is_double)
      {
        if ((vtop[-1].type.t & VT_BTYPE) == VT_FLOAT)
        {
          vswap();
          CType dt = {0};
          dt.t = VT_DOUBLE;
          gen_cast(&dt);
          vswap();
        }
        if ((vtop[0].type.t & VT_BTYPE) == VT_FLOAT)
        {
          CType dt = {0};
          dt.t = VT_DOUBLE;
          gen_cast(&dt);
        }
      }

      /* Save both operands — they'll be used twice (once per call) */
      SValue y_save = vtop[0];
      SValue x_save = vtop[-1];

      /* --- Call 1: dcmpun(x, y) → int (1 if NaN, 0 if ordered) --- */
      gen_builtin_libcall(tok_alloc_const(is_double ? "__aeabi_dcmpun" : "__aeabi_fcmpun"), 2, VT_INT);
      /* Stack: ... unordered_int */

      /* --- Call 2: dcmpeq(x, y) → int (1 if equal, 0 if not) --- */
      vpushv(&x_save);
      vpushv(&y_save);
      gen_builtin_libcall(tok_alloc_const(is_double ? "__aeabi_dcmpeq" : "__aeabi_fcmpeq"), 2, VT_INT);
      /* Stack: ... unordered_int equal_int */

      /* Result = !(unordered | equal) = (unordered == 0) && (equal == 0)
       * Use bitwise OR then == 0 check for branchless code. */
      gen_op('|'); /* unordered | equal */
      vpushi(0);
      gen_op(TOK_EQ); /* (unordered | equal) == 0 */
    }
    break;
  }
  }
}
/* __builtin_modff / __builtin_modf / __builtin_modfl
 * Signature: float modff(float x, float *iptr)
 *            double modf(double x, double *iptr)
 * Returns the fractional part; stores the integer part through *iptr. */
void __attribute__((noinline)) unary_builtin_modf(void)
{
  int tok1 = tok;
  next();
  skip('(');
  expr_eq();
  convert_parameter_type(&vtop->type);
  skip(',');
  expr_eq();
  convert_parameter_type(&vtop->type);
  skip(')');

  /* vstack: [..., value, pointer] */

  int is_float = (tok1 == TOK_builtin_modff);

  /* Try constant folding when the value argument is a compile-time constant */
  if (is_const_for_folding(&vtop[-1]))
  {
    CValue ipart_cv, frac_cv;
    memset(&ipart_cv, 0, sizeof(ipart_cv));
    memset(&frac_cv, 0, sizeof(frac_cv));
    int ret_bt;

    if (is_float)
    {
      float val = get_const_float(&vtop[-1]);
      float ipart;
      float frac = modff(val, &ipart);
      ipart_cv.f = ipart;
      frac_cv.f = frac;
      ret_bt = VT_FLOAT;
    }
    else
    {
      double val = get_const_double(&vtop[-1]);
      double ipart;
      double frac = modf(val, &ipart);
      if (tok1 == TOK_builtin_modfl)
      {
        ipart_cv.ld = ipart;
        frac_cv.ld = frac;
        ret_bt = VT_LDOUBLE;
      }
      else
      {
        ipart_cv.d = ipart;
        frac_cv.d = frac;
        ret_bt = VT_DOUBLE;
      }
    }

    /* Store the integer part through the pointer:
     * vtop[0] = pointer, dereference it and store the constant */
    SValue ptr_sv = vtop[0];
    vtop--;            /* pop pointer, value is now on top */
    vtop[0] = ptr_sv;  /* replace value with pointer */
    indir();           /* dereference: pointer → lvalue */

    CType ct;
    ct.t = ret_bt;
    ct.ref = NULL;
    vsetc(&ct, VT_CONST, &ipart_cv); /* push the integer part constant */
    vstore();          /* store integer part to *iptr */
    vpop();            /* pop stored value left by vstore */

    /* Push the fractional part as the result */
    CType rt;
    rt.t = ret_bt;
    rt.ref = NULL;
    vsetc(&rt, VT_CONST, &frac_cv);
  }
  else
  {
    /* Runtime: emit a call to modff/modf/modfl */
    const char *func_name;
    int ret_type;
    if (tok1 == TOK_builtin_modff)
    {
      func_name = "modff";
      ret_type = VT_FLOAT;
    }
    else if (tok1 == TOK_builtin_modf)
    {
      func_name = "modf";
      ret_type = VT_DOUBLE;
    }
    else
    {
      func_name = "modfl";
      ret_type = VT_LDOUBLE;
    }
    gen_builtin_libcall(tok_alloc_const(func_name), 2, ret_type);
  }
}
