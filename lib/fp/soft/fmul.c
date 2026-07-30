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

  /* Add the implicit bit for normals; a subnormal's effective exponent is 1,
   * not the stored 0. */
  if (a_exp != 0)
    a_mant |= FLOAT_IMPLICIT_BIT;
  else
    a_exp = 1;
  if (b_exp != 0)
    b_mant |= FLOAT_IMPLICIT_BIT;
  else
    b_exp = 1;

  /* 24 x 24 -> 48 bits.  Each input is m x 2^-23, so the product is p x 2^-46. */
  uint64_t product = (uint64_t)a_mant * (uint64_t)b_mant;
  int result_exp = a_exp + b_exp - FLOAT_EXP_BIAS;

  /* Normalize the product to a known leading-bit position *before* narrowing
   * it to 32 bits.  A subnormal operand leaves the leading bit far below 46,
   * so a fixed shift would discard significant bits rather than just guard
   * bits (FLT_MIN_SUBNORMAL * FLT_MAX lost 20 bits of significand that way). */
  {
    int lead = 63 - clz64(product);
    int adj = 46 - lead;
    if (adj > 0)
      product <<= adj;
    else if (adj < 0)
      product = sfp_shr_sticky64(product, -adj);
    result_exp -= adj;
  }

  /* Reduce to the position sfp_round_pack_float() expects (leading bit at
   * 23 + SFP_GRS), folding the discarded bits into the sticky bit rather than
   * dropping them -- dropping them truncated every inexact product. */
  uint32_t result_mant = (uint32_t)sfp_shr_sticky64(product, 46 - (23 + SFP_GRS));

  ur.u = sfp_round_pack_float(result_sign, result_exp, result_mant);
  return ur.f;
}
