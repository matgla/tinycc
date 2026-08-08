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

/* overflow.c -- Overflow-checked arithmetic builtins.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Extracted from unary() to reduce stack frame size. */
void __attribute__((noinline)) unary_builtin_overflow(void)
{
  switch (tok)
  {
  case TOK_builtin_add_overflow:
  case TOK_builtin_sub_overflow:
  case TOK_builtin_mul_overflow:
  case TOK_builtin_sadd_overflow:
  case TOK_builtin_uadd_overflow:
  case TOK_builtin_ssub_overflow:
  case TOK_builtin_usub_overflow:
  case TOK_builtin_umul_overflow:
  {
    /* __builtin_{add,sub,mul}_overflow(a, b, *res) — type-generic
     * __builtin_{s,u}{add,sub,mul}_overflow(T a, T b, T *res) — typed (int)
     *
     * Implementation for result types <= 32 bits: widen operands to
     * long long, perform the operation, truncate to the result type,
     * store through the pointer, then sign/zero-extend the truncated
     * value back and compare with the wide result to detect overflow. */
    int op_tok = tok;
    CType res_type;

    next();
    skip('(');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(',');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(',');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(')');

    /* Stack: a  b  res_ptr */

    if (!(vtop->type.t & VT_PTR))
      tcc_error("third argument to overflow builtin must be a pointer");
    res_type = *pointed_type(&vtop->type);
    int res_bt = res_type.t & VT_BTYPE;
    int is_unsigned;

    switch (op_tok)
    {
    case TOK_builtin_uadd_overflow:
    case TOK_builtin_usub_overflow:
    case TOK_builtin_umul_overflow:
      is_unsigned = 1;
      break;
    case TOK_builtin_sadd_overflow:
    case TOK_builtin_ssub_overflow:
    case TOK_builtin_smul_overflow:
      is_unsigned = 0;
      break;
    default:
      is_unsigned = (res_type.t & VT_UNSIGNED) != 0;
      break;
    }

    int arith_tok;
    switch (op_tok)
    {
    case TOK_builtin_add_overflow:
    case TOK_builtin_sadd_overflow:
    case TOK_builtin_uadd_overflow:
      arith_tok = '+';
      break;
    case TOK_builtin_sub_overflow:
    case TOK_builtin_ssub_overflow:
    case TOK_builtin_usub_overflow:
      arith_tok = '-';
      break;
    default:
      arith_tok = '*';
      break;
    }

    if (res_bt == VT_LLONG)
    {
      /* 64-bit result: can't widen further on 32-bit target.
       * Use arithmetic overflow checks instead. */

      /* Stack: a  b  res_ptr → res_ptr  a  b */
      vrott(3);

      /* For the type-generic __builtin_mul_overflow with unsigned 64-bit
       * result but signed inputs that fit in 32 bits, the infinite-precision
       * product always fits in signed long long.  Overflow into unsigned
       * long long means the signed product is negative.  Use signed
       * multiplication so we can test the sign bit afterwards. */
      int a_bt = vtop[-1].type.t & VT_BTYPE;
      int b_bt = vtop[0].type.t & VT_BTYPE;
      int a_signed = !(vtop[-1].type.t & VT_UNSIGNED);
      int b_signed = !(vtop[0].type.t & VT_UNSIGNED);
      int signed_to_unsigned_mul = is_unsigned && arith_tok == '*' && op_tok == TOK_builtin_mul_overflow &&
                                   (a_signed || b_signed) && (a_bt <= VT_INT && b_bt <= VT_INT);

      CType ll_type;
      ll_type.ref = NULL;
      if (signed_to_unsigned_mul)
        ll_type.t = VT_LLONG; /* signed — preserve sign for overflow check */
      else
        ll_type.t = is_unsigned ? (VT_LLONG | VT_UNSIGNED) : VT_LLONG;
      gen_cast(&ll_type); /* cast b */
      vswap();
      gen_cast(&ll_type); /* cast a */
      vswap();
      /* Stack: res_ptr  a  b */

      /* Save copies of a and b for the overflow check. */
      vpushv(vtop);     /* Stack: res_ptr  a  b  b2 */
      vrott(4);         /* Stack: b2  res_ptr  a  b */
      vpushv(vtop - 1); /* Stack: b2  res_ptr  a  b  a2 */
      vrott(5);         /* Stack: a2  b2  res_ptr  a  b */

      gen_op(arith_tok); /* Stack: a2  b2  res_ptr  result */

      /* For all cases except pure-unsigned mul, save a result copy. */
      int need_result = !(is_unsigned && arith_tok == '*') || signed_to_unsigned_mul;
      if (need_result)
      {
        vpushv(vtop); /* Stack: a2  b2  res_ptr  result  r2 */
        vrott(3);     /* Stack: a2  b2  r2  res_ptr  result */
      }

      /* Store result through pointer. */
      vswap();  /* ...  result  res_ptr */
      indir();  /* ...  result  *res_ptr */
      vswap();  /* ...  *res_ptr  result */
      vstore(); /* pops rvalue, lvalue remains */
      vpop();   /* discard lvalue leftover */

      /* After store:
       *   need_result true:  a2  b2  r2
       *   need_result false: a2  b2
       */

      if (is_unsigned && arith_tok == '+')
      {
        /* unsigned add overflow: result < a
         * Stack: a2  b2  r2 */
        vswap();        /* a2  r2  b2 */
        vpop();         /* a2  r2 */
        vswap();        /* r2  a2 */
        gen_op(TOK_LT); /* r2 < a2 */
      }
      else if (is_unsigned && arith_tok == '-')
      {
        /* unsigned sub overflow: a < result
         * Stack: a2  b2  r2 */
        vswap();        /* a2  r2  b2 */
        vpop();         /* a2  r2 */
        gen_op(TOK_LT); /* a2 < r2 */
      }
      else if (!is_unsigned && arith_tok == '+')
      {
        /* signed add overflow: ((a ^ r) & (b ^ r)) < 0
         *
         * Compute (b ^ r) first (like sub computes (a ^ b)),
         * then (a ^ r), then AND. Stack: a2  b2  r2 */

        /* Push b2 (at vtop-1 before any pushes) */
        vpushv(vtop - 1);
        /* Now vtop = b2copy, vtop-1 = r2. Push r2 for (b ^ r). */
        vpushv(vtop - 1);
        gen_op('^'); /* a2  b2  r2  (b ^ r) */

        /* For (a ^ r), need a2 and r2.
         * Stack: a2  b2  r2  xor_br
         * vtop = xor_br, vtop-1 = r2, vtop-2 = b2, vtop-3 = a2 */
        vpushv(vtop - 3); /* ...  xor_br  a4  (vtop-3 = a2) */
        vpushv(vtop - 2); /* ...  xor_br  a4  r4  (vtop-2 = r2) */
        gen_op('^');      /* a2  b2  r2  xor_br  xor_ar */

        gen_op('&'); /* a2  b2  r2  (xor_br & xor_ar) */

        vpushi(0);
        gen_op(TOK_LT); /* overflow_flag  a2  b2  r2 */

        /* Discard unused copies */
        vrott(4);
        vpop();
        vpop();
        vpop(); /* overflow_flag */
      }
      else if (!is_unsigned && arith_tok == '-')
      {
        /* signed sub overflow: ((a ^ b) & (a ^ result)) < 0
         * Stack: a2  b2  r2 */

        /* Need copies of a2 for both XORs. */
        vpushv(vtop - 2); /* a2  b2  r2  a3  (vtop-2 = a2) */
        vpushv(vtop - 2); /* a2  b2  r2  a3  b3  (vtop-2 = b2) */

        /* Compute a ^ b: a3  b3 on top */
        gen_op('^'); /* a2  b2  r2  (a3^b3) = xor_ab */

        /* Compute a ^ result: need a2 and r2 copies */
        vpushv(vtop - 3); /* ...  xor_ab  a4  (vtop-3 = a2) */
        vpushv(vtop - 2); /* ...  xor_ab  a4  r3  (vtop-2 = r2) */
        gen_op('^');      /* a2  b2  r2  xor_ab  (a4^r3) = xor_ar */

        gen_op('&'); /* a2  b2  r2  (xor_ab & xor_ar) */

        vpushi(0);
        gen_op(TOK_LT); /* combined < 0 */

        /* Stack: a2  b2  r2  overflow_flag — discard unused copies */
        vrott(4); /* overflow_flag  a2  b2  r2 */
        vpop();
        vpop();
        vpop(); /* overflow_flag */
      }
      else if (signed_to_unsigned_mul)
      {
        /* Signed inputs multiplied into unsigned 64-bit result.
         * Both inputs are ≤ 32-bit, so the signed product always fits
         * in signed long long.  Overflow into unsigned long long
         * simply means the signed product is negative.
         * Stack: a2  b2  r2 */
        vrott(3); /* r2  a2  b2 */
        vpop();
        vpop(); /* r2 */

        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpushi(0);
          gen_cast(&sll);
        }
        gen_op(TOK_LT); /* r2 < 0  →  overflow_flag */
      }
      else if (is_unsigned && arith_tok == '*')
      {
        /* unsigned mul overflow: UINT64_MAX / (a | (a==0)) < b
         * Stack: a2  b2
         * Use safe_a = a | (a==0) to avoid division by zero. */

        /* Compute a == 0 */
        vpushv(vtop - 1); /* a2  b2  a3 */
        vpushi(0);
        gen_cast(&ll_type);
        gen_op(TOK_EQ); /* a2  b2  (a3==0) */

        /* Compute a3 | (a3==0) = safe_a */
        vpushv(vtop - 2); /* a2  b2  (a==0)  a4  (vtop-2 = a2) */
        gen_op('|');      /* a2  b2  ((a==0)|a4) = safe_a */

        /* Push UINT64_MAX */
        {
          CType ull_type;
          ull_type.t = VT_LLONG | VT_UNSIGNED;
          ull_type.ref = NULL;
          vpush(&ull_type);
          vtop->r = VT_CONST;
          vtop->c.i = -1; /* UINT64_MAX */
        }
        /* Stack: a2  b2  safe_a  UINT64_MAX */

        vswap();     /* a2  b2  UINT64_MAX  safe_a */
        gen_op('/'); /* a2  b2  limit */

        /* Check limit < b */
        vswap();        /* a2  limit  b2 */
        gen_op(TOK_LT); /* a2  (limit < b2) */

        /* Discard a2 */
        vswap();
        vpop(); /* overflow_flag */
      }
      else
      {
        /* signed mul overflow: branchless division round-trip.
         *
         * safe_a = a + (a==0) + 2*(a==-1)   [maps 0→1, -1→1, else unchanged]
         * div_check = (result / safe_a != b)
         * a_normal  = (a != 0) & (a != -1)
         * base_ovf  = div_check & a_normal
         * edge1 = (a == -1) & (b == LLONG_MIN)
         * edge2 = (b == -1) & (a == LLONG_MIN)
         * overflow = base_ovf | edge1 | edge2
         *
         * Stack: a2  b2  r2 */

        /* --- Compute safe_a = a + (a==0) + 2*(a==-1) --- */
        vpushv(vtop - 2); /* ...  a3 */
        vpushi(0);
        gen_cast(&ll_type);
        gen_op(TOK_EQ); /* ...  (a==0) */

        vpushv(vtop - 3); /* ...  (a==0)  a4   (vtop-3 = a2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = -1;
        } /* ...  (a==0)  a4  -1LL */
        gen_op(TOK_EQ); /* ...  (a==0)  (a4==-1) */

        vpushi(2);
        gen_op('*'); /* ...  (a==0)  2*(a==-1) */
        gen_op('+'); /* ...  ((a==0) + 2*(a==-1)) = adjustment */

        vpushv(vtop - 3); /* ...  adj  a5   (vtop-3 = a2) */
        gen_op('+');      /* ...  (a5 + adj) = safe_a */

        /* Stack: a2  b2  r2  safe_a */

        /* --- Compute div_check = (r2 / safe_a != b2) --- */
        vpushv(vtop - 1); /* ...  safe_a  r3   (vtop-1 = r2) */
        vswap();          /* ...  r3  safe_a */
        gen_op('/');      /* ...  (r3 / safe_a) = quot */

        vpushv(vtop - 2); /* ...  quot  b3   (vtop-2 = b2) */
        gen_op(TOK_NE);   /* ...  (quot != b3) = div_check */

        /* Stack: a2  b2  r2  div_check */

        /* --- Compute a_normal = (a != 0) & (a != -1) --- */
        vpushv(vtop - 3); /* ...  div_check  a6   (vtop-3 = a2) */
        vpushi(0);
        gen_cast(&ll_type);
        gen_op(TOK_NE); /* (a6 != 0) */

        vpushv(vtop - 4); /* ...  (a!=0)  a7   (vtop-4 = a2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = -1;
        }
        gen_op(TOK_NE); /* (a7 != -1) */
        gen_op('&');    /* a_normal = (a!=0) & (a!=-1) */

        /* Stack: a2  b2  r2  div_check  a_normal */
        gen_op('&'); /* base_ovf = div_check & a_normal */

        /* Stack: a2  b2  r2  base_ovf */

        /* --- edge1 = (a == -1) & (b == LLONG_MIN) --- */
        vpushv(vtop - 3); /* ...  base_ovf  a8   (vtop-3 = a2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = -1;
        }
        gen_op(TOK_EQ); /* (a8 == -1) */

        vpushv(vtop - 3); /* ...  (a==-1)  b4   (vtop-3 = b2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = (int64_t)((uint64_t)1 << 63); /* LLONG_MIN */
        }
        gen_op(TOK_EQ); /* (b4 == LLONG_MIN) */
        gen_op('&');    /* edge1 */

        /* Stack: a2  b2  r2  base_ovf  edge1 */
        gen_op('|'); /* base_ovf | edge1 */

        /* --- edge2 = (b == -1) & (a == LLONG_MIN) --- */
        vpushv(vtop - 2); /* ...  (base|e1)  b5   (vtop-2 = b2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = -1;
        }
        gen_op(TOK_EQ); /* (b5 == -1) */

        vpushv(vtop - 4); /* ...  (b==-1)  a9   (vtop-4 = a2) */
        {
          CType sll;
          sll.t = VT_LLONG;
          sll.ref = NULL;
          vpush(&sll);
          vtop->r = VT_CONST;
          vtop->c.i = (int64_t)((uint64_t)1 << 63);
        }
        gen_op(TOK_EQ); /* (a9 == LLONG_MIN) */
        gen_op('&');    /* edge2 */

        /* Stack: a2  b2  r2  (base|e1)  edge2 */
        gen_op('|'); /* overflow = (base|e1) | edge2 */

        /* Stack: a2  b2  r2  overflow_flag — discard unused copies */
        vrott(4);
        vpop();
        vpop();
        vpop(); /* overflow_flag */
      }

      break;
    }

    /* 32-bit or smaller result: widen to long long, compute, truncate, compare */
    vrott(3); /* → res_ptr  a  b */

    /* Widen both operands to (unsigned) long long */
    CType wide_type;
    wide_type.ref = NULL;
    wide_type.t = is_unsigned ? (VT_LLONG | VT_UNSIGNED) : VT_LLONG;

    gen_cast(&wide_type); /* cast b */
    vswap();
    gen_cast(&wide_type); /* cast a */
    vswap();
    /* Stack: res_ptr  a_wide  b_wide */

    gen_op(arith_tok);
    /* Stack: res_ptr  wide_result */

    vpushv(vtop); /* dup wide_result */
    /* Stack: res_ptr  wide_result  wide_result2 */

    gen_cast(&res_type); /* truncate copy to result type */
    /* Stack: res_ptr  wide_result  truncated */

    vpushv(vtop); /* dup truncated */
    /* Stack: res_ptr  wide_result  truncated  truncated2 */

    gen_cast(&wide_type); /* re-extend for comparison */
    /* Stack: res_ptr  wide_result  truncated  extended */

    /* Bring wide_result next to extended for comparison.
     * vrotb(3) moves vtop[-2] to vtop within the top 3:
     * [wide_result  truncated  extended] → [truncated  extended  wide_result] */
    vrotb(3);
    /* Stack: res_ptr  truncated  extended  wide_result */

    gen_op(TOK_NE);
    /* Stack: res_ptr  truncated  overflow_flag */

    /* Rearrange to store truncated through res_ptr.
     * Need: [overflow_flag ... *res_ptr  truncated] for vstore. */
    vrott(3);
    /* Stack: overflow_flag  res_ptr  truncated */

    vswap();
    /* Stack: overflow_flag  truncated  res_ptr */

    indir(); /* res_ptr → *res_ptr (lvalue) */
    /* Stack: overflow_flag  truncated  *res_ptr */

    vswap();
    /* Stack: overflow_flag  *res_ptr  truncated */

    vstore();
    /* vstore pops rvalue; lvalue remains → Stack: overflow_flag  *res_ptr' */

    vpop(); /* discard the store result */
    /* Stack: overflow_flag — this is our return value */

    break;
  }
  case TOK_builtin_add_overflow_p:
  case TOK_builtin_sub_overflow_p:
  case TOK_builtin_mul_overflow_p:
  {
    /* __builtin_{add,sub,mul}_overflow_p(a, b, dummy) — type-generic predicate
     *
     * Similar to the _overflow builtins, but instead of storing the result
     * through a pointer, this just returns whether overflow would occur.
     * The third argument is a dummy value of the result type (not a pointer).
     *
     * Implementation: widen operands to long long, perform the operation,
     * truncate to the result type, sign/zero-extend back and compare with
     * the wide result to detect overflow. */
    int op_tok = tok;
    CType dummy_type;

    next();
    skip('(');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(',');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(',');
    expr_eq();
    convert_parameter_type(&vtop->type);
    skip(')');

    /* Stack: a  b  dummy */

    /* Get the result type from the dummy argument (it's a value, not a pointer) */
    dummy_type = vtop->type;
    int res_bt = dummy_type.t & VT_BTYPE;
    int is_unsigned = (dummy_type.t & VT_UNSIGNED) != 0;

    /* Pop the dummy value - we only need its type */
    vpop();
    /* Stack: a  b */

    int arith_tok;
    switch (op_tok)
    {
    case TOK_builtin_add_overflow_p:
      arith_tok = '+';
      break;
    case TOK_builtin_sub_overflow_p:
      arith_tok = '-';
      break;
    default:
      arith_tok = '*';
      break;
    }

    if (res_bt == VT_LLONG)
    {
      /* 64-bit result: can't widen further on 32-bit target.
       * Use arithmetic overflow checks. */

      /* Stack: a  b */
      CType ll_type;
      ll_type.ref = NULL;
      ll_type.t = is_unsigned ? (VT_LLONG | VT_UNSIGNED) : VT_LLONG;
      gen_cast(&ll_type); /* cast b */
      vswap();
      gen_cast(&ll_type); /* cast a */
      vswap();
      /* Stack: a  b (both widened) */

      /* Save copies of a and b for the overflow check. */
      vpushv(vtop);     /* Stack: a  b  b2 */
      vrott(3);         /* Stack: b2  a  b */
      vpushv(vtop - 1); /* Stack: b2  a  b  a2 */
      vrott(4);         /* Stack: a2  b2  a  b */

      gen_op(arith_tok); /* Stack: a2  b2  result */

      /* For all cases except pure-unsigned mul, save a result copy. */
      int need_result = !(is_unsigned && arith_tok == '*');
      if (need_result)
      {
        vpushv(vtop); /* Stack: a2  b2  result  r2 */
        vrott(3);     /* Stack: a2  b2  r2  result */
      }

      /* Discard the result (we don't store it for _overflow_p) */
      vpop();
      /* After pop:
       *   need_result true:  a2  b2  r2
       *   need_result false: a2  b2
       */

      if (is_unsigned && arith_tok == '+')
      {
        /* unsigned add overflow: result < a */
        vswap();        /* a2  r2  b2 */
        vpop();         /* a2  r2 */
        vswap();        /* r2  a2 */
        gen_op(TOK_LT); /* r2 < a2 */
      }
      else if (is_unsigned && arith_tok == '-')
      {
        /* unsigned sub overflow: a < result */
        vswap();        /* a2  r2  b2 */
        vpop();         /* a2  r2 */
        gen_op(TOK_LT); /* a2 < r2 */
      }
      else if (!is_unsigned && arith_tok == '+')
      {
        /* signed add overflow: ((a ^ result) & (b ^ result)) < 0
         *
         * Note: after vrott(3)+vpop above, the actual stack layout is
         *   a2  r2  b2  (vrott moves top to deepest in the 3-group).
         * Indices below account for that layout. */
        vpushv(vtop - 1); /* a2  r2  b2  r2copy */
        vpushv(vtop - 1); /* a2  r2  b2  r2copy  b2copy */
        gen_op('^');      /* a2  r2  b2  (r ^ b)  [== (b ^ r)] */
        vpushv(vtop - 3); /* ...  (b^r)  a2 */
        vpushv(vtop - 3); /* ...  (b^r)  a2  r2 */
        gen_op('^');      /* a2  r2  b2  (b^r)  (a^r) */
        gen_op('&');      /* a2  r2  b2  ((b^r) & (a^r)) */
        vpushi(0);
        gen_op(TOK_LT); /* a2  r2  b2  overflow_flag */
        /* Discard unused copies */
        vrott(4);
        vpop();
        vpop();
        vpop();
      }
      else if (!is_unsigned && arith_tok == '-')
      {
        /* signed sub overflow: ((a ^ b) & (a ^ result)) < 0 */
        vpushv(vtop - 2); /* a2  b2  r2  a3 */
        vpushv(vtop - 2); /* a2  b2  r2  a3  b3 */
        gen_op('^');      /* a2  b2  r2  xor_ab */
        vpushv(vtop - 3); /* ...  xor_ab  a4 */
        vpushv(vtop - 2); /* ...  xor_ab  a4  r3 */
        gen_op('^');      /* a2  b2  r2  xor_ab  xor_ar */
        gen_op('&');      /* a2  b2  r2  (xor_ab & xor_ar) */
        vpushi(0);
        gen_op(TOK_LT); /* overflow_flag  a2  b2  r2 */
        /* Discard unused copies */
        vrott(4);
        vpop();
        vpop();
        vpop();
      }
      else if (is_unsigned && arith_tok == '*')
      {
        /* unsigned mul overflow: UINT64_MAX / (a | (a==0)) < b */
        /* Stack: a2  b2 */
        /* Compute a == 0 */
        vpushv(vtop - 1); /* a2  b2  a3 */
        vpushi(0);
        gen_cast(&ll_type);
        gen_op(TOK_EQ); /* a2  b2  (a3==0) */
        /* Compute a3 | (a3==0) = safe_a */
        vpushv(vtop - 2); /* a2  b2  (a==0)  a4 */
        gen_op('|');      /* a2  b2  safe_a */
        /* Push UINT64_MAX */
        {
          CType ull_type;
          ull_type.t = VT_LLONG | VT_UNSIGNED;
          ull_type.ref = NULL;
          vpush(&ull_type);
          vtop->r = VT_CONST;
          vtop->c.i = -1;
        }
        /* Stack: a2  b2  safe_a  UINT64_MAX */
        vswap();     /* a2  b2  UINT64_MAX  safe_a */
        gen_op('/'); /* a2  b2  limit */
        /* Check limit < b */
        vswap();        /* a2  limit  b2 */
        gen_op(TOK_LT); /* a2  (limit < b2) */
        /* Discard a2 */
        vswap();
        vpop();
      }
      else
      {
        /* signed mul overflow: branchless division round-trip. */
        CType sll;
        sll.t = VT_LLONG;
        sll.ref = NULL;

        /* --- Compute safe_a = a + (a==0) + 2*(a==-1) --- */
        /* Note: actual stack is a2  r2  b2 (vrott moves top to deepest) */
        vpushv(vtop - 2); /* ...  a3 */
        vpushi(0);
        gen_cast(&sll);
        gen_op(TOK_EQ);   /* ...  (a==0) */
        vpushv(vtop - 3); /* ...  (a==0)  a4 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = -1;
        gen_op(TOK_EQ); /* ...  (a==0)  (a4==-1) */
        vpushi(2);
        gen_op('*');      /* ...  (a==0)  2*(a==-1) */
        gen_op('+');      /* ...  adjustment */
        vpushv(vtop - 3); /* ...  adj  a5 */
        gen_op('+');      /* ...  safe_a */
        /* Stack: a2  b2  r2  safe_a */

        /* --- Compute div_check = (r2 / safe_a != b2) --- */
        vpushv(vtop - 2); /* ...  safe_a  r3 */
        vswap();          /* ...  r3  safe_a */
        gen_op('/');      /* ...  quot */
        vpushv(vtop - 1); /* ...  quot  b3 */
        gen_op(TOK_NE);   /* ...  div_check */
        /* Stack: a2  b2  r2  div_check */

        /* --- Compute a_normal = (a != 0) & (a != -1) --- */
        vpushv(vtop - 3); /* ...  div_check  a6 */
        vpushi(0);
        gen_cast(&sll);
        gen_op(TOK_NE);   /* (a6 != 0) */
        vpushv(vtop - 4); /* ...  (a!=0)  a7 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = -1;
        gen_op(TOK_NE); /* (a7 != -1) */
        gen_op('&');    /* a_normal */
        /* Stack: a2  b2  r2  div_check  a_normal */
        gen_op('&'); /* base_ovf */
        /* Stack: a2  b2  r2  base_ovf */

        /* --- edge1 = (a == -1) & (b == LLONG_MIN) --- */
        vpushv(vtop - 3); /* ...  base_ovf  a8 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = -1;
        gen_op(TOK_EQ);   /* (a8 == -1) */
        vpushv(vtop - 2); /* ...  (a==-1)  b4 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = (int64_t)((uint64_t)1 << 63); /* LLONG_MIN */
        gen_op(TOK_EQ);                           /* (b4 == LLONG_MIN) */
        gen_op('&');                              /* edge1 */
        /* Stack: a2  r2  b2  base_ovf  edge1 */
        gen_op('|');

        /* --- edge2 = (b == -1) & (a == LLONG_MIN) --- */
        vpushv(vtop - 1); /* ...  (base|e1)  b5 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = -1;
        gen_op(TOK_EQ);   /* (b5 == -1) */
        vpushv(vtop - 4); /* ...  (b==-1)  a9 */
        vpush(&sll);
        vtop->r = VT_CONST;
        vtop->c.i = (int64_t)((uint64_t)1 << 63);
        gen_op(TOK_EQ); /* (a9 == LLONG_MIN) */
        gen_op('&');    /* edge2 */
        /* Stack: a2  b2  r2  (base|e1)  edge2 */
        gen_op('|'); /* overflow */
        /* Discard unused copies */
        vrott(4);
        vpop();
        vpop();
        vpop();
      }

      break;
    }

    /* 32-bit or smaller result: widen to long long, compute, truncate, compare */
    /* Widen both operands to (unsigned) long long */
    CType wide_type;
    wide_type.ref = NULL;
    wide_type.t = is_unsigned ? (VT_LLONG | VT_UNSIGNED) : VT_LLONG;

    gen_cast(&wide_type); /* cast b */
    vswap();
    gen_cast(&wide_type); /* cast a */
    vswap();
    /* Stack: a_wide  b_wide */

    gen_op(arith_tok);
    /* Stack: wide_result */

    vpushv(vtop); /* dup wide_result */
    /* Stack: wide_result  wide_result2 */

    gen_cast(&dummy_type); /* truncate copy to result type */
    /* Stack: wide_result  truncated */

    gen_cast(&wide_type); /* re-extend for comparison */
    /* Stack: wide_result  extended */

    gen_op(TOK_NE);
    /* Stack: overflow_flag - this is our return value */

    break;
  }
  }
}
