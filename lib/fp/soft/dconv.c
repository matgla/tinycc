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

/* The 64-bit forms (__aeabi_d2lz / __aeabi_d2ulz) live in conv64.c so that a
 * hardware FP runtime can link them without also getting the 32-bit
 * conversions above, which it implements itself. */

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

  if (exp == 0)
  {
    /* Subnormal float.  Its significand has no implicit leading 1, so copying
     * it straight across (as the code below does for normals) produced a
     * completely wrong value -- FLT_MIN_SUBNORMAL widened to ~5.4e-39 instead
     * of 1.4e-45.  Double has ample exponent range, so every float subnormal
     * is exactly representable as a *normal* double once normalized. */
    int msb_pos = 31 - clz32(mant); /* 0..22 */
    int sub_exp = DOUBLE_EXP_BIAS - FLOAT_EXP_BIAS + 1 + msb_pos - 23;
    uint64_t dmant = (uint64_t)__aeabi_llsl((long long)mant, 52 - msb_pos) & DOUBLE_MANT_MASK;

    ur.w.hi = ((uint32_t)sign << 31) | ((uint32_t)sub_exp << 20) | (uint32_t)(dmant >> 32);
    ur.w.lo = (uint32_t)dmant;
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

  /* Restore the implicit bit for normals; a subnormal double is far below the
   * float range and will simply round away, but it must still go through the
   * same path rather than being special-cased to zero. */
  uint64_t sig;
  int src_exp;
  if (exp != 0)
  {
    sig = mant | DOUBLE_IMPLICIT_BIT;
    src_exp = exp;
  }
  else
  {
    sig = mant;
    src_exp = 1;
  }

  int new_exp = src_exp - DOUBLE_EXP_BIAS + FLOAT_EXP_BIAS;

  /* Narrow the 53-bit significand (leading bit at 52) to the position the
   * rounding core expects, keeping everything shifted out as sticky.  The old
   * code truncated with a bare >> 29 and flushed any result with new_exp <= 0
   * to zero, so (double)(1/3.0) narrowed to 0x3eaaaaaa instead of 0x3eaaaaab
   * and every subnormal float result was lost. */
  uint32_t new_mant = (uint32_t)sfp_shr_sticky64(sig, 52 - (23 + SFP_GRS));

  ur.u = sfp_round_pack_float(sign, new_exp, new_mant);
  return ur.f;
}
