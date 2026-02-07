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

  /* Add implicit bit */
  if (a_exp != 0)
    a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= FLOAT_IMPLICIT_BIT;

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

  /* Generate 25 bits (1 integer + 23 fraction + 1 guard) */
  for (int i = 0; i < 25; i++)
  {
    quotient <<= 1;
    if (dividend >= divisor)
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  /* Round using guard bit - round half up */
  uint32_t guard = quotient & 1;
  quotient >>= 1;
  if (guard && dividend)
    quotient++;

  /* Final normalization - quotient should be in [2^23, 2^24) */
  if (quotient >= (FLOAT_IMPLICIT_BIT << 1))
  {
    quotient >>= 1;
    result_exp++;
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

  uint32_t result_mant = (uint32_t)quotient & FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}
