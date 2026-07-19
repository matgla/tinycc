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
  return mant >> (-shift);
}

/* The 64-bit forms (__aeabi_f2lz / __aeabi_f2ulz) live in conv64.c so that a
 * hardware FP runtime can link them without also getting the 32-bit
 * conversions above, which it implements itself. */

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
    /* Negate in unsigned arithmetic: -a overflows for INT_MIN, which is
     * undefined behaviour and one of the values under test. */
    abs_a = (uint32_t)0 - (uint32_t)a;
  }
  else
  {
    abs_a = (uint32_t)a;
  }

  /* Find position of MSB */
  int leading_zeros = clz32(abs_a);
  int msb_pos = 31 - leading_zeros;

  /* Align the MSB to 23 + SFP_GRS and let the shared core round.  A plain
   * >> (msb_pos - 23) truncated toward zero, so any integer needing more than
   * 24 significant bits came out low -- INT_MAX became 2147483520.0f instead
   * of 2147483648.0f. */
  uint32_t mant;
  int shift = (23 + SFP_GRS) - msb_pos;
  if (shift >= 0)
    mant = abs_a << shift;
  else
    mant = sfp_shr_sticky32(abs_a, -shift);

  ur.u = sfp_round_pack_float(sign, FLOAT_EXP_BIAS + msb_pos, mant);
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

  /* Same rounding fix as __aeabi_i2f: UINT_MAX must round up to 2^32, not
   * truncate down to 4294967040.0f. */
  uint32_t mant;
  int shift = (23 + SFP_GRS) - msb_pos;
  if (shift >= 0)
    mant = a << shift;
  else
    mant = sfp_shr_sticky32(a, -shift);

  ur.u = sfp_round_pack_float(0, FLOAT_EXP_BIAS + msb_pos, mant);
  return ur.f;
}
