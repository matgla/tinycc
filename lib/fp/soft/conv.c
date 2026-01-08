/*
 * Soft-float Conversions - Single Precision
 * Implements __aeabi_f2iz, __aeabi_f2uiz, __aeabi_i2f, __aeabi_ui2f for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Convert single-precision float to signed 32-bit integer (truncate toward zero) */
int __aeabi_f2iz(float a)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a};
  uint32_t bits = ua.u;

  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);

  /* Handle special cases */
  if (exp == 0xFF)
    return 0; /* NaN or Inf -> 0 (undefined behavior anyway) */
  if (exp == 0)
    return 0; /* Zero or denormal */

  /* Add implicit bit */
  mant |= FLOAT_IMPLICIT_BIT;

  /* Calculate actual exponent */
  int actual_exp = exp - FLOAT_EXP_BIAS;

  /* If exponent is negative, result is 0 */
  if (actual_exp < 0)
    return 0;

  /* If exponent >= 31, overflow */
  if (actual_exp >= 31)
    return sign ? (int)0x80000000U : 0x7FFFFFFF;

  /* Shift mantissa to get integer part */
  /* Mantissa has 23 bits of fraction, so shift by (actual_exp - 23) */
  int shift = actual_exp - 23;
  uint32_t result;
  if (shift >= 0)
  {
    result = mant << shift;
  }
  else
  {
    result = mant >> (-shift);
  }

  return sign ? -(int)result : (int)result;
}

/* Convert single-precision float to unsigned 32-bit integer (truncate toward zero) */
unsigned int __aeabi_f2uiz(float a)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a};
  uint32_t bits = ua.u;

  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);

  /* Negative -> 0 */
  if (sign)
    return 0;

  /* Handle special cases */
  if (exp == 0xFF)
    return 0; /* NaN or Inf */
  if (exp == 0)
    return 0; /* Zero or denormal */

  mant |= FLOAT_IMPLICIT_BIT;

  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 32)
    return 0xFFFFFFFFU;

  int shift = actual_exp - 23;
  if (shift >= 0)
  {
    return mant << shift;
  }
  else
  {
    return mant >> (-shift);
  }
}

/* Convert single-precision float to unsigned 64-bit integer (truncate toward zero) */
unsigned long long __aeabi_f2ulz(float a)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a};
  uint32_t bits = ua.u;

  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);

  if (sign)
    return 0;
  if (exp == 0xFF)
    return 0; /* NaN/Inf */
  if (exp == 0)
    return 0; /* Zero/denormal */

  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 64)
    return ~0ULL;

  int shift = actual_exp - 23;
  if (shift >= 0)
  {
    if (shift >= 64)
      return ~0ULL;
    return (unsigned long long)mant << shift;
  }
  else
  {
    return (unsigned long long)mant >> (-shift);
  }
}

/* Convert single-precision float to signed 64-bit integer (truncate toward zero) */
long long __aeabi_f2lz(float a)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a};
  uint32_t bits = ua.u;

  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);

  if (exp == 0xFF)
    return 0; /* NaN/Inf */
  if (exp == 0)
    return 0; /* Zero/denormal */

  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 63)
    return sign ? (long long)0x8000000000000000ULL : (long long)0x7FFFFFFFFFFFFFFFULL;

  int shift = actual_exp - 23;
  unsigned long long magnitude;
  if (shift >= 0)
    magnitude = (unsigned long long)mant << shift;
  else
    magnitude = (unsigned long long)mant >> (-shift);

  return sign ? -(long long)magnitude : (long long)magnitude;
}

/* Convert signed 32-bit integer to single-precision float */
float __aeabi_i2f(int a)
{
  union
  {
    float f;
    uint32_t u;
  } ur;

  if (a == 0)
  {
    ur.u = 0;
    return ur.f;
  }

  int sign = 0;
  uint32_t abs_a;
  if (a < 0)
  {
    sign = 1;
    abs_a = (uint32_t)(-a);
  }
  else
  {
    abs_a = (uint32_t)a;
  }

  /* Find position of MSB */
  int leading_zeros = clz32(abs_a);
  int msb_pos = 31 - leading_zeros;

  /* Exponent = bias + msb_pos */
  int exp = FLOAT_EXP_BIAS + msb_pos;

  /* Shift to get 23-bit mantissa (remove implicit bit) */
  uint32_t mant;
  if (msb_pos > 23)
  {
    mant = abs_a >> (msb_pos - 23);
  }
  else
  {
    mant = abs_a << (23 - msb_pos);
  }
  mant &= FLOAT_MANT_MASK;

  ur.u = ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | mant;
  return ur.f;
}

/* Convert unsigned 32-bit integer to single-precision float */
float __aeabi_ui2f(unsigned int a)
{
  union
  {
    float f;
    uint32_t u;
  } ur;

  if (a == 0)
  {
    ur.u = 0;
    return ur.f;
  }

  /* Find position of MSB */
  int leading_zeros = clz32(a);
  int msb_pos = 31 - leading_zeros;

  int exp = FLOAT_EXP_BIAS + msb_pos;

  uint32_t mant;
  if (msb_pos > 23)
  {
    mant = a >> (msb_pos - 23);
  }
  else
  {
    mant = a << (23 - msb_pos);
  }
  mant &= FLOAT_MANT_MASK;

  ur.u = ((uint32_t)exp << 23) | mant;
  return ur.f;
}
