/*
 * Soft-float Division - Double Precision
 * Implements __aeabi_ddiv for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Divide two double-precision floats */
double __aeabi_ddiv(double a, double b)
{
  union
  {
    double d;
    uint64_t u;
  } ua, ub, ur;
  ua.d = a;
  ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;

  int a_sign = double_sign(a_bits);
  int b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits);
  int b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits);
  uint64_t b_mant = double_mant(b_bits);

  /* Result sign is XOR of input signs */
  int result_sign = a_sign ^ b_sign;

  /* Handle NaN */
  if (is_nan_bits(a_bits))
  {
    ur.u = a_bits;
    return ur.d;
  }
  if (is_nan_bits(b_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }

  /* Handle infinity */
  if (is_inf_bits(a_bits))
  {
    if (is_inf_bits(b_bits))
    {
      /* inf / inf = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    /* inf / x = inf */
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_inf_bits(b_bits))
  {
    /* x / inf = 0 */
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Handle zero */
  if (is_zero_bits(b_bits))
  {
    if (is_zero_bits(a_bits))
    {
      /* 0 / 0 = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    /* x / 0 = inf */
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_zero_bits(a_bits))
  {
    /* 0 / x = 0 */
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Add implicit bit for normals; scale subnormals into normal form and pay
   * for it in the exponent, so the restoring division below always starts from
   * a full-width significand. */
  if (a_exp != 0)
  {
    a_mant |= DOUBLE_IMPLICIT_BIT;
  }
  else
  {
    a_exp = 1;
    while (!(a_mant & DOUBLE_IMPLICIT_BIT))
    {
      a_mant <<= 1;
      a_exp--;
    }
  }
  if (b_exp != 0)
  {
    b_mant |= DOUBLE_IMPLICIT_BIT;
  }
  else
  {
    b_exp = 1;
    while (!(b_mant & DOUBLE_IMPLICIT_BIT))
    {
      b_mant <<= 1;
      b_exp--;
    }
  }

  /* Calculate result exponent: ea - eb + bias */
  int result_exp = a_exp - b_exp + DOUBLE_EXP_BIAS;

  /* Perform division using restoring division algorithm */
  /* We need 53 bits of quotient precision plus guard bits */
  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient = 0;

  /* Both mantissas have implicit bit at position 52, values in [1.0, 2.0) */
  /* The quotient will be in range [0.5, 2.0) */
  /* We want to generate the quotient bit by bit starting from MSB */

  /* If dividend < divisor, the quotient is in [0.5, 1.0) */
  /* Pre-shift dividend to ensure first iteration can produce a quotient bit */
  if (dividend < divisor)
  {
    dividend <<= 1;
    result_exp--;
  }

  /* Generate 53 significand bits plus SFP_GRS guard bits.  After the
   * alignment above the first iteration always sets a bit, so the quotient
   * lands with its leading bit at 52 + SFP_GRS -- exactly where
   * sfp_round_pack_double() expects it.  (dividend stays below 2^54
   * throughout: it is reduced below the divisor before each shift.) */
  for (int i = 0; i < 53 + SFP_GRS; i++)
  {
    quotient <<= 1;
    if (!(dividend < divisor))
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  /* A non-zero remainder means the quotient is inexact; fold that into the
   * sticky bit.  The old code kept a single guard bit and rounded half *up*,
   * so exact ties went the wrong way and subnormal results were flushed. */
  quotient |= (dividend != 0);

  ur.u = sfp_round_pack_double(result_sign, result_exp, quotient);
  return ur.d;
}
