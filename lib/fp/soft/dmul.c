/*
 * Soft-float Multiplication - Double Precision
 * Implements __aeabi_dmul for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Multiply two 32-bit values to get 64-bit result */
static inline void mul32x32(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
  uint64_t result = (uint64_t)a * (uint64_t)b;
  *hi = (uint32_t)(result >> 32);
  *lo = (uint32_t)result;
}

/* Multiply two 64-bit mantissas, return high 64 bits + overflow info */
static uint64_t mul_mant(uint64_t a, uint64_t b, int *extra_bit)
{
  /* Split into 32-bit parts: a = a_hi * 2^32 + a_lo */
  uint32_t a_hi = (uint32_t)(a >> 32);
  uint32_t a_lo = (uint32_t)a;
  uint32_t b_hi = (uint32_t)(b >> 32);
  uint32_t b_lo = (uint32_t)b;

  /* Full 128-bit product: a*b = a_hi*b_hi*2^64 + (a_hi*b_lo + a_lo*b_hi)*2^32 + a_lo*b_lo */
  uint32_t p0_hi, p0_lo; /* a_lo * b_lo */
  uint32_t p1_hi, p1_lo; /* a_lo * b_hi */
  uint32_t p2_hi, p2_lo; /* a_hi * b_lo */
  uint32_t p3_hi, p3_lo; /* a_hi * b_hi */

  mul32x32(a_lo, b_lo, &p0_hi, &p0_lo);
  mul32x32(a_lo, b_hi, &p1_hi, &p1_lo);
  mul32x32(a_hi, b_lo, &p2_hi, &p2_lo);
  mul32x32(a_hi, b_hi, &p3_hi, &p3_lo);

  /* Sum the middle parts with carry */
  uint64_t mid = (uint64_t)p0_hi + (uint64_t)p1_lo + (uint64_t)p2_lo;
  uint64_t high = (uint64_t)p3_lo + (uint64_t)p1_hi + (uint64_t)p2_hi + (mid >> 32);
  high = (high << 32) | (mid & 0xFFFFFFFF);
  high += (uint64_t)p3_hi << 32;

  /* Check if MSB of result is set (for normalization) */
  *extra_bit = (high >> 63) & 1;

  return high;
}

/* Multiply two double-precision floats */
double __aeabi_dmul(double a, double b)
{
  union
  {
    double d;
    uint64_t u;
  } ua = {.d = a}, ub = {.d = b}, ur;
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

  /* Multiply mantissas (53-bit * 53-bit = 106-bit result) */
  /* We need the top 53 bits of the result */
  int extra_bit;
  uint64_t result_mant = mul_mant(a_mant, b_mant, &extra_bit);

  /* Normalize: the product of two 1.xxx numbers is in range [1, 4) */
  /* If MSB (bit 63) is set, we have overflow, need to shift right */
  if (extra_bit)
  {
    result_mant >>= 1;
    result_exp++;
  }

  /* Shift to get 52-bit mantissa (remove implicit bit position) */
  result_mant >>= (64 - 53); /* Shift to position mantissa */

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
  result_mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}
