/*
 * Soft-float Addition - Double Precision
 * Implements __aeabi_dadd and __aeabi_dsub for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Add two double-precision floats */
double __aeabi_dadd(double a, double b)
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
    if (is_inf_bits(b_bits) && (a_sign != b_sign))
    {
      /* inf + (-inf) = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = a_bits;
    return ur.d;
  }
  if (is_inf_bits(b_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }

  /* Handle zero */
  if (is_zero_bits(a_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }
  if (is_zero_bits(b_bits))
  {
    ur.u = a_bits;
    return ur.d;
  }

  /* Add implicit bit for normalized numbers */
  if (a_exp != 0)
    a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= DOUBLE_IMPLICIT_BIT;

  /* Align exponents - shift smaller mantissa right */
  int exp_diff = a_exp - b_exp;
  int result_exp;
  uint64_t result_mant;
  int result_sign;

  if (exp_diff > 0)
  {
    /* a has larger exponent */
    if (exp_diff < 64)
      b_mant >>= exp_diff;
    else
      b_mant = 0;
    result_exp = a_exp;
  }
  else if (exp_diff < 0)
  {
    /* b has larger exponent */
    if (-exp_diff < 64)
      a_mant >>= -exp_diff;
    else
      a_mant = 0;
    result_exp = b_exp;
  }
  else
  {
    result_exp = a_exp;
  }

  /* Add or subtract mantissas based on signs */
  if (a_sign == b_sign)
  {
    /* Same sign: add mantissas */
    result_mant = a_mant + b_mant;
    result_sign = a_sign;

    /* Check for overflow (carry) */
    if (result_mant & (DOUBLE_IMPLICIT_BIT << 1))
    {
      result_mant >>= 1;
      result_exp++;
    }
  }
  else
  {
    /* Different signs: subtract mantissas */
    if (a_mant >= b_mant)
    {
      result_mant = a_mant - b_mant;
      result_sign = a_sign;
    }
    else
    {
      result_mant = b_mant - a_mant;
      result_sign = b_sign;
    }

    /* Normalize - shift left until implicit bit is set */
    if (result_mant == 0)
    {
      ur.u = 0;
      return ur.d;
    }
    while (!(result_mant & DOUBLE_IMPLICIT_BIT) && result_exp > 0)
    {
      result_mant <<= 1;
      result_exp--;
    }
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

  /* Remove implicit bit and build result */
  result_mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}

/* Subtract two double-precision floats */
double __aeabi_dsub(double a, double b)
{
  /* Negate b and add */
  union
  {
    double d;
    uint64_t u;
  } ub = {.d = b};
  ub.u ^= DOUBLE_SIGN_BIT; /* Flip sign bit */
  return __aeabi_dadd(a, ub.d);
}
