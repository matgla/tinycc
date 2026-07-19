/*
 * Soft-float Division - Single Precision
 * Implements __aeabi_fdiv for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Divide two single-precision floats */
float __aeabi_fdiv(float a, float b)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a}, ub = {.f = b}, ur;
  uint32_t a_bits = ua.u, b_bits = ub.u;

  int a_sign = float_sign(a_bits);
  int b_sign = float_sign(b_bits);
  int a_exp = float_exp(a_bits);
  int b_exp = float_exp(b_bits);
  uint32_t a_mant = float_mant(a_bits);
  uint32_t b_mant = float_mant(b_bits);

  int result_sign = a_sign ^ b_sign;

  /* Handle NaN */
  if (is_nan_f(a_bits))
  {
    ur.u = a_bits;
    return ur.f;
  }
  if (is_nan_f(b_bits))
  {
    ur.u = b_bits;
    return ur.f;
  }

  /* Handle infinity */
  if (is_inf_f(a_bits))
  {
    if (is_inf_f(b_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (is_inf_f(b_bits))
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  /* Handle zero */
  if (is_zero_f(b_bits))
  {
    if (is_zero_f(a_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (is_zero_f(a_bits))
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  /* Add implicit bit for normals; scale subnormals into normal form and pay
   * for it in the exponent, so the restoring division below always starts from
   * a full-width significand. */
  if (a_exp != 0)
  {
    a_mant |= FLOAT_IMPLICIT_BIT;
  }
  else
  {
    a_exp = 1;
    while (!(a_mant & FLOAT_IMPLICIT_BIT))
    {
      a_mant <<= 1;
      a_exp--;
    }
  }
  if (b_exp != 0)
  {
    b_mant |= FLOAT_IMPLICIT_BIT;
  }
  else
  {
    b_exp = 1;
    while (!(b_mant & FLOAT_IMPLICIT_BIT))
    {
      b_mant <<= 1;
      b_exp--;
    }
  }

  /* Calculate result exponent */
  int result_exp = a_exp - b_exp + FLOAT_EXP_BIAS;

  /* Perform division using restoring division algorithm */
  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient = 0;

  /* Align dividend with divisor */
  if (dividend < divisor)
  {
    dividend <<= 1;
    result_exp--;
  }

  /* Generate 24 significand bits plus SFP_GRS guard bits.  After the
   * alignment above the first iteration always sets a bit, so the quotient
   * lands with its leading bit at 23 + SFP_GRS -- exactly where
   * sfp_round_pack_float() expects it. */
  for (int i = 0; i < 24 + SFP_GRS; i++)
  {
    quotient <<= 1;
    if (dividend >= divisor)
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  /* A non-zero remainder means the quotient is inexact; fold that into the
   * sticky bit.  The old code inspected only a single guard bit and rounded
   * half *up*, which both mis-rounded exact ties (they must go to even) and
   * lost the information needed to tell a tie from just-above-a-tie. */
  quotient |= (dividend != 0);

  ur.u = sfp_round_pack_float(result_sign, result_exp, (uint32_t)quotient);
  return ur.f;
}
