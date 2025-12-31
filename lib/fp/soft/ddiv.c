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
  } ua = {.d = a}, ub = {.d = b}, ur;
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

  /* Add implicit bit for normalized numbers */
  if (a_exp != 0)
    a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= DOUBLE_IMPLICIT_BIT;

  /* Calculate result exponent: ea - eb + bias */
  int result_exp = a_exp - b_exp + DOUBLE_EXP_BIAS;

  /* Perform division using restoring division algorithm */
  /* We need 53 bits of quotient precision */
  /* Shift dividend left to maximize precision */
  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient = 0;

  /* Normalize both to have MSB at bit 63 for maximum precision */
  int a_shift = clz64(dividend);
  int b_shift = clz64(divisor);
  dividend <<= a_shift;
  divisor <<= b_shift;

  /* Adjust exponent for the shift */
  result_exp += (b_shift - a_shift);

  /* If dividend < divisor after normalization, we need to adjust */
  if (dividend < divisor)
  {
    result_exp--;
  }

  /* Perform 53 iterations of division */
  for (int i = 0; i < 54; i++)
  {
    quotient <<= 1;
    if (dividend >= divisor)
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  /* Round: check if remainder >= half divisor */
  if (dividend >= divisor)
  {
    quotient++;
  }

  /* Normalize quotient - should have MSB around bit 53 */
  /* Shift to get 52-bit mantissa */
  while (quotient >= (DOUBLE_IMPLICIT_BIT << 1))
  {
    quotient >>= 1;
    result_exp++;
  }
  while (quotient && !(quotient & DOUBLE_IMPLICIT_BIT))
  {
    quotient <<= 1;
    result_exp--;
  }

  /* Check for overflow to infinity */
  if (result_exp >= 0x7FF)
  {
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }

  /* Check for underflow to zero */
  if (result_exp <= 0)
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Remove implicit bit */
  uint64_t result_mant = quotient & DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}
