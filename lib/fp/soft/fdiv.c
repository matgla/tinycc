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

  /* Normalize for division */
  int a_shift = clz32(a_mant) - 8; /* Shift to bit 23 */
  int b_shift = clz32(b_mant) - 8;

  /* Use 64-bit for precision */
  uint64_t dividend = (uint64_t)a_mant << 32;
  uint64_t divisor = (uint64_t)b_mant << (32 - 23);

  /* Adjust exponent */
  result_exp += (b_shift - a_shift);

  /* Perform division */
  uint64_t quotient = 0;
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

  /* Normalize quotient */
  while (quotient >= (FLOAT_IMPLICIT_BIT << 1))
  {
    quotient >>= 1;
    result_exp++;
  }
  while (quotient && !(quotient & FLOAT_IMPLICIT_BIT))
  {
    quotient <<= 1;
    result_exp--;
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
