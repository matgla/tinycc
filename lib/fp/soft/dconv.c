/*
 * Soft-float Conversions - Double Precision and Float<->Double
 * Implements __aeabi_i2d, __aeabi_d2iz, __aeabi_f2d, __aeabi_d2f for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

unsigned long long __aeabi_llsr(unsigned long long a, int b);
long long __aeabi_llsl(long long a, int b);

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
  uint64_t mant = (uint64_t)__aeabi_llsl((long long)abs_a, 52 - msb_pos);
  mant &= DOUBLE_MANT_MASK;

  ur.u = make_double(sign, exp, mant);
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

  uint64_t mant = (uint64_t)__aeabi_llsl((long long)a, 52 - msb_pos);
  mant &= DOUBLE_MANT_MASK;

  ur.u = make_double(0, exp, mant);
  return ur.d;
}

/* Convert double to signed int (truncate toward zero) */
int __aeabi_d2iz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
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
    result = (uint32_t)__aeabi_llsl((long long)mant, shift);
  else
    result = (uint32_t)__aeabi_llsr(mant, -shift);

  return sign ? -(int)result : (int)result;
}

/* Convert double to unsigned int (truncate toward zero) */
unsigned int __aeabi_d2uiz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
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
    return (uint32_t)__aeabi_llsl((long long)mant, shift);
  return (uint32_t)__aeabi_llsr(mant, -shift);
}

/* Convert double to unsigned 64-bit integer (truncate toward zero) */
unsigned long long __aeabi_d2ulz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
  uint64_t bits = ua.u;

  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  if (sign)
    return 0;
  if (exp == 0x7FF)
    return 0; /* NaN/Inf */
  if (exp == 0)
    return 0; /* Zero/denormal */

  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 64)
    return ~0ULL;

  int shift = actual_exp - 52;
  if (shift >= 0)
  {
    if (shift >= 64)
      return ~0ULL;
    return (unsigned long long)__aeabi_llsl((long long)mant, shift);
  }
  return (unsigned long long)__aeabi_llsr(mant, -shift);
}

/* Convert double to signed 64-bit integer (truncate toward zero) */
long long __aeabi_d2lz(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
  uint64_t bits = ua.u;

  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  if (exp == 0x7FF)
    return 0; /* NaN/Inf */
  if (exp == 0)
    return 0; /* Zero/denormal */

  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0)
    return 0;
  if (actual_exp >= 63)
    return sign ? (long long)0x8000000000000000ULL : (long long)0x7FFFFFFFFFFFFFFFULL;

  int shift = actual_exp - 52;
  unsigned long long magnitude;
  if (shift >= 0)
    magnitude = (unsigned long long)__aeabi_llsl((long long)mant, shift);
  else
    magnitude = (unsigned long long)__aeabi_llsr(mant, -shift);

  return sign ? -(long long)magnitude : (long long)magnitude;
}

/* Convert single to double precision (raw float bits in r0). */
double __aeabi_f2d_bits(uint32_t bits)
{
  union
  {
    struct
    {
      uint32_t lo;
      uint32_t hi;
    } w;
    double d;
  } ur;

  int sign = (bits >> 31) & 1;
  int exp = (bits >> 23) & 0xFF;
  uint32_t mant = bits & FLOAT_MANT_MASK;

  /* Handle special cases */
  if (exp == 0xFF)
  {
    /* Inf or NaN */
    ur.w.hi = ((uint32_t)sign << 31) | 0x7FF00000u | (mant >> 3);
    ur.w.lo = mant << 29;
    return ur.d;
  }
  if (exp == 0 && mant == 0)
  {
    /* Zero */
    ur.w.hi = (uint32_t)sign << 31;
    ur.w.lo = 0;
    return ur.d;
  }

  /* Convert exponent: remove float bias, add double bias */
  int new_exp = exp - FLOAT_EXP_BIAS + DOUBLE_EXP_BIAS;

  /* Build double using 32-bit words to avoid 64-bit shifts. */
  ur.w.hi = ((uint32_t)sign << 31) | ((uint32_t)new_exp << 20) | (mant >> 3);
  ur.w.lo = mant << 29;
  return ur.d;
}

/* Convert double to single precision */
float __aeabi_d2f(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
  union
  {
    float f;
    uint32_t u;
  } ur;
  uint64_t bits = ua.u;

  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  /* Handle special cases */
  if (exp == 0x7FF)
  {
    /* Inf or NaN */
    ur.u = ((uint32_t)sign << 31) | 0x7F800000U | ((uint32_t)__aeabi_llsr(mant, 29) & FLOAT_MANT_MASK);
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
  uint32_t new_mant = (uint32_t)__aeabi_llsr(mant, 29);

  ur.u = ((uint32_t)sign << 31) | ((uint32_t)new_exp << 23) | new_mant;
  return ur.f;
}
