/*
 * Soft-float Multiplication - Double Precision
 * Implements __aeabi_dmul for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* 64x64 -> 128 multiply.
 *
 * Keep multiplications to 32x32->64, but avoid doing 64-bit additions.
 * Some low-opt codegen paths for 64-bit add/adc are unreliable; accumulating
 * in 32-bit words with explicit carry keeps the result stable at -O0/-O1.
 */
static inline uint32_t add32_c(uint32_t a, uint32_t b, uint32_t cin, uint32_t *cout)
{
  uint32_t s = a + b;
  uint32_t c = (s < a);
  uint32_t s2 = s + cin;
  c |= (s2 < s);
  *cout = c;
  return s2;
}

static inline void add64_shift32(uint32_t *w1, uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi)
{
  uint32_t c;
  *w1 = add32_c(*w1, lo, 0, &c);
  *w2 = add32_c(*w2, hi, c, &c);
  *w3 = add32_c(*w3, 0, c, &c);
}

static inline void add64_shift64(uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi)
{
  uint32_t c;
  *w2 = add32_c(*w2, lo, 0, &c);
  *w3 = add32_c(*w3, hi, c, &c);
}

static inline void mul32wide_u32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
  const uint32_t a0 = a & 0xFFFFu;
  const uint32_t a1 = a >> 16;
  const uint32_t b0 = b & 0xFFFFu;
  const uint32_t b1 = b >> 16;

  const uint32_t p0 = a0 * b0;
  const uint32_t p1 = a0 * b1;
  const uint32_t p2 = a1 * b0;
  const uint32_t p3 = a1 * b1;

  const uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
  *lo = (p0 & 0xFFFFu) | (mid << 16);
  *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

static inline void mul64wide(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  /* Avoid 64-bit shifts-by-32 here.
   * Some low-opt codegen paths have historically produced wrong results for
   * those, which breaks the wide-multiply path for non-power-of-two inputs.
   */
  u64_words aa;
  u64_words bb;
  aa.u = a;
  bb.u = b;

  uint32_t a0 = aa.w.lo;
  uint32_t a1 = aa.w.hi;
  uint32_t b0 = bb.w.lo;
  uint32_t b1 = bb.w.hi;

  uint32_t p0_lo, p0_hi;
  uint32_t p1_lo, p1_hi;
  uint32_t p2_lo, p2_hi;
  uint32_t p3_lo, p3_hi;
  mul32wide_u32(a0, b0, &p0_lo, &p0_hi);
  mul32wide_u32(a0, b1, &p1_lo, &p1_hi);
  mul32wide_u32(a1, b0, &p2_lo, &p2_hi);
  mul32wide_u32(a1, b1, &p3_lo, &p3_hi);

  uint32_t w0 = p0_lo;
  uint32_t w1 = p0_hi;
  uint32_t w2 = 0;
  uint32_t w3 = 0;

  add64_shift32(&w1, &w2, &w3, p1_lo, p1_hi);
  add64_shift32(&w1, &w2, &w3, p2_lo, p2_hi);
  add64_shift64(&w2, &w3, p3_lo, p3_hi);

  u64_words out_lo;
  u64_words out_hi;
  out_lo.w.lo = w0;
  out_lo.w.hi = w1;
  out_hi.w.lo = w2;
  out_hi.w.hi = w3;
  *lo = out_lo.u;
  *hi = out_hi.u;
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

  /* Fast path: multiplying by an exact power-of-two keeps the other mantissa
   * unchanged (no rounding), only the exponent is adjusted.
   *
   * This also avoids low-opt codegen pitfalls in the wide-multiply path.
   */
  if (a_exp != 0 && b_exp != 0)
  {
    if (a_mant == 0)
    {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF)
      {
        ur.u = make_double(result_sign, 0x7FF, 0);
        return ur.d;
      }
      if (exp <= 0)
      {
        ur.u = make_double(result_sign, 0, 0);
        return ur.d;
      }
      ur.u = make_double(result_sign, exp, b_mant);
      return ur.d;
    }
    if (b_mant == 0)
    {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF)
      {
        ur.u = make_double(result_sign, 0x7FF, 0);
        return ur.d;
      }
      if (exp <= 0)
      {
        ur.u = make_double(result_sign, 0, 0);
        return ur.d;
      }
      ur.u = make_double(result_sign, exp, a_mant);
      return ur.d;
    }
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
  /* Determine whether the top bit is at position 105 (vs 104). Avoid 64-bit
   * masking/shift here; use 32-bit word access instead.
   *
   * bit105 is bit 41 within prod_hi, i.e. bit 9 of prod_hi.hi (bits 32..63).
   */
  u64_words prod_hi_w;
  prod_hi_w.u = prod_hi;
  int shift = 52;
  if (prod_hi_w.w.hi & (1u << 9))
  {
    shift = 53;
    result_exp++;
  }

  /* Compute mant = prod >> shift (yields a 53-bit value with implicit bit).
   *
   * Do this with 32-bit pieces to avoid fragile 64-bit shift codegen on some
   * low-opt paths.
   */
  u64_words prod_lo_w;
  u64_words prod_hi_w2;
  prod_lo_w.u = prod_lo;
  prod_hi_w2.u = prod_hi;

  const uint32_t prod_lo_lo = prod_lo_w.w.lo;
  const uint32_t prod_lo_hi = prod_lo_w.w.hi;
  const uint32_t prod_hi_lo = prod_hi_w2.w.lo;
  const uint32_t prod_hi_hi = prod_hi_w2.w.hi;

  uint32_t mant_lo32;
  uint32_t mant_hi32;
  int guard;
  int sticky;
  if (shift == 52)
  {
    /* mant = (prod_hi << 12) | (prod_lo >> 52) */
    mant_lo32 = (prod_hi_lo << 12) | (prod_lo_hi >> 20);
    mant_hi32 = (prod_hi_hi << 12) | (prod_hi_lo >> 20);

    /* guard is bit 51 of prod_lo => bit 19 of prod_lo_hi */
    guard = (int)((prod_lo_hi >> 19) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 19) - 1u)) != 0);
  }
  else
  {
    /* shift == 53: mant = (prod_hi << 11) | (prod_lo >> 53) */
    mant_lo32 = (prod_hi_lo << 11) | (prod_lo_hi >> 21);
    mant_hi32 = (prod_hi_hi << 11) | (prod_hi_lo >> 21);

    /* guard is bit 52 of prod_lo => bit 20 of prod_lo_hi */
    guard = (int)((prod_lo_hi >> 20) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 20) - 1u)) != 0);
  }

  uint64_t mant = ((uint64_t)mant_hi32 << 32) | (uint64_t)mant_lo32;

  /* Round to nearest, ties to even: increment if guard==1 and
   * (sticky==1 or LSB==1).
   */
  if (guard && (sticky || (mant & 1ULL)))
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
