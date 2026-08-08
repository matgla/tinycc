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

/* float.c -- Floating-point operator lowering and negation.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#if defined TCC_TARGET_X86_64 || defined TCC_TARGET_I386
#define gen_negf gen_opf
#elif defined TCC_TARGET_ARM
void gen_negf(int op)
{
  /* IEEE 754: negate(x) must flip the sign bit, not compute 0-x.
   * 0-x produces +0 for -0 input and vice-versa, and also differs
   * for NaN payloads.  Use IR FNEG which XORs the sign bit. */
  tcc_ir_gen_f(tcc_state->ir, 'n');
}
#else
/* XXX: implement in gen_opf() for other backends too */
void gen_negf(int op)
{
  /* In IEEE negate(x) isn't subtract(0,x).  Without NaNs it's
     subtract(-0, x), but with them it's really a sign flip
     operation.  We implement this with bit manipulation and have
     to do some type reinterpretation for this, which TCC can do
     only via memory.  */

  int align, size, bt;

  size = type_size(&vtop->type, &align);
  bt = vtop->type.t & VT_BTYPE;
  gv(RC_TYPE(bt));
  vdup();
  incr_bf_adr(size - 1);
  vdup();
  vpushi(0x80); /* flip sign */
  gen_op('^');
  vstore();
  vpop();
}
#endif

/* generate a floating point operation with constant propagation */
void gen_opif(int op)
{
  int c1, c2, i, bt;
  SValue *v1, *v2;
#if defined _MSC_VER && defined __x86_64__
  /* avoid bad optimization with f1 -= f2 for f1:-0.0, f2:0.0 */
  volatile
#endif
      long double f1,
      f2;

  v1 = vtop - 1;
  v2 = vtop;
  if (op == TOK_NEG)
    v1 = v2;
  bt = v1->type.t & VT_BTYPE;

  /* currently, we cannot do computations with forward symbols */
  c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

  /* Complex float/double constant folding: operate component-wise */
  if (c1 && c2 && ((v1->type.t | v2->type.t) & VT_COMPLEX))
  {
    double r1 = 0, i1 = 0, r2 = 0, i2 = 0, rr, ri;

    /* Extract components from v1 */
    if (bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(v1->c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(v1->c.i >> 32);
      r1 = a.f;
      i1 = b.f;
    }
    else
    {
      memcpy(&r1, &v1->c, 8);
      memcpy(&i1, (char *)&v1->c + 8, 8);
    }

    /* Extract components from v2 */
    int bt2v = v2->type.t & VT_BTYPE;
    if (bt2v == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(v2->c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(v2->c.i >> 32);
      r2 = a.f;
      i2 = b.f;
    }
    else
    {
      memcpy(&r2, &v2->c, 8);
      memcpy(&i2, (char *)&v2->c + 8, 8);
    }

    switch (op)
    {
    case '+':
      rr = r1 + r2;
      ri = i1 + i2;
      break;
    case '-':
      rr = r1 - r2;
      ri = i1 - i2;
      break;
    case '*':
      rr = r1 * r2 - i1 * i2;
      ri = r1 * i2 + i1 * r2;
      break;
    case '/':
    {
      double denom = r2 * r2 + i2 * i2;
      rr = (r1 * r2 + i1 * i2) / denom;
      ri = (i1 * r2 - r1 * i2) / denom;
      break;
    }
    case TOK_EQ:
      i = (r1 == r2) && (i1 == i2);
      vtop -= 2;
      vpushi(i);
      return;
    case TOK_NE:
      i = (r1 != r2) || (i1 != i2);
      vtop -= 2;
      vpushi(i);
      return;
    default:
      goto general_case;
    }

    vtop--;
    /* Pack result */
    memset(&v1->c, 0, sizeof(CValue));
    if (bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.f = (float)rr;
      b.f = (float)ri;
      v1->c.i = (uint64_t)a.u | ((uint64_t)b.u << 32);
    }
    else
    {
      double dr = rr, di = ri;
      memcpy(&v1->c, &dr, 8);
      memcpy((char *)&v1->c + 8, &di, 8);
    }
    return;
  }

  /* IEEE 754: any ordered comparison with NaN yields false,
     unordered (!=) yields true.  If exactly one operand is a
     compile-time NaN constant, fold the comparison. */
  if ((c1 || c2) && !(c1 && c2))
  {
    int is_cmp = (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE);
    if (is_cmp)
    {
      SValue *cv = c1 ? v1 : v2;
      long double fv;
      if (bt == VT_FLOAT)
        fv = cv->c.f;
      else if (bt == VT_DOUBLE)
        fv = cv->c.d;
      else
        fv = cv->c.ld;
      /* NaN is the only value where fv != fv */
      if (fv != fv)
      {
        i = (op == TOK_NE) ? 1 : 0;
        vtop -= 2;
        vpushi(i);
        return;
      }
      /* Strict comparison beyond infinity is always false:
         x > +inf, +inf < x, x < -inf, -inf > x */
      if (!ieee_finite(fv))
      {
        int fold = 0;
        if (fv > 0)
        { /* +inf */
          if ((c2 && op == TOK_GT) || (c1 && op == TOK_LT))
            fold = 1;
        }
        else
        { /* -inf */
          if ((c2 && op == TOK_LT) || (c1 && op == TOK_GT))
            fold = 1;
        }
        if (fold)
        {
          vtop -= 2;
          vpushi(0);
          return;
        }
      }
    }
  }

  if (c1 && c2)
  {
    if (bt == VT_FLOAT)
    {
      f1 = v1->c.f;
      f2 = v2->c.f;
    }
    else if (bt == VT_DOUBLE)
    {
      f1 = v1->c.d;
      f2 = v2->c.d;
    }
    else
    {
      f1 = v1->c.ld;
      f2 = v2->c.ld;
    }
    /* NOTE: we only do constant propagation if finite number (not
       NaN or infinity) (ANSI spec).  Comparison operators are safe
       to fold with NaN/Inf since they don't raise FP exceptions. */
    if (!(ieee_finite(f1) || !ieee_finite(f2)) && !CONST_WANTED)
    {
      int is_cmp = (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE);
      if (!is_cmp)
        goto general_case;
    }
    /* Fold VT_DOUBLE operands in double precision (and VT_FLOAT in float),
       not through the shared 80-bit long double f1/f2.  Rounding the
       long-double result down to double at the store below would round a
       SECOND time (double-rounding), so `d / d` folded here could differ by
       1 ULP from both gcc and tcc's own (correct) runtime softfloat routine.
       Computing in the operand's native precision and widening back is exact,
       so the later `v1->c.d = f1` is an identity round-trip.  (fuzz float
       206597/268558) */
    switch (op)
    {
    case '+':
      if (bt == VT_DOUBLE)      { double d = (double)f1 + (double)f2; f1 = d; }
      else if (bt == VT_FLOAT)  { float s = (float)f1 + (float)f2; f1 = s; }
      else f1 += f2;
      break;
    case '-':
      if (bt == VT_DOUBLE)      { double d = (double)f1 - (double)f2; f1 = d; }
      else if (bt == VT_FLOAT)  { float s = (float)f1 - (float)f2; f1 = s; }
      else f1 -= f2;
      break;
    case '*':
      if (bt == VT_DOUBLE)      { double d = (double)f1 * (double)f2; f1 = d; }
      else if (bt == VT_FLOAT)  { float s = (float)f1 * (float)f2; f1 = s; }
      else f1 *= f2;
      break;
    case '/':
      if (f2 == 0.0)
      {
        union
        {
          float f;
          unsigned u;
        } x1, x2, y;
        /* If not in initializer we need to potentially generate
           FP exceptions at runtime, otherwise we want to fold.  */
        if (!CONST_WANTED)
          goto general_case;
        /* the run-time result of 0.0/0.0 on x87, also of other compilers
           when used to compile the f1 /= f2 below, would be -nan */
        x1.f = f1, x2.f = f2;
        if (f1 == 0.0)
          y.u = 0x7fc00000; /* nan */
        else
          y.u = 0x7f800000;                /* infinity */
        y.u |= (x1.u ^ x2.u) & 0x80000000; /* set sign */
        f1 = y.f;
        break;
      }
      if (bt == VT_DOUBLE)      { double d = (double)f1 / (double)f2; f1 = d; }
      else if (bt == VT_FLOAT)  { float s = (float)f1 / (float)f2; f1 = s; }
      else f1 /= f2;
      break;
    case TOK_NEG:
      f1 = -f1;
      goto unary_result;
    case TOK_EQ:
      i = f1 == f2;
    make_int:
      vtop -= 2;
      print_vstack("gen_opif(0)");
      vpushi(i);
      return;
    case TOK_NE:
      i = f1 != f2;
      goto make_int;
    case TOK_LT:
      i = f1 < f2;
      goto make_int;
    case TOK_GE:
      i = f1 >= f2;
      goto make_int;
    case TOK_LE:
      i = f1 <= f2;
      goto make_int;
    case TOK_GT:
      i = f1 > f2;
      goto make_int;
    default:
      goto general_case;
    }
    vtop--;
    print_vstack("gen_opif(1)");
  unary_result:
    /* XXX: overflow test ? */
    if (bt == VT_FLOAT)
    {
      v1->c.f = f1;
    }
    else if (bt == VT_DOUBLE)
    {
      v1->c.d = f1;
    }
    else
    {
      v1->c.ld = f1;
    }
  }
  else
  {
  general_case:
    if (op == TOK_NEG)
    {
      gen_negf(op);
    }
    else
    {
      /* Canonicalize commutative float/double ops to keep the constant as the
       * second helper argument (r2:r3), mirroring gen_opic's integer
       * "put c2 as constant" rule.  The non-constant operand is far more
       * likely to already be live in the return pair r0:r1 (it is typically a
       * previous __aeabi_d* result), so making it the first argument lets it
       * stay in r0:r1 instead of being shuffled into r2:r3 while the constant
       * is loaded into r0:r1.  Saves a 64-bit register move per op in chained
       * soft-float expressions (e.g. pr58574 Horner polynomials). */
      if (c1 && !c2 && (op == '+' || op == '*'))
        vswap();
      // gen_opf(op);
      tcc_ir_gen_f(tcc_state->ir, op);
    }
  }
}
