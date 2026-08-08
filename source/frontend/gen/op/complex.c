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

/* complex.c -- _Complex arithmetic, comparison and conjugation lowering.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Decompose complex integer == / != into component-wise comparisons.
 *
 * Stack on entry:  [... lhs rhs]   (both have VT_COMPLEX set, integer base types)
 * Stack on exit:   [... result]    (int 0 or 1)
 *
 * For !=: (__real__ a != __real__ b) || (__imag__ a != __imag__ b)
 * For ==: (__real__ a == __real__ b) && (__imag__ a == __imag__ b)
 *
 * We avoid the usual arithmetic promotion to _Complex int because the runtime
 * cast from _Complex char/short to _Complex int is not implemented (it would
 * need to unpack/repack the components).  Instead we compare each component
 * individually, promoting component types via the normal integer rules.
 */
void gen_complex_int_cmp(int op)
{
  int lbt = vtop[-1].type.t & VT_BTYPE;
  int rbt = vtop[0].type.t & VT_BTYPE;
  int l_elem = btype_size(lbt);
  int r_elem = btype_size(rbt);

  int l_const = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int r_const = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

  /* Extract constant component values via the packed representation:
   * _Complex char packs as (imag << 8 | real) in 16 bits,
   * _Complex int  packs as (imag << 32 | real) in 64 bits, etc. */
  SValue saved_rhs;
  uint64_t l_real_c = 0, l_imag_c = 0, r_real_c = 0, r_imag_c = 0;

  if (r_const)
  {
    int shift = r_elem * 8;
    uint64_t mask = (shift >= 64) ? ~0ULL : (1ULL << shift) - 1;
    r_real_c = vtop[0].c.i & mask;
    r_imag_c = (shift >= 64) ? 0 : ((vtop[0].c.i >> shift) & mask);
  }
  if (l_const)
  {
    int shift = l_elem * 8;
    uint64_t mask = (shift >= 64) ? ~0ULL : (1ULL << shift) - 1;
    l_real_c = vtop[-1].c.i & mask;
    l_imag_c = (shift >= 64) ? 0 : ((vtop[-1].c.i >> shift) & mask);
  }

  /* Save SValues so we can push them again after popping.
   * For lvalues this is safe because they reference memory, not registers. */
  saved_rhs = *vtop;

  /* Pop rhs */
  vpop();
  /* Stack: [... lhs] */

  /* --- Compare real parts --- */
  /* Push real(lhs) */
  if (l_const)
  {
    vpop(); /* remove lhs */
    vpush64(VT_INT, l_real_c);
  }
  else
  {
    vdup(); /* [... lhs lhs_copy] */
    /* Change copy to base scalar type (strips VT_COMPLEX, keeps lvalue) */
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | lbt;
  }

  /* Push real(rhs) */
  if (r_const)
  {
    vpush64(VT_INT, r_real_c);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | rbt;
  }

  /* Compare real parts (normal integer promotion handles char→int etc.) */
  gen_op(op);

  /* Stack: [... lhs result_real]  (if lhs non-const)
   *    or: [... result_real]      (if lhs const) */

  if (!l_const)
    vswap(); /* [... result_real lhs] */

  /* --- Compare imaginary parts --- */
  /* Push imag(lhs) */
  if (l_const)
  {
    vpush64(VT_INT, l_imag_c);
  }
  else
  {
    /* The original lhs is still on the vstack as an lvalue.
     * Strip VT_COMPLEX so the load uses the base type size,
     * then use incr_offset to advance to the imaginary component.
     * incr_offset takes the address, adds the offset, and re-marks
     * the result as an lvalue — this properly generates an ADD in the IR. */
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | lbt;
    if (l_elem > 0)
      incr_offset(l_elem);
  }

  /* Push imag(rhs) */
  if (r_const)
  {
    vpush64(VT_INT, r_imag_c);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | rbt;
    if (r_elem > 0)
      incr_offset(r_elem);
  }

  /* Compare imaginary parts */
  gen_op(op);

  /* Stack: [... result_real result_imag] */

  /* Combine: && for EQ  (both must match),  || for NE  (either differs) */
  gen_op(op == TOK_EQ ? '&' : '|');
}

/* Decompose complex floating-point == / != into component-wise comparisons.
 *
 * Stack on entry:  [... lhs rhs]   (both have VT_COMPLEX set, float base types)
 * Stack on exit:   [... result]    (int 0 or 1)
 *
 * For !=: (__real__ a != __real__ b) || (__imag__ a != __imag__ b)
 * For ==: (__real__ a == __real__ b) && (__imag__ a == __imag__ b)
 *
 * Complex float: real at offset 0 (4B), imag at offset 4 (4B).
 * Complex double: real at offset 0 (8B), imag at offset 8 (8B).
 */
void gen_complex_float_cmp(int op)
{
  int l_bt = vtop[-1].type.t & VT_BTYPE;
  int r_bt = vtop[0].type.t & VT_BTYPE;
  int l_elem_size = (l_bt == VT_DOUBLE || l_bt == VT_LDOUBLE) ? 8 : 4;
  int r_elem_size = (r_bt == VT_DOUBLE || r_bt == VT_LDOUBLE) ? 8 : 4;

  int l_const = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int r_const = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

  /* Extract constant float/double component values.
   * Use each operand's OWN base type for extraction. */
  CValue l_real_cv, l_imag_cv, r_real_cv, r_imag_cv;
  int l_push_bt = l_bt, r_push_bt = r_bt;
  memset(&l_real_cv, 0, sizeof(CValue));
  memset(&l_imag_cv, 0, sizeof(CValue));
  memset(&r_real_cv, 0, sizeof(CValue));
  memset(&r_imag_cv, 0, sizeof(CValue));

  if (r_const)
  {
    if (!is_float(r_bt))
    {
      /* Integer promoted to complex: real = cast to float/double, imag = 0. */
      r_push_bt = VT_DOUBLE;
      r_real_cv.d = (double)vtop[0].c.i;
    }
    else if (r_bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(vtop[0].c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(vtop[0].c.i >> 32);
      r_real_cv.f = a.f;
      r_imag_cv.f = b.f;
    }
    else
    {
      memcpy(&r_real_cv.d, &vtop[0].c, 8);
      memcpy(&r_imag_cv.d, (char *)&vtop[0].c + 8, 8);
    }
  }
  if (l_const)
  {
    if (!is_float(l_bt))
    {
      l_push_bt = VT_DOUBLE;
      l_real_cv.d = (double)vtop[-1].c.i;
    }
    else if (l_bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(vtop[-1].c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(vtop[-1].c.i >> 32);
      l_real_cv.f = a.f;
      l_imag_cv.f = b.f;
    }
    else
    {
      memcpy(&l_real_cv.d, &vtop[-1].c, 8);
      memcpy(&l_imag_cv.d, (char *)&vtop[-1].c + 8, 8);
    }
  }

  SValue saved_rhs = *vtop;

  /* Pop rhs */
  vpop();
  /* Stack: [... lhs] */

  /* --- Compare real parts --- */
  if (l_const)
  {
    vpop(); /* remove lhs */
    CType ctype = {0};
    ctype.t = l_push_bt;
    vsetc(&ctype, VT_CONST, &l_real_cv);
  }
  else
  {
    vdup(); /* [... lhs lhs_copy] */
    vtop->type.t &= ~VT_COMPLEX;
  }

  if (r_const)
  {
    CType ctype = {0};
    ctype.t = r_push_bt;
    vsetc(&ctype, VT_CONST, &r_real_cv);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t &= ~VT_COMPLEX;
  }

  /* Compare real parts (scalar comparison — gen_op handles type promotion) */
  gen_op(op);

  if (!l_const)
    vswap();

  /* --- Compare imaginary parts --- */
  if (l_const)
  {
    CType ctype = {0};
    ctype.t = l_push_bt;
    vsetc(&ctype, VT_CONST, &l_imag_cv);
  }
  else
  {
    vtop->type.t &= ~VT_COMPLEX;
    incr_offset(l_elem_size);
  }

  if (r_const)
  {
    CType ctype = {0};
    ctype.t = r_push_bt;
    vsetc(&ctype, VT_CONST, &r_imag_cv);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t &= ~VT_COMPLEX;
    incr_offset(r_elem_size);
  }

  /* Compare imaginary parts */
  gen_op(op);

  /* Combine: && for EQ  (both must match),  || for NE  (either differs) */
  gen_op(op == TOK_EQ ? '&' : '|');
}

/* Decompose complex integer +, -, *, / into component-wise scalar operations.
 *
 * Stack on entry:  [... lhs rhs]   (both have VT_COMPLEX set, integer base types)
 * Stack on exit:   [... result]    (complex integer lvalue in temp local)
 *
 * For +: result.real = a.real + b.real, result.imag = a.imag + b.imag
 * For -: result.real = a.real - b.real, result.imag = a.imag - b.imag
 * For *: (a+bi)(c+di) = (ac-bd) + (ad+bc)i
 * For /: (a+bi)/(c+di) = ((ac+bd) + (bc-ad)i) / (cc+dd)
 *
 * Complex int:   real at offset +0, imag at offset +elem_size.
 * Constant complex ints are packed into 64 bits: real in low, imag in high.
 *
 * Before decomposition, both operands must be promoted to the same base type
 * via the usual arithmetic conversions so that elem_size is consistent.
 */
void gen_complex_int_arith(int op)
{
  int t1 = vtop[-1].type.t;
  int t2 = vtop[0].type.t;
  int was_complex_lhs = (t1 & VT_COMPLEX) != 0;
  int was_complex_rhs = (t2 & VT_COMPLEX) != 0;

  /* Determine promoted base type via usual arithmetic conversions.
   * Both operands should already have the same type from gen_cast_s
   * in the caller, but handle any remaining differences. */
  int bt1 = t1 & VT_BTYPE;
  int bt2 = t2 & VT_BTYPE;
  int bt;
  if (bt1 == VT_LLONG || bt2 == VT_LLONG)
    bt = VT_LLONG;
  else
    bt = VT_INT; /* C integer promotion: at least int */
  int elem_size = btype_size(bt);
  int complex_size = elem_size * 2;
  int is_unsigned = (t1 | t2) & VT_UNSIGNED;

  /* Element type: promoted scalar type (no VT_COMPLEX). */
  CType elem_type;
  elem_type.t = bt | (is_unsigned ? VT_UNSIGNED : 0);
  elem_type.ref = NULL;

  /* Cast both operands to the promoted type (strip VT_COMPLEX for cast,
   * but retain the complex flag on the SValue for component extraction). */
  vswap();
  if ((vtop->type.t & VT_BTYPE) != bt)
    gen_cast_s(elem_type.t);
  vtop->type.t |= VT_COMPLEX;
  vswap();
  if ((vtop->type.t & VT_BTYPE) != bt)
    gen_cast_s(elem_type.t);
  vtop->type.t |= VT_COMPLEX;

  int l_const = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int r_const = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

  SValue saved_lhs = vtop[-1];
  SValue saved_rhs = vtop[0];
  vpop();
  vpop();

  /* Allocate temp local for result. */
  int res_vr;
  int res_loc = get_temp_local_var(complex_size, elem_size, &res_vr);

  /* ---- Helper macros to push/store components ---- */
#define PUSH_COMP(sv, is_const, was_cplx, comp)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(was_cplx))                                                                                                   \
    {                                                                                                                  \
      if ((comp) == 0)                                                                                                 \
      {                                                                                                                \
        vpushv(&(sv));                                                                                                 \
        vtop->type.t &= ~VT_COMPLEX;                                                                                   \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        vpushi(0);                                                                                                     \
        vtop->type = elem_type;                                                                                        \
      }                                                                                                                \
    }                                                                                                                  \
    else if (is_const)                                                                                                 \
    {                                                                                                                  \
      int shift_ = elem_size * 8;                                                                                      \
      uint64_t mask_ = (elem_size == 8) ? ~0ULL : ((1ULL << shift_) - 1);                                              \
      uint64_t val_ = (sv).c.i;                                                                                        \
      vpushi(0);                                                                                                       \
      vtop->c.i = (int64_t)(((comp) == 0) ? (val_ & mask_) : ((val_ >> shift_) & mask_));                              \
      vtop->type = elem_type;                                                                                          \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
      vpushv(&(sv));                                                                                                   \
      vtop->type.t &= ~VT_COMPLEX;                                                                                     \
      if ((comp) == 1)                                                                                                 \
        incr_offset(elem_size);                                                                                        \
    }                                                                                                                  \
  } while (0)

#define STORE_COMP(comp)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    SValue dst_;                                                                                                       \
    memset(&dst_, 0, sizeof(dst_));                                                                                    \
    dst_.type = elem_type;                                                                                             \
    dst_.r = VT_LOCAL | VT_LVAL;                                                                                       \
    dst_.vr = res_vr;                                                                                                  \
    dst_.c.i = res_loc + (comp) * elem_size;                                                                           \
    vpushv(&dst_);                                                                                                     \
    vswap();                                                                                                           \
    vstore();                                                                                                          \
    vpop();                                                                                                            \
  } while (0)

  switch (op)
  {
  case '+':
  case '-':
    /* real = a.real op b.real */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op(op);
    STORE_COMP(0);
    /* imag = a.imag op b.imag */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op(op);
    STORE_COMP(1);
    break;

  case '*':
    /* real = a.real * b.real - a.imag * b.imag */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    gen_op('-');
    STORE_COMP(0);
    /* imag = a.real * b.imag + a.imag * b.real */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    gen_op('+');
    STORE_COMP(1);
    break;

  case '/':
  {
    /* Compute denom = c.real^2 + c.imag^2 inline for each component
     * to avoid temp variable reuse issues. */

    /* real = (a.real * c.real + a.imag * c.imag) / (c.real^2 + c.imag^2) */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    gen_op('+');
    /* denom */
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    gen_op('+');
    gen_op('/');
    STORE_COMP(0);

    /* imag = (a.imag * c.real - a.real * c.imag) / (c.real^2 + c.imag^2) */
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    PUSH_COMP(saved_lhs, l_const, was_complex_lhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    gen_op('-');
    /* denom again */
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 0);
    gen_op('*');
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    PUSH_COMP(saved_rhs, r_const, was_complex_rhs, 1);
    gen_op('*');
    gen_op('+');
    gen_op('/');
    STORE_COMP(1);
    break;
  }
  default:
    tcc_error("unsupported complex integer operation");
  }

#undef PUSH_COMP
#undef STORE_COMP

  /* Push result as complex lvalue. */
  {
    SValue result;
    memset(&result, 0, sizeof(result));
    result.type.t = bt | VT_COMPLEX | (is_unsigned ? VT_UNSIGNED : 0);
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = res_vr;
    result.c.i = res_loc;
    vpushv(&result);
  }
}

/* Decompose complex floating-point +/- into component-wise operations.
 *
 * Stack on entry:  [... lhs rhs]   (both have VT_COMPLEX set, float base types)
 * Stack on exit:   [... result]    (complex float/double lvalue in temp local)
 *
 * For +: result.real = lhs.real + rhs.real, result.imag = lhs.imag + rhs.imag
 * For -: result.real = lhs.real - rhs.real, result.imag = lhs.imag - rhs.imag
 *
 * Complex float:  real at offset +0 (4 B), imag at offset +4 (4 B).
 * Complex double: real at offset +0 (8 B), imag at offset +8 (8 B).
 *
 * This decomposition is necessary because complex double (128 bits) does not
 * fit in a register pair (64 bits max), so the IR/register allocator cannot
 * handle it as a single value.  Complex float also uses this path for
 * consistency.
 */

/* Generate complex conjugate: negate the imaginary part.
 * Works for both float and integer complex types.
 * Expects vtop to hold a complex value. */
void gen_complex_conjugate(void)
{
  int base_type = vtop->type.t & VT_BTYPE;
  int is_int_complex = !is_float(base_type);
  int elem_size;

  if (is_int_complex)
    elem_size = btype_size(base_type);
  else if (base_type == VT_DOUBLE || base_type == VT_LDOUBLE)
    elem_size = 8;
  else
    elem_size = 4; /* float */

  /* Constant-folding fast path: if both parts are known at compile time,
   * produce the conjugate as a new constant without emitting any code. */
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    if (is_int_complex)
    {
      int shift = elem_size * 8;
      uint64_t mask = (shift >= 64) ? ~0ULL : (1ULL << shift) - 1;
      int64_t real_part = (int64_t)(vtop->c.i & mask);
      int64_t imag_part = (shift >= 64) ? 0 : (int64_t)((vtop->c.i >> shift) & mask);
      imag_part = -imag_part;
      vtop->c.i = (uint64_t)(real_part & mask) | ((uint64_t)(imag_part & mask) << shift);
      return;
    }
    if (base_type == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } r, im;
      r.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
      im.u = (uint32_t)(vtop->c.i >> 32);
      im.f = -im.f;
      vtop->c.i = (uint64_t)r.u | ((uint64_t)im.u << 32);
      return;
    }
    /* double / ldouble complex */
    {
      double src_real, src_imag;
      memcpy(&src_real, &vtop->c, 8);
      memcpy(&src_imag, (char *)&vtop->c + 8, 8);
      src_imag = -src_imag;
      memcpy(&vtop->c, &src_real, 8);
      memcpy((char *)&vtop->c + 8, &src_imag, 8);
      return;
    }
  }

  int result_size = elem_size * 2;
  int res_vr;
  int res_loc = get_temp_local_var(result_size, result_size > 8 ? 8 : result_size, &res_vr);

  /* Element type: strip VT_COMPLEX from the type */
  CType elem_type;
  elem_type = vtop->type;
  elem_type.t &= ~VT_COMPLEX;
  if (!is_int_complex)
  {
    if (elem_size == 4)
      elem_type.t = (elem_type.t & ~VT_BTYPE) | VT_FLOAT;
    else
      elem_type.t = (elem_type.t & ~VT_BTYPE) | VT_DOUBLE;
  }

  /* If the value is not already a local or lvalue (e.g. VT_CONST),
   * materialize it to a temp local so extraction below works uniformly. */
  if ((vtop->r & VT_VALMASK) != VT_LOCAL && !(vtop->r & VT_LVAL))
  {
    int mat_vr;
    int mat_loc = get_temp_local_var(result_size, result_size > 8 ? 8 : result_size, &mat_vr);

    CType orig_ctype = vtop->type;
    int is_const = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

    if (is_const && is_float(base_type))
    {
      /* Constant complex float/double: unpack and store each component */
      double src_real = 0.0, src_imag = 0.0;
      if (base_type == VT_FLOAT)
      {
        union
        {
          float f;
          uint32_t u;
        } r, im;
        r.u = (uint32_t)(vtop->c.i & 0xFFFFFFFF);
        im.u = (uint32_t)(vtop->c.i >> 32);
        src_real = r.f;
        src_imag = im.f;
      }
      else
      {
        memcpy(&src_real, &vtop->c, 8);
        memcpy(&src_imag, (char *)&vtop->c + 8, 8);
      }
      vpop();

      /* Store real part */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = elem_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = mat_vr;
        dst.c.i = mat_loc;
        vpushv(&dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        if (base_type == VT_FLOAT)
          cv.f = (float)src_real;
        else
          cv.d = src_real;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }
      /* Store imag part */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = elem_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = mat_vr;
        dst.c.i = mat_loc + elem_size;
        vpushv(&dst);
        CValue cv;
        memset(&cv, 0, sizeof(cv));
        if (base_type == VT_FLOAT)
          cv.f = (float)src_imag;
        else
          cv.d = src_imag;
        vsetc(&elem_type, VT_CONST, &cv);
        vstore();
        vpop();
      }
    }
    else if (is_const && !is_float(base_type))
    {
      /* Constant complex integer: unpack and store each component */
      int shift = elem_size * 8;
      uint64_t packed = vtop->c.i;
      uint64_t mask = (base_type == VT_LLONG) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << shift) - 1);
      int64_t src_real = (int64_t)(packed & mask);
      int64_t src_imag = (int64_t)((packed >> shift) & mask);
      vpop();

      /* Store real part */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = elem_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = mat_vr;
        dst.c.i = mat_loc;
        vpushv(&dst);
        vpushi(src_real);
        if (elem_size > 4)
          vtop->type.t = VT_LLONG;
        vstore();
        vpop();
      }
      /* Store imag part */
      {
        SValue dst;
        memset(&dst, 0, sizeof(dst));
        dst.type = elem_type;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = mat_vr;
        dst.c.i = mat_loc + elem_size;
        vpushv(&dst);
        vpushi(src_imag);
        if (elem_size > 4)
          vtop->type.t = VT_LLONG;
        vstore();
        vpop();
      }
    }
    else
    {
      /* Register or other non-const complex: store via temp */
      SValue mat_sv;
      memset(&mat_sv, 0, sizeof(mat_sv));
      mat_sv.type = orig_ctype;
      mat_sv.r = VT_LOCAL | VT_LVAL;
      mat_sv.vr = mat_vr;
      mat_sv.c.i = mat_loc;
      vpushv(&mat_sv);
      vswap();
      vstore();
      vpop();
    }

    /* Replace vtop with the materialized local */
    SValue mat_sv;
    memset(&mat_sv, 0, sizeof(mat_sv));
    mat_sv.type = orig_ctype;
    mat_sv.r = VT_LOCAL | VT_LVAL;
    mat_sv.vr = mat_vr;
    mat_sv.c.i = mat_loc;
    vpushv(&mat_sv);
  }

  /* Save the original complex value (now guaranteed VT_LOCAL or VT_LVAL) */
  SValue orig_val = *vtop;
  vpop();

  /* Extract real part */
  vpushv(&orig_val);
  if ((orig_val.r & VT_VALMASK) == VT_LOCAL)
  {
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | (elem_type.t & VT_BTYPE);
  }
  else if (orig_val.r & VT_LVAL)
  {
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | (elem_type.t & VT_BTYPE) | VT_LVAL;
    indir();
  }

  /* Store real part to result[0] */
  SValue res_addr;
  memset(&res_addr, 0, sizeof(res_addr));
  res_addr.type = elem_type;
  res_addr.r = VT_LOCAL | VT_LVAL;
  res_addr.vr = res_vr;
  res_addr.c.i = res_loc;

  vpushv(&res_addr);
  vswap();
  vstore();
  vpop();

  /* Extract imaginary part */
  vpushv(&orig_val);
  if ((orig_val.r & VT_VALMASK) == VT_LOCAL)
  {
    vtop->c.i += elem_size;
    vtop->type.t = (vtop->type.t & ~VT_BTYPE & ~VT_COMPLEX) | (elem_type.t & VT_BTYPE);
  }
  else if (orig_val.r & VT_LVAL)
  {
    vpushi(elem_size);
    gen_op('+');
    vtop->type.t = (orig_val.type.t & ~VT_BTYPE & ~VT_COMPLEX) | (elem_type.t & VT_BTYPE) | VT_LVAL;
    indir();
  }

  /* Negate the imaginary part */
  if (is_int_complex)
  {
    vpushi(0);
    vswap();
    gen_op('-');
  }
  else
  {
    gen_opif(TOK_NEG);
  }

  /* Store negated imaginary part */
  res_addr.c.i = res_loc + elem_size;
  vpushv(&res_addr);
  vswap();
  vstore();
  vpop();

  /* Push result as complex type */
  memset(&res_addr, 0, sizeof(res_addr));
  res_addr.type = orig_val.type;
  res_addr.r = VT_LOCAL | VT_LVAL;
  res_addr.vr = res_vr;
  res_addr.c.i = res_loc;
  vpushv(&res_addr);
}

void gen_complex_float_arith(int op)
{
  /* Either side may be a plain scalar (mixed scalar+complex arithmetic);
   * take the element type from the complex operand. */
  int l_complex = (vtop[-1].type.t & VT_COMPLEX) != 0;
  int r_complex = (vtop[0].type.t & VT_COMPLEX) != 0;
  int bt = (l_complex ? vtop[-1].type.t : vtop[0].type.t) & VT_BTYPE;
  int elem_size = (bt == VT_DOUBLE || bt == VT_LDOUBLE) ? 8 : 4;
  int complex_size = elem_size * 2;

  int l_const = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
  int r_const = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

  /* Extract constant float/double component values. */
  CValue l_real_cv, l_imag_cv, r_real_cv, r_imag_cv;
  memset(&l_real_cv, 0, sizeof(CValue));
  memset(&l_imag_cv, 0, sizeof(CValue));
  memset(&r_real_cv, 0, sizeof(CValue));
  memset(&r_imag_cv, 0, sizeof(CValue));

  if (r_const)
  {
    int r_bt = vtop[0].type.t & VT_BTYPE;
    if (!is_float(r_bt))
    {
      if (bt == VT_FLOAT)
        r_real_cv.f = (float)vtop[0].c.i;
      else
        r_real_cv.d = (double)vtop[0].c.i;
    }
    else if (bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(vtop[0].c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(vtop[0].c.i >> 32);
      r_real_cv.f = a.f;
      r_imag_cv.f = b.f;
    }
    else
    {
      memcpy(&r_real_cv.d, &vtop[0].c, 8);
      memcpy(&r_imag_cv.d, (char *)&vtop[0].c + 8, 8);
    }
  }
  if (l_const)
  {
    int l_bt = vtop[-1].type.t & VT_BTYPE;
    if (!is_float(l_bt))
    {
      if (bt == VT_FLOAT)
        l_real_cv.f = (float)vtop[-1].c.i;
      else
        l_real_cv.d = (double)vtop[-1].c.i;
    }
    else if (bt == VT_FLOAT)
    {
      union
      {
        float f;
        uint32_t u;
      } a, b;
      a.u = (uint32_t)(vtop[-1].c.i & 0xFFFFFFFF);
      b.u = (uint32_t)(vtop[-1].c.i >> 32);
      l_real_cv.f = a.f;
      l_imag_cv.f = b.f;
    }
    else
    {
      memcpy(&l_real_cv.d, &vtop[-1].c, 8);
      memcpy(&l_imag_cv.d, (char *)&vtop[-1].c + 8, 8);
    }
  }
  SValue saved_lhs = vtop[-1];
  SValue saved_rhs = vtop[0];
  vpop();
  vpop();

  /* Allocate a temp local for the complex result. */
  int res_vr;
  int res_loc = get_temp_local_var(complex_size, elem_size > 8 ? 8 : elem_size, &res_vr);

  /* --- Compute real parts --- */
  if (l_const)
  {
    CType ct = {0};
    ct.t = bt;
    vsetc(&ct, VT_CONST, &l_real_cv);
  }
  else
  {
    vpushv(&saved_lhs);
    vtop->type.t &= ~VT_COMPLEX;
  }
  if (r_const)
  {
    CType ct = {0};
    ct.t = bt;
    vsetc(&ct, VT_CONST, &r_real_cv);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t &= ~VT_COMPLEX;
  }
  /* result.real = lhs.real op rhs.real (scalar float/double) */
  gen_op(op);
  /* Store to result temp at offset 0 (real part) */
  {
    SValue dst;
    memset(&dst, 0, sizeof(dst));
    dst.type.t = bt;
    dst.r = VT_LOCAL | VT_LVAL;
    dst.vr = res_vr;
    dst.c.i = res_loc;
    vpushv(&dst);
    vswap();
    vstore();
    vpop();
  }

  /* --- Compute imaginary parts ---
   * A runtime scalar operand has no imaginary half in memory — its imag is
   * the constant 0 (l_imag_cv/r_imag_cv are already zeroed). */
  if (l_const || !l_complex)
  {
    CType ct = {0};
    ct.t = bt;
    vsetc(&ct, VT_CONST, &l_imag_cv);
  }
  else
  {
    vpushv(&saved_lhs);
    vtop->type.t &= ~VT_COMPLEX;
    incr_offset(elem_size);
  }
  if (r_const || !r_complex)
  {
    CType ct = {0};
    ct.t = bt;
    vsetc(&ct, VT_CONST, &r_imag_cv);
  }
  else
  {
    vpushv(&saved_rhs);
    vtop->type.t &= ~VT_COMPLEX;
    incr_offset(elem_size);
  }
  /* result.imag = lhs.imag op rhs.imag (scalar float/double) */
  gen_op(op);
  /* Store to result temp at offset +elem_size (imag part) */
  {
    SValue dst;
    memset(&dst, 0, sizeof(dst));
    dst.type.t = bt;
    dst.r = VT_LOCAL | VT_LVAL;
    dst.vr = res_vr;
    dst.c.i = res_loc + elem_size;
    vpushv(&dst);
    vswap();
    vstore();
    vpop();
  }

  /* Push result as complex lvalue. */
  {
    SValue result;
    memset(&result, 0, sizeof(result));
    result.type.t = bt | VT_COMPLEX;
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = res_vr;
    result.c.i = res_loc;
    vpushv(&result);
  }
}

/* Decompose complex float `*` into component-wise scalar operations.
 *
 * Stack on entry:  [... lhs rhs]   at least one operand has VT_COMPLEX;
 *                                  base types are float/double (already promoted).
 * Stack on exit:   [... result]    complex lvalue in a temp local.
 *
 * Emits only the muls/adds/subs that are mathematically needed (scalar × complex
 * uses 2 muls, complex × complex uses 4 muls + add + sub).  Going through gen_op()
 * means the resulting scalar ops feed the regular IR codegen, so downstream
 * optimizer passes don't have to know that the underlying memory has a complex
 * memory layout — and we no longer rely on the backend's complex-aware MOP path.
 *
 * Division is intentionally NOT handled here: it falls through to the existing
 * __divsc3 / __divdc3 helpers, which use IEEE-compliant scaling for extreme
 * values that the naïve (c²+d²) formula would over/underflow on.
 */
void gen_complex_float_mul(int op)
{
  (void)op; /* dispatch only invokes this with '*' */
  int lhs_complex = (vtop[-1].type.t & VT_COMPLEX) != 0;
  int rhs_complex = (vtop[0].type.t & VT_COMPLEX) != 0;
  /* Use the complex operand's base type to determine result element size.
   * Both operands have already been promoted to the same fp width via
   * combine_types + gen_cast_s before reaching here. */
  int bt = (lhs_complex ? vtop[-1].type.t : vtop[0].type.t) & VT_BTYPE;
  int elem_size = (bt == VT_DOUBLE || bt == VT_LDOUBLE) ? 8 : 4;
  int complex_size = elem_size * 2;

  CType scalar_type;
  scalar_type.t = bt;
  scalar_type.ref = NULL;

  SValue saved_lhs = vtop[-1];
  SValue saved_rhs = vtop[0];
  vpop();
  vpop();

  /* Allocate temp local for the complex result. */
  int res_vr;
  int res_loc = get_temp_local_var(complex_size, elem_size, &res_vr);
  (void)res_vr;

/* Push the real (comp=0) or imag (comp=1) component of sv onto the vstack.
 * If sv is scalar (was_cplx=0), real is the scalar itself; imag is 0.0.
 * A VT_CONST complex carries both components packed in its CValue (floats
 * in the lo/hi words of c.i, doubles at byte offsets 0 and 8) — extract
 * from there; incr_offset() is only meaningful for lvalues and previously
 * turned `s * (1.0+1.0i)` into `s * 1.0` (constant's imag was lost). */
#define PUSH_FCOMP(sv, was_cplx, comp)                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(was_cplx))                                                                                                   \
    {                                                                                                                  \
      if ((comp) == 0)                                                                                                 \
      {                                                                                                                \
        vpushv(&(sv));                                                                                                 \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        CValue _z;                                                                                                     \
        memset(&_z, 0, sizeof(_z));                                                                                    \
        if (bt == VT_FLOAT)                                                                                            \
          _z.f = 0.0f;                                                                                                 \
        else if (bt == VT_DOUBLE)                                                                                      \
          _z.d = 0.0;                                                                                                  \
        else                                                                                                           \
          _z.ld = 0.0;                                                                                                 \
        vsetc(&scalar_type, VT_CONST, &_z);                                                                            \
      }                                                                                                                \
    }                                                                                                                  \
    else if (((sv).r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)                                                   \
    {                                                                                                                  \
      CValue _c;                                                                                                       \
      memset(&_c, 0, sizeof(_c));                                                                                      \
      if (bt == VT_FLOAT)                                                                                              \
      {                                                                                                                \
        union { float f; uint32_t u; } _x;                                                                             \
        _x.u = (comp) == 0 ? (uint32_t)((sv).c.i & 0xFFFFFFFF) : (uint32_t)((sv).c.i >> 32);                           \
        _c.f = _x.f;                                                                                                   \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        memcpy(&_c.d, (char *)&(sv).c + ((comp) ? 8 : 0), 8);                                                          \
      }                                                                                                                \
      vsetc(&scalar_type, VT_CONST, &_c);                                                                              \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
      vpushv(&(sv));                                                                                                   \
      vtop->type.t &= ~VT_COMPLEX;                                                                                     \
      if ((comp) == 1)                                                                                                 \
        incr_offset(elem_size);                                                                                        \
    }                                                                                                                  \
  } while (0)

#define STORE_FCOMP(comp)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    SValue _d;                                                                                                         \
    memset(&_d, 0, sizeof(_d));                                                                                        \
    _d.type = scalar_type;                                                                                             \
    _d.r = VT_LOCAL | VT_LVAL;                                                                                         \
    _d.vr = -1;                                                                                                        \
    _d.c.i = res_loc + (comp) * elem_size;                                                                             \
    vpushv(&_d);                                                                                                       \
    vswap();                                                                                                           \
    vstore();                                                                                                          \
    vpop();                                                                                                            \
  } while (0)

  if (!lhs_complex)
  {
    /* scalar × complex: a * (c + d*i) = (a*c) + (a*d)*i */
    PUSH_FCOMP(saved_lhs, 0, 0);
    PUSH_FCOMP(saved_rhs, 1, 0);
    gen_op('*');
    STORE_FCOMP(0);

    PUSH_FCOMP(saved_lhs, 0, 0);
    PUSH_FCOMP(saved_rhs, 1, 1);
    gen_op('*');
    STORE_FCOMP(1);
  }
  else if (!rhs_complex)
  {
    /* complex × scalar: (a+b*i) * c = (a*c) + (b*c)*i */
    PUSH_FCOMP(saved_lhs, 1, 0);
    PUSH_FCOMP(saved_rhs, 0, 0);
    gen_op('*');
    STORE_FCOMP(0);

    PUSH_FCOMP(saved_lhs, 1, 1);
    PUSH_FCOMP(saved_rhs, 0, 0);
    gen_op('*');
    STORE_FCOMP(1);
  }
  else
  {
    /* complex × complex: (a+b*i)(c+d*i) = (a*c - b*d) + (a*d + b*c)*i */
    PUSH_FCOMP(saved_lhs, 1, 0);
    PUSH_FCOMP(saved_rhs, 1, 0);
    gen_op('*');
    PUSH_FCOMP(saved_lhs, 1, 1);
    PUSH_FCOMP(saved_rhs, 1, 1);
    gen_op('*');
    gen_op('-');
    STORE_FCOMP(0);

    PUSH_FCOMP(saved_lhs, 1, 0);
    PUSH_FCOMP(saved_rhs, 1, 1);
    gen_op('*');
    PUSH_FCOMP(saved_lhs, 1, 1);
    PUSH_FCOMP(saved_rhs, 1, 0);
    gen_op('*');
    gen_op('+');
    STORE_FCOMP(1);
  }

#undef PUSH_FCOMP
#undef STORE_FCOMP

  /* Push result as complex lvalue. */
  {
    SValue result;
    memset(&result, 0, sizeof(result));
    result.type.t = bt | VT_COMPLEX;
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = -1;
    result.c.i = res_loc;
    vpushv(&result);
  }
}
