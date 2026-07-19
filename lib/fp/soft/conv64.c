/*
 * Soft-float Conversions - float/double -> 64-bit integer
 * Implements __aeabi_f2lz, __aeabi_f2ulz, __aeabi_d2lz, __aeabi_d2ulz.
 *
 * These live apart from conv.c / dconv.c on purpose.  A hardware FP runtime
 * (vfpv4-sp, vfpv5-dp, rp2350) overrides the 32-bit conversions with its own
 * instruction sequences but has no 64-bit form to offer -- FPv5-SP cannot
 * convert to 64-bit at all, and the DCP's integer ports are 32-bit.  Keeping
 * the 64-bit conversions in their own translation unit lets those runtimes
 * link this object without dragging in a duplicate __aeabi_i2f / __aeabi_i2d.
 */

#include "../fp_abi.h"
#include "soft_common.h"

unsigned long long __aeabi_llsr(unsigned long long a, int b);
long long __aeabi_llsl(long long a, int b);

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
  return (unsigned long long)mant >> (-shift);
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
