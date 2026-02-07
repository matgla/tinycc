/*
 * Soft-float Addition - Single Precision
 * Implements __aeabi_fadd and __aeabi_fsub for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Add two single-precision floats in software */
float __aeabi_fadd(float a, float b)
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
    if (is_inf_f(b_bits) && (a_sign != b_sign))
    {
      ur.u = 0x7FC00000U; /* NaN */
      return ur.f;
    }
    ur.u = a_bits;
    return ur.f;
  }
  if (is_inf_f(b_bits))
  {
    ur.u = b_bits;
    return ur.f;
  }

  /* Handle zero */
  if (is_zero_f(a_bits))
  {
    ur.u = b_bits;
    return ur.f;
  }
  if (is_zero_f(b_bits))
  {
    ur.u = a_bits;
    return ur.f;
  }

  /* Add implicit bit for normalized numbers */
  if (a_exp != 0)
    a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= FLOAT_IMPLICIT_BIT;

  /* Align exponents */
  int exp_diff = a_exp - b_exp;
  int result_exp;
  uint32_t result_mant;
  int result_sign;

  if (exp_diff > 0)
  {
    if (exp_diff < 32)
      b_mant >>= exp_diff;
    else
      b_mant = 0;
    result_exp = a_exp;
  }
  else if (exp_diff < 0)
  {
    if (-exp_diff < 32)
      a_mant >>= -exp_diff;
    else
      a_mant = 0;
    result_exp = b_exp;
  }
  else
  {
    result_exp = a_exp;
  }

  /* Add or subtract mantissas */
  if (a_sign == b_sign)
  {
    result_mant = a_mant + b_mant;
    result_sign = a_sign;
    if (result_mant & (FLOAT_IMPLICIT_BIT << 1))
    {
      result_mant >>= 1;
      result_exp++;
    }
  }
  else
  {
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
    if (result_mant == 0)
    {
      ur.u = 0;
      return ur.f;
    }
    while (!(result_mant & FLOAT_IMPLICIT_BIT) && result_exp > 0)
    {
      result_mant <<= 1;
      result_exp--;
    }
  }

  if (result_exp >= 0xFF)
  {
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (result_exp <= 0)
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  result_mant &= FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}

/* Subtract two single-precision floats */
float __aeabi_fsub(float a, float b)
{
  union
  {
    float f;
    uint32_t u;
  } ub = {.f = b};
  ub.u ^= FLOAT_SIGN_BIT;
  return __aeabi_fadd(a, ub.f);
}
