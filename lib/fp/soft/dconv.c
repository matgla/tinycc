/*
 * Soft-float Conversions - Double Precision and Float<->Double
 * Implements __aeabi_i2d, __aeabi_d2iz, __aeabi_f2d, __aeabi_d2f for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Convert signed int to double */
double __aeabi_i2d(int a)
{
  union
  {
    double d;
    uint64_t u;
  } ur;

  if (a == 0)
  {
    ur.u = 0;
    return ur.d;
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

  /* Find MSB position */
  int leading_zeros = clz32(abs_a);
  int msb_pos = 31 - leading_zeros;

  /* Exponent = bias + msb_pos */
  int exp = DOUBLE_EXP_BIAS + msb_pos;

  /* Shift to get 52-bit mantissa */
  uint64_t mant = (uint64_t)abs_a << (52 - msb_pos);
  mant &= DOUBLE_MANT_MASK;

  ur.u = ((uint64_t)sign << 63) | ((uint64_t)exp << 52) | mant;
  return ur.d;
}

/* Convert unsigned int to double */
double __aeabi_ui2d(unsigned int a)
{
  union
  {
    double d;
    uint64_t u;
  } ur;

  if (a == 0)
  {
    ur.u = 0;
    return ur.d;
  }

  int leading_zeros = clz32(a);
  int msb_pos = 31 - leading_zeros;

  int exp = DOUBLE_EXP_BIAS + msb_pos;

  uint64_t mant = (uint64_t)a << (52 - msb_pos);
  mant &= DOUBLE_MANT_MASK;

  ur.u = ((uint64_t)exp << 52) | mant;
  return ur.d;
}

/* Convert double to signed int (truncate toward zero) */
int __aeabi_d2iz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua = {.d = a};
  uint64_t bits = ua.u;

  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  /* Handle special cases */
  if (exp == 0x7FF)
    return 0; /* NaN or Inf */
  if (exp == 0)
    return 0; /* Zero or denormal */

  mant |= DOUBLE_IMPLICIT_BIT;

  int actual_exp = exp - DOUBLE_EXP_BIAS;

  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 31)
    return sign ? (int)0x80000000U : 0x7FFFFFFF;

  /* Shift mantissa: 52 bits of fraction */
  int shift = actual_exp - 52;
  uint32_t result;
  if (shift >= 0)
  {
    result = (uint32_t)(mant << shift);
  }
  else
  {
    result = (uint32_t)(mant >> (-shift));
  }

  return sign ? -(int)result : (int)result;
}

/* Convert double to unsigned int (truncate toward zero) */
unsigned int __aeabi_d2uiz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua = {.d = a};
  uint64_t bits = ua.u;

  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  if (sign)
    return 0;
  if (exp == 0x7FF)
    return 0;
  if (exp == 0)
    return 0;

  mant |= DOUBLE_IMPLICIT_BIT;

  int actual_exp = exp - DOUBLE_EXP_BIAS;

  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 32)
    return 0xFFFFFFFFU;

  int shift = actual_exp - 52;
  if (shift >= 0)
  {
    return (uint32_t)(mant << shift);
  }
  else
  {
    return (uint32_t)(mant >> (-shift));
  }
}

/* Convert single to double precision */
double __aeabi_f2d(float a)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a};
  union
  {
    double d;
    uint64_t u;
  } ur;
  uint32_t bits = ua.u;

  int sign = (bits >> 31) & 1;
  int exp = (bits >> 23) & 0xFF;
  uint32_t mant = bits & FLOAT_MANT_MASK;

  /* Handle special cases */
  if (exp == 0xFF)
  {
    /* Inf or NaN */
    ur.u = ((uint64_t)sign << 63) | DOUBLE_EXP_MASK | ((uint64_t)mant << 29);
    return ur.d;
  }
  if (exp == 0 && mant == 0)
  {
    /* Zero */
    ur.u = (uint64_t)sign << 63;
    return ur.d;
  }

  /* Convert exponent: remove float bias, add double bias */
  int new_exp = exp - FLOAT_EXP_BIAS + DOUBLE_EXP_BIAS;

  /* Expand mantissa from 23 bits to 52 bits */
  uint64_t new_mant = (uint64_t)mant << 29;

  ur.u = ((uint64_t)sign << 63) | ((uint64_t)new_exp << 52) | new_mant;
  return ur.d;
}

/* Convert double to single precision */
float __aeabi_d2f(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua = {.d = a};
  union
  {
    float f;
    uint32_t u;
  } ur;
  uint64_t bits = ua.u;

  int sign = (bits >> 63) & 1;
  int exp = (bits >> 52) & 0x7FF;
  uint64_t mant = bits & DOUBLE_MANT_MASK;

  /* Handle special cases */
  if (exp == 0x7FF)
  {
    /* Inf or NaN */
    ur.u = ((uint32_t)sign << 31) | 0x7F800000U | ((uint32_t)(mant >> 29) & FLOAT_MANT_MASK);
    return ur.f;
  }
  if (exp == 0 && mant == 0)
  {
    /* Zero */
    ur.u = (uint32_t)sign << 31;
    return ur.f;
  }

  /* Convert exponent */
  int new_exp = exp - DOUBLE_EXP_BIAS + FLOAT_EXP_BIAS;

  /* Check for overflow -> infinity */
  if (new_exp >= 0xFF)
  {
    ur.u = ((uint32_t)sign << 31) | 0x7F800000U;
    return ur.f;
  }

  /* Check for underflow -> zero */
  if (new_exp <= 0)
  {
    ur.u = (uint32_t)sign << 31;
    return ur.f;
  }

  /* Truncate mantissa from 52 bits to 23 bits */
  uint32_t new_mant = (uint32_t)(mant >> 29);

  ur.u = ((uint32_t)sign << 31) | ((uint32_t)new_exp << 23) | new_mant;
  return ur.f;
}
