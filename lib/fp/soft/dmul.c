/*
 * Soft-float Multiplication - Double Precision
 * Implements __aeabi_dmul for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* 64x64 -> 128 multiply using 32-bit partial products.
 * Returns the full product as (hi, lo) words.
 */
static inline void mul64wide(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  const uint64_t a0 = (uint32_t)a;
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = (uint32_t)b;
  const uint64_t b1 = b >> 32;

  const uint64_t p0 = a0 * b0;
  const uint64_t p1 = a0 * b1;
  const uint64_t p2 = a1 * b0;
  const uint64_t p3 = a1 * b1;

  const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
  *lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
  *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

/* Multiply two double-precision floats */
double __aeabi_dmul(double a, double b)
{
  union
  {
    double d;
    uint64_t u;
  } ua, ub, ur;
  ua.d = a;
  ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;

  int a_sign = double_sign(a_bits);
  int b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits);
  int b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits);
  uint64_t b_mant = double_mant(b_bits);

  /* Result sign is XOR of input signs */
  int result_sign = a_sign ^ b_sign;

  /* Handle NaN */
  if (is_nan_bits(a_bits))
  {
    ur.u = a_bits;
    return ur.d;
  }
  if (is_nan_bits(b_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }

  /* Handle infinity */
  if (is_inf_bits(a_bits))
  {
    if (is_zero_bits(b_bits))
    {
      /* inf * 0 = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_inf_bits(b_bits))
  {
    if (is_zero_bits(a_bits))
    {
      /* 0 * inf = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }

  /* Handle zero */
  if (is_zero_bits(a_bits) || is_zero_bits(b_bits))
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Add implicit bit for normalized numbers */
  if (a_exp != 0)
    a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= DOUBLE_IMPLICIT_BIT;

  /* Calculate result exponent: ea + eb - bias */
  int result_exp = a_exp + b_exp - DOUBLE_EXP_BIAS;

  /* Multiply mantissas (53-bit * 53-bit = up to 106-bit result).
   * Mantissas are integer values with the implicit bit set at bit 52.
   * The raw product therefore has its leading 1 at bit 104 or 105.
   */
  uint64_t prod_hi, prod_lo;
  mul64wide(a_mant, b_mant, &prod_hi, &prod_lo);

  /* Normalize so the implicit bit ends up at bit 52.
   * If bit105 is set, shift by 53 and increment exponent.
   * Otherwise shift by 52.
   */
  const uint64_t bit105_mask = 1ULL << (105 - 64); /* bit 41 within prod_hi */
  int shift = 52;
  if (prod_hi & bit105_mask)
  {
    shift = 53;
    result_exp++;
  }

  /* Compute mant = prod >> shift (this yields a 53-bit value with implicit bit). */
  uint64_t mant = (prod_hi << (64 - shift)) | (prod_lo >> shift);

  /* Round to nearest, ties to even, using the remaining low 'shift' bits. */
  const uint64_t rem_mask = (1ULL << shift) - 1ULL;
  const uint64_t rem = prod_lo & rem_mask;
  const uint64_t halfway = 1ULL << (shift - 1);
  if (rem > halfway || (rem == halfway && (mant & 1ULL)))
    mant++;

  /* Handle rounding overflow (e.g. 1.111... + 1 ulp -> 10.000...). */
  if (mant & (DOUBLE_IMPLICIT_BIT << 1))
  {
    mant >>= 1;
    result_exp++;
  }

  /* Check for overflow to infinity */
  if (result_exp >= 0x7FF)
  {
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }

  /* Check for underflow to zero */
  if (result_exp <= 0)
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Remove implicit bit */
  mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, mant);
  return ur.d;
}
