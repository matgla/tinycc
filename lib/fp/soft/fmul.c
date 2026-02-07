/*
 * Soft-float Multiplication - Single Precision
 * Implements __aeabi_fmul for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Multiply two single-precision floats */
float __aeabi_fmul(float a, float b)
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
    if (is_zero_f(b_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (is_inf_f(b_bits))
  {
    if (is_zero_f(a_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }

  /* Handle zero */
  if (is_zero_f(a_bits) || is_zero_f(b_bits))
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
  int result_exp = a_exp + b_exp - FLOAT_EXP_BIAS;

  /* Multiply mantissas (24-bit * 24-bit = 48-bit) */
  uint64_t product = (uint64_t)a_mant * (uint64_t)b_mant;

  /* Normalize: product is in bits 46-0, implicit bit at 46 or 47 */
  if (product & (1ULL << 47))
  {
    product >>= 1;
    result_exp++;
  }

  /* Shift to get 23-bit mantissa */
  uint32_t result_mant = (uint32_t)(product >> 23);

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
