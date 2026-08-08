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

/* op.c -- Binary operator dispatch (gen_op).
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* generic gen_op: handles types problems */
static HOT void gen_op_impl(int op);

/* Wrapper: propagate the `underaligned` access mark through pointer
 * arithmetic.  `packed->arr + i` / `packed->arr[i]` must reach the final
 * dereference still marked, or the backend would emit LDRD/STRD on an
 * unaligned address (UsageFault on ARMv7-M/v8-M regardless of UNALIGN_TRP).
 * Only '+' and '-' preserve address provenance; other operators produce
 * non-address values.  Over-marking is harmless (falls back to the
 * LDR/STR pair); under-marking faults, so this errs on the OR of both
 * operands. */
ST_FUNC HOT void gen_op(int op)
{
  int under = 0;
  if (op == '+' || op == '-')
    under = vtop[0].underaligned | vtop[-1].underaligned;
  gen_op_impl(op);
  if (under)
    vtop->underaligned = 1;
}

static HOT void gen_op_impl(int op)
{
  int t1, t2, bt1, bt2, t;
  CType type1, combtype;
  int op_class = op;
  int bf_trunc_size = 0;

  if (op == TOK_SHR || op == TOK_SAR || op == TOK_SHL)
    op_class = SHIFT_OP;
  else if (TOK_ISCOND(op)) /* == != > ... */
    op_class = CMP_OP;

redo:
  t1 = vtop[-1].type.t;
  t2 = vtop[0].type.t;
  bt1 = t1 & VT_BTYPE;
  bt2 = t2 & VT_BTYPE;

  /* Complex integer == and != : decompose into per-component comparisons
   * before the usual arithmetic conversions.  We do this early because the
   * runtime cast from a narrow complex type (_Complex char/short) to a wider
   * one (_Complex int) is not implemented – it would naïvely sign-extend
   * just the low byte, losing the packed imaginary part. */
  if ((op == TOK_EQ || op == TOK_NE) && ((t1 | t2) & VT_COMPLEX) && !is_float(bt1) && !is_float(bt2))
  {
    /* Promote a non-complex operand to complex (imag = 0) so the
     * decomposition helper always sees VT_COMPLEX on both sides. */
    if (!(t1 & VT_COMPLEX))
    {
      /* lhs is real: rewrite as _Complex with base type = bt1 */
      vtop[-1].type.t |= VT_COMPLEX;
      /* For constants: mask to only the real part so the imaginary
       * (high) bits are zero.  A sign-extended c.i (e.g. -1 stored
       * as 0xFFFFFFFFFFFFFFFF) would otherwise look like imag = -1. */
      if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && btype_size(bt1) < 8)
        vtop[-1].c.i &= (1ULL << (btype_size(bt1) * 8)) - 1;
    }
    if (!(t2 & VT_COMPLEX))
    {
      vtop[0].type.t |= VT_COMPLEX;
      /* Same masking for rhs constants. */
      if ((vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && btype_size(bt2) < 8)
        vtop[0].c.i &= (1ULL << (btype_size(bt2) * 8)) - 1;
    }
    gen_complex_int_cmp(op);
    return;
  }

  /* Complex float/double == and != : decompose into per-component comparisons.
   * The FCMP backend only compares the real (lo) half, so we split the
   * comparison into two scalar float/double comparisons here. */
  if ((op == TOK_EQ || op == TOK_NE) && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    if (!(t1 & VT_COMPLEX))
      vtop[-1].type.t |= VT_COMPLEX;
    if (!(t2 & VT_COMPLEX))
      vtop[0].type.t |= VT_COMPLEX;

    gen_complex_float_cmp(op);
    return;
  }

  /* Complex float/double +/- : decompose into per-component scalar operations.
   * Complex double (128 bits) does not fit in a register pair (64 bits max),
   * so we decompose at the front-end level. Complex float also uses this
   * path for consistency. Skip when both are constant (gen_opif folds).
   * Mixed scalar+complex takes this path too (the scalar's imaginary part
   * is 0) — letting it fall through to the generic conversions reduces the
   * complex operand to its real half and silently drops the result's
   * imaginary part (`x + 1.0i` lost the `i`). */
  if ((op == '+' || op == '-') && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    int l_c = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int r_c = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (!(l_c && r_c))
    {
      gen_complex_float_arith(op);
      return;
    }
  }

  /* Complex float/double * : decompose at the frontend.  Handles mixed
   * scalar+complex (scalar's imag treated as 0) via the optimal 2-mul path,
   * and complex×complex via the standard 4-mul/add/sub formula.  Routing
   * through gen_op() emits plain scalar ops, so downstream optimizer passes
   * don't have to know that the underlying memory has a complex layout.
   *
   * Division is intentionally left to the backend's __divdc3 / __divsc3
   * libgcc helpers — the naïve decomposition (c² + d²) loses precision and
   * over/underflows on extreme values that IEEE-compliant libgcc handles. */
  if (op == '*' && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    int l_c = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int r_c = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (!(l_c && r_c))
    {
      gen_complex_float_mul(op);
      return;
    }
  }

  /* Mixed scalar/complex float division: promote the scalar operand to the
   * complex type, then fall through — the both-complex path reaches the
   * backend's IEEE-correct __divdc3/__divsc3 helpers.  Without this the
   * generic conversions reduce the complex operand to its real half. */
  if (op == '/' && (((t1 ^ t2) & VT_COMPLEX) != 0) && (is_float(bt1) || is_float(bt2)))
  {
    int scalar_below = (t2 & VT_COMPLEX) != 0; /* scalar is vtop[-1] */
    CType cplx_type = scalar_below ? vtop[0].type : vtop[-1].type;
    CType scalar_ct;
    scalar_ct.t = cplx_type.t & ~VT_COMPLEX;
    scalar_ct.ref = NULL;
    if (scalar_below)
      vswap();
    gen_cast(&scalar_ct); /* align base type (no-op when equal) */
    gen_cast(&cplx_type); /* scalar → (scalar, 0) complex temp */
    if (scalar_below)
      vswap();
    t1 = vtop[-1].type.t;
    t2 = vtop[0].type.t;
    bt1 = t1 & VT_BTYPE;
    bt2 = t2 & VT_BTYPE;
  }

  /* Complex integer +, -, *, / : decompose into component-wise scalar operations.
   * Complex integers don't fit in a single 32-bit register, so non-constant
   * operations must be decomposed into real/imag scalar operations.
   * Skip when both are constant (gen_opic constant-folds those). */
  if ((op == '+' || op == '-' || op == '*' || op == '/') && ((t1 | t2) & VT_COMPLEX) && !is_float(bt1) &&
      !is_float(bt2))
  {
    int l_c = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int r_c = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (!(l_c && r_c))
    {
      /* Don't set VT_COMPLEX on non-complex operands here;
       * gen_complex_int_arith needs to know which were originally complex
       * to correctly extract imaginary parts (0 for real operands). */
      gen_complex_int_arith(op);
      return;
    }
  }

  /* C11 6.7.2.1p10: a bit-field has an integer type of the specified width.
     For unsigned long long bit-fields narrower than 64 bits but wider than
     32, arithmetic must wrap at the bit-field width, not at 64 bits.
     Track the effective width here so we can truncate after the operation. */
  bf_trunc_size = 0;
  if ((t1 & VT_BITFIELD) && (t1 & VT_UNSIGNED) && bt1 == VT_LLONG)
  {
    int bs = BIT_SIZE(t1);
    if (bs > 32 && bs < 64)
      bf_trunc_size = bs;
  }
  if ((t2 & VT_BITFIELD) && (t2 & VT_UNSIGNED) && bt2 == VT_LLONG)
  {
    int bs = BIT_SIZE(t2);
    if (bs > 32 && bs < 64 && bs > bf_trunc_size)
      bf_trunc_size = bs;
  }

  /* GCC vector extension: dispatch to element-wise scalar lowering */
  if ((t1 & VT_VECTOR) || (t2 & VT_VECTOR))
  {
    gen_op_vector(op);
    return;
  }

  if (bt1 == VT_FUNC || bt2 == VT_FUNC)
  {
    if (bt2 == VT_FUNC)
    {
      mk_pointer(&vtop->type);
      gaddrof();
    }
    if (bt1 == VT_FUNC)
    {
      vswap();
      mk_pointer(&vtop->type);
      gaddrof();
      vswap();
    }
    goto redo;
  }
  else if (!combine_types(&combtype, vtop - 1, vtop, op_class))
  {
  op_err:
    tcc_error("invalid operand types for binary operation");
  }
  else if (bt1 == VT_PTR || bt2 == VT_PTR)
  {
    /* at least one operand is a pointer */
    /* relational op: must be both pointers */
    int align;
    if (op_class == CMP_OP)
      goto std_op;
    /* if both pointers, then it must be the '-' op */
    if (bt1 == VT_PTR && bt2 == VT_PTR)
    {
      if (op != '-')
        goto op_err;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      vtop->type.t &= ~VT_UNSIGNED;
      vrott(3);
      gen_opic(op);
      vtop->type.t = VT_PTRDIFF_T;
      vswap();
      gen_op(TOK_PDIV);
    }
    else
    {
      /* exactly one pointer : must be '+' or '-'. */
      if (op != '-' && op != '+')
        goto op_err;
      /* Put pointer as first operand */
      if (bt2 == VT_PTR)
      {
        vswap();
        t = t1, t1 = t2, t2 = t;
        bt2 = bt1;
      }
#if PTR_SIZE == 4
      if (bt2 == VT_LLONG)
        /* XXX: truncate here because gen_opl can't handle ptr + long long */
        gen_cast_s(VT_INT);
#endif
      type1 = vtop[-1].type;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      gen_op('*');
#ifdef CONFIG_TCC_BCHECK
      if (tcc_state->do_bounds_check && !CONST_WANTED)
      {
        /* if bounded pointers, we generate a special code to
           test bounds */
        if (op == '-')
        {
          vpushi(0);
          vswap();
          gen_op('-');
        }
        gen_bounded_ptr_add();
      }
      else
#endif
      {
        gen_opic(op);
      }
      type1.t &= ~(VT_ARRAY | VT_VLA);
      /* put again type if gen_opic() swaped operands */
      vtop->type = type1;
    }
  }
  else
  {
    /* floats can only be used for a few operations */
    if (is_float(combtype.t) && op != '+' && op != '-' && op != '*' && op != '/' && op_class != CMP_OP)
    {
      goto op_err;
    }
  std_op:
    t = t2 = combtype.t;
    /* special case for shifts and long long: we keep the shift as
       an integer */
    if (op_class == SHIFT_OP)
      t2 = VT_INT;
    /* XXX: currently, some unsigned operations are explicit, so
       we modify them here */
    if (t & VT_UNSIGNED)
    {
      if (op == TOK_SAR)
        op = TOK_SHR;
      else if (op == '/')
        op = TOK_UDIV;
      else if (op == '%')
        op = TOK_UMOD;
      else if (op == TOK_LT)
        op = TOK_ULT;
      else if (op == TOK_GT)
        op = TOK_UGT;
      else if (op == TOK_LE)
        op = TOK_ULE;
      else if (op == TOK_GE)
        op = TOK_UGE;
    }
    vswap();
    gen_cast_s(t);
    vswap();
    gen_cast_s(t2);
    if (is_float(t))
      gen_opif(op);
    else
      gen_opic(op);
    /* Truncate result for wide unsigned bit-field arithmetic (C11 6.7.2.1p10).
       Bit-fields wider than int but narrower than their base type have their
       own effective integer type; arithmetic must wrap at the bit-field width,
       not the full long long width. */
    if (bf_trunc_size > 0 && op_class != CMP_OP)
    {
      vpush64(VT_LLONG | VT_UNSIGNED, (1ULL << bf_trunc_size) - 1);
      gen_opic('&');
    }
    if (op_class == CMP_OP)
    {
      /* relational op: the result is an int */
      vtop->type.t = VT_INT;
    }
    else if (op == TOK_UMULL || op == TOK_SMULL)
    {
      /* UMULL/SMULL produce 64-bit result from 32-bit inputs - preserve the type set by tcc_ir_gen_opi */
    }
    else
    {
      vtop->type.t = t;
    }
  }
  // Make sure that we have converted to an rvalue:
  // if (vtop->r & VT_LVAL)
  //   gv(is_float(vtop->type.t & VT_BTYPE) ? RC_FLOAT : RC_INT);
}
