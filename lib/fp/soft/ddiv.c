/*
 * Soft-float Division - Double Precision
 * Implements __aeabi_ddiv for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* ============================================================================
 * Significand division
 *
 * The quotient the packer wants is exactly
 *
 *     Q = floor(dividend * 2^55 / divisor),   sticky = (that remainder != 0)
 *
 * which a restoring loop used to produce one bit at a time: 56 iterations of
 * ~25 instructions, and a dynamic profile of the benchmark's double kernels
 * put 280,000 iterations through it against 5,000 calls through the whole rest
 * of this file.  Cortex-M33 has a 32-bit UDIV, so the same quotient comes out
 * of two Knuth algorithm-D digit steps instead.
 *
 * The two layers are separately verified against `unsigned __int128` on the
 * host (6.1M cases over the ddiv domain, arbitrary normalized divisors, and
 * the clamp/edge paths), and the whole thing against the old loop bit-for-bit
 * (8M cases).
 * ==========================================================================*/

/* 64/32 -> 32, for an already-normalized divisor (v >= 2^31) and u1 < v so the
 * quotient fits in a digit.  Hacker's Delight "divlu" with the normalization
 * shift folded out; two 32-bit UDIVs, no 64-bit division anywhere. */
static uint32_t sfp_divlu32(uint32_t u1, uint32_t u0, uint32_t v, uint32_t *r)
{
  const uint32_t b = 65536u;
  uint32_t vn1 = v >> 16, vn0 = v & 0xFFFFu;
  uint32_t un1 = u0 >> 16, un0 = u0 & 0xFFFFu;
  uint32_t q1, q0, rhat, un21;

  q1 = u1 / vn1;
  rhat = u1 - q1 * vn1;
  while (q1 >= b || q1 * vn0 > b * rhat + un1)
  {
    q1--;
    rhat += vn1;
    if (rhat >= b)
      break;
  }

  /* Wraps mod 2^32; the true value is a partial remainder and fits. */
  un21 = u1 * b + un1 - q1 * v;

  q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;
  while (q0 >= b || q0 * vn0 > b * rhat + un0)
  {
    q0--;
    rhat += vn1;
    if (rhat >= b)
      break;
  }

  *r = un21 * b + un0 - q0 * v;
  return q1 * b + q0;
}

/* One algorithm-D digit step: divide the three-digit window (w2:w1:w0) by the
 * two-digit divisor (v1:v0), leaving the remainder in the window.  A macro
 * rather than a function taking pointers, because pointer parameters would
 * make the window words address-taken locals and cost them their registers --
 * which in the hottest routine in the library is the whole point. */
#define SFP_DIVSTEP(w2, w1, w0, qd)                                                            \
  do                                                                                           \
  {                                                                                            \
    uint32_t qh_;                                                                              \
    uint64_t rh_; /* 64-bit on purpose: it legitimately exceeds 2^32 */                        \
    if ((w2) >= v1)                                                                            \
    {                                                                                          \
      /* The digit would be 2^32.  Clamp, and carry the remainder that goes    */              \
      /* with the clamped value: w2*2^32 + w1 - (2^32-1)*v1.                   */              \
      qh_ = 0xFFFFFFFFu;                                                                       \
      rh_ = ((uint64_t)((w2) - v1) << 32) + (w1) + v1;                                         \
    }                                                                                          \
    else                                                                                       \
    {                                                                                          \
      uint32_t r32_;                                                                           \
      qh_ = sfp_divlu32((w2), (w1), v1, &r32_);                                                \
      rh_ = r32_;                                                                              \
    }                                                                                          \
    /* Dividing by the top divisor digit alone can overshoot by two, and the   */              \
    /* add-back below only recovers one.  Bringing in the second divisor digit */              \
    /* bounds the error to one.  Once rh_ reaches 2^32 the test cannot fail.   */              \
    while (rh_ < ((uint64_t)1 << 32) && (uint64_t)qh_ * v0 > ((rh_ << 32) | (w0)))             \
    {                                                                                          \
      qh_--;                                                                                   \
      rh_ += v1;                                                                               \
    }                                                                                          \
    {                                                                                          \
      /* (w2:w1:w0) -= qh_ * (v1:v0) */                                                        \
      uint64_t p0_ = (uint64_t)qh_ * v0;                                                       \
      uint64_t p1_ = (uint64_t)qh_ * v1 + (p0_ >> 32);                                         \
      int64_t t0_ = (int64_t)(uint64_t)(w0) - (int64_t)(uint64_t)(uint32_t)p0_;                \
      (w0) = (uint32_t)t0_;                                                                    \
      int64_t t1_ = (int64_t)(uint64_t)(w1) - (int64_t)(uint64_t)(uint32_t)p1_ - (t0_ < 0);    \
      (w1) = (uint32_t)t1_;                                                                    \
      int64_t t2_ =                                                                            \
          (int64_t)(uint64_t)(w2) - (int64_t)(uint64_t)(uint32_t)(p1_ >> 32) - (t1_ < 0);      \
      (w2) = (uint32_t)t2_;                                                                    \
      if (t2_ < 0)                                                                             \
      {                                                                                        \
        /* Over-estimated by one after all: put the divisor back. */                           \
        qh_--;                                                                                 \
        uint64_t s0_ = (uint64_t)(w0) + v0;                                                    \
        (w0) = (uint32_t)s0_;                                                                  \
        uint64_t s1_ = (uint64_t)(w1) + v1 + (s0_ >> 32);                                      \
        (w1) = (uint32_t)s1_;                                                                  \
        (w2) = (uint32_t)((uint64_t)(w2) + (s1_ >> 32));                                       \
      }                                                                                        \
    }                                                                                          \
    (qd) = qh_;                                                                                \
  } while (0)

/* floor(dividend * 2^55 / divisor), with the sticky bit OR'd into bit 0 --
 * bit-identical to what the restoring loop returned.
 *
 * Callers guarantee divisor in [2^52, 2^53) (the implicit bit is set, and
 * subnormals were scaled up first), so it has exactly 11 leading zeros and
 * normalizing is a constant shift.  Scaling both sides by 2^11 leaves the
 * quotient unchanged and the remainder multiplied by 2^11, which cannot change
 * whether it is zero.  dividend < 2^54, so the numerator dividend * 2^66 is
 * (dividend << 2) * 2^64: its low 64 bits are zero and its high word is far
 * below the normalized divisor, so the quotient fits in 64 bits. */
static uint64_t sfp_div_sig64(uint64_t dividend, uint64_t divisor)
{
  const uint64_t d = divisor << 11;
  const uint32_t v1 = (uint32_t)(d >> 32), v0 = (uint32_t)d;
  uint32_t w3 = (uint32_t)((dividend << 2) >> 32);
  uint32_t w2 = (uint32_t)(dividend << 2);
  uint32_t w1 = 0, w0 = 0;
  uint32_t q1, q0;

  SFP_DIVSTEP(w3, w2, w1, q1);
  SFP_DIVSTEP(w2, w1, w0, q0);

  return (((uint64_t)q1 << 32) | q0) | ((w1 | w0) != 0);
}

#undef SFP_DIVSTEP

/* Divide two double-precision floats */
double __aeabi_ddiv(double a, double b)
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

  /* NaN, infinity, zero and the implicit bit, nested rather than written as a
   * flat sequence of independent `if`s.
   *
   * Flat, this tested `a_exp == 0x7FF` twice, `b_exp == 0x7FF` twice and each
   * `exp == 0` twice (once for the zero case, again for the implicit bit) --
   * every re-test decided by an earlier branch on the same value.  A compiler
   * that threads jumps through duplicated test blocks removes them; tcc does
   * not thread, so it evaluated all of them on the common path.  See the same
   * rewrite in dadd.c. */
  if (a_exp == 0x7FF)
  {
    if (a_mant != 0)
    {
      ur.u = a_bits; /* a is NaN */
      return ur.d;
    }
    /* a is an infinity; a NaN b still wins, as in the flat order. */
    if (b_exp == 0x7FF)
    {
      if (b_mant != 0)
      {
        ur.u = b_bits; /* b is NaN */
        return ur.d;
      }
      ur.u = 0x7FF8000000000000ULL; /* inf / inf = NaN */
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0); /* inf / x = inf */
    return ur.d;
  }
  if (b_exp == 0x7FF)
  {
    if (b_mant != 0)
    {
      ur.u = b_bits; /* b is NaN */
      return ur.d;
    }
    ur.u = make_double(result_sign, 0, 0); /* x / inf = 0 */
    return ur.d;
  }

  /* Both operands are finite.  The divisor's zero test and its implicit bit
   * share one `b_exp == 0`; likewise for the dividend.  Scale subnormals into
   * normal form and pay for it in the exponent, so the restoring division
   * below always starts from a full-width significand. */
  if (b_exp == 0)
  {
    if (b_mant == 0)
    {
      if (a_exp == 0 && a_mant == 0)
      {
        ur.u = 0x7FF8000000000000ULL; /* 0 / 0 = NaN */
        return ur.d;
      }
      ur.u = make_double(result_sign, 0x7FF, 0); /* x / 0 = inf */
      return ur.d;
    }
    b_exp = 1;
    while (!(b_mant & DOUBLE_IMPLICIT_BIT))
    {
      b_mant <<= 1;
      b_exp--;
    }
  }
  else
  {
    b_mant |= DOUBLE_IMPLICIT_BIT;
  }

  if (a_exp == 0)
  {
    if (a_mant == 0)
    {
      ur.u = make_double(result_sign, 0, 0); /* 0 / x = 0 */
      return ur.d;
    }
    a_exp = 1;
    while (!(a_mant & DOUBLE_IMPLICIT_BIT))
    {
      a_mant <<= 1;
      a_exp--;
    }
  }
  else
  {
    a_mant |= DOUBLE_IMPLICIT_BIT;
  }

  /* Calculate result exponent: ea - eb + bias */
  int result_exp = a_exp - b_exp + DOUBLE_EXP_BIAS;

  /* Both mantissas have their implicit bit at 52, so both are in [1.0, 2.0)
   * and the quotient is in [0.5, 2.0). */
  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient;

  /* Align so the quotient's leading bit is where the packer expects it: after
   * this, dividend is in [divisor, 2*divisor). */
  if (dividend < divisor)
  {
    dividend <<= 1;
    result_exp--;
  }

  /* Generate 53 significand bits plus SFP_GRS guard bits, and fold a non-zero
   * remainder into the sticky bit -- an inexact quotient must not round as if
   * it were exact.  After the alignment above the quotient's leading bit lands
   * at 52 + SFP_GRS, exactly where sfp_round_pack_double() expects it. */
  quotient = sfp_div_sig64(dividend, divisor);

  ur.u = sfp_round_pack_double(result_sign, result_exp, quotient);
  return ur.d;
}
