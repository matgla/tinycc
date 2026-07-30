/*
 * Soft-float Common Helpers - Shared Utilities
 * IEEE 754 bit manipulation and helper functions
 * Used by all soft-float implementation files
 */

#ifndef SOFT_COMMON_H
#define SOFT_COMMON_H

#include "tcc_stdint.h"

/* ===== DOUBLE PRECISION (64-bit) ===== */

#ifndef DOUBLE_SIGN_BIT
#define DOUBLE_SIGN_BIT (1ULL << 63)
#endif

#define DOUBLE_EXP_MASK 0x7FF0000000000000ULL
#define DOUBLE_MANT_MASK 0x000FFFFFFFFFFFFFULL
#define DOUBLE_EXP_BIAS 1023
#define DOUBLE_EXP_SHIFT 52
#define DOUBLE_IMPLICIT_BIT (1ULL << 52)

typedef union
{
  uint64_t u;
  struct
  {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    uint32_t hi;
    uint32_t lo;
#else
    uint32_t lo;
    uint32_t hi;
#endif
  } w;
} u64_words;

/* Extract sign from double bits.
 * NB: operate directly on the 64-bit value rather than via a u64_words union
 * local.  The armv8m cross/self-hosted codegen can drop the store of an
 * address-taken local across the function body (the parameter never reaches
 * the stack slot, so v.w.hi reads uninitialised memory), which silently
 * corrupts every soft-double operation.  Pure shifts avoid the address-taken
 * local entirely. */
static inline int double_sign(uint64_t bits)
{
  return (int)((bits >> 63) & 1);
}

/* Extract exponent from double bits */
static inline int double_exp(uint64_t bits)
{
  return (int)((bits >> 52) & 0x7FF);
}

/* Extract mantissa from double bits */
static inline uint64_t double_mant(uint64_t bits)
{
  return bits & DOUBLE_MANT_MASK;
}

/* Check if double bits represent NaN */
static inline int is_nan_bits(uint64_t bits)
{
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) != 0);
}

/* Check if double bits represent infinity */
static inline int is_inf_bits(uint64_t bits)
{
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) == 0);
}

/* Check if double bits represent zero (+0 or -0) */
static inline int is_zero_bits(uint64_t bits)
{
  return (double_exp(bits) == 0) && (double_mant(bits) == 0);
}

/* Build double from components */
static inline uint64_t make_double(int sign, int exp, uint64_t mant)
{
  /* Direct bit assembly (no address-taken union local — see double_sign). */
  return ((uint64_t)(sign & 1) << 63) | ((uint64_t)(exp & 0x7FF) << 52) | (mant & DOUBLE_MANT_MASK);
}

/* Count leading zeros in 32-bit value */
static inline int clz32(uint32_t x)
{
  int n = 0;
  if (x == 0)
    return 32;
  if ((x & 0xFFFF0000U) == 0)
  {
    n += 16;
    x <<= 16;
  }
  if ((x & 0xFF000000U) == 0)
  {
    n += 8;
    x <<= 8;
  }
  if ((x & 0xF0000000U) == 0)
  {
    n += 4;
    x <<= 4;
  }
  if ((x & 0xC0000000U) == 0)
  {
    n += 2;
    x <<= 2;
  }
  if ((x & 0x80000000U) == 0)
  {
    n += 1;
  }
  return n;
}

/* Count leading zeros in 64-bit value */
static inline int clz64(uint64_t x)
{
  uint32_t hi = (uint32_t)(x >> 32);
  if (hi != 0)
    return clz32(hi);
  return 32 + clz32((uint32_t)x);
}

/* ===== SINGLE PRECISION (32-bit) ===== */

#define FLOAT_SIGN_BIT (1U << 31)
#define FLOAT_EXP_MASK 0x7F800000U
#define FLOAT_MANT_MASK 0x007FFFFFU
#define FLOAT_EXP_BIAS 127
#define FLOAT_IMPLICIT_BIT (1U << 23)

/* Extract sign from float bits */
static inline int float_sign(uint32_t bits)
{
  return (bits >> 31) & 1;
}

/* Extract exponent from float bits */
static inline int float_exp(uint32_t bits)
{
  return (bits >> 23) & 0xFF;
}

/* Extract mantissa from float bits */
static inline uint32_t float_mant(uint32_t bits)
{
  return bits & FLOAT_MANT_MASK;
}

/* Check if float bits represent NaN */
static inline int is_nan_f(uint32_t bits)
{
  return (float_exp(bits) == 0xFF) && (float_mant(bits) != 0);
}

/* Check if float bits represent infinity */
static inline int is_inf_f(uint32_t bits)
{
  return (float_exp(bits) == 0xFF) && (float_mant(bits) == 0);
}

/* Check if float bits represent zero (+0 or -0) */
static inline int is_zero_f(uint32_t bits)
{
  return (float_exp(bits) == 0) && (float_mant(bits) == 0);
}

/* Build float from components */
static inline uint32_t make_float(int sign, int exp, uint32_t mant)
{
  return ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | (mant & FLOAT_MANT_MASK);
}

/* ===== ROUNDING CORE =====
 *
 * IEEE 754 requires round-to-nearest-ties-to-even and gradual underflow to
 * subnormals.  Getting either wrong is invisible in casual testing but shows up
 * immediately against bit-exact reference vectors (tests/fp/), so the logic
 * lives here once rather than being re-derived in add/sub/mul/div/convert.
 *
 * Callers carry SFP_GRS extra low-order bits below the significand: the top two
 * are guard and round, and the lowest is a *sticky* bit that must be OR-ed with
 * every bit ever shifted out.  Dropping those bits instead of accumulating them
 * is what makes a naive implementation truncate toward zero.
 */

#define SFP_GRS 3

/* Right-shift, folding everything shifted out into the sticky (bit 0).  A
 * plain >> would silently discard the information rounding depends on. */
static inline uint32_t sfp_shr_sticky32(uint32_t m, int shift)
{
  if (shift <= 0)
    return m;
  if (shift >= 32)
    return m != 0; /* all bits become sticky */
  return (m >> shift) | ((m & (((uint32_t)1 << shift) - 1)) != 0);
}

static inline uint64_t sfp_shr_sticky64(uint64_t m, int shift)
{
  if (shift <= 0)
    return m;
  if (shift >= 64)
    return m != 0;
  return (m >> shift) | ((m & (((uint64_t)1 << shift) - 1)) != 0);
}

/* Normalize, round and pack a float.
 *
 * `mant` holds the significand shifted left by SFP_GRS, so a normalized value
 * has its leading bit at FLOAT_NORM_BIT.  `exp` is the biased exponent for that
 * position.  Neither needs to be normalized on entry: this routine shifts the
 * leading bit into place, denormalizes when the exponent falls below 1, rounds
 * to nearest-even, and handles the carry a round-up can produce (including the
 * subnormal->normal and normal->infinity boundaries).
 */
#define FLOAT_NORM_BIT ((uint32_t)1 << (23 + SFP_GRS))

static inline uint32_t sfp_round_pack_float(int sign, int exp, uint32_t mant)
{
  uint32_t lsb, rem;

  if (mant == 0)
    return make_float(sign, 0, 0);

  /* Bring the leading bit to FLOAT_NORM_BIT. */
  while (mant >= (FLOAT_NORM_BIT << 1))
  {
    mant = sfp_shr_sticky32(mant, 1);
    exp++;
  }
  while (mant < FLOAT_NORM_BIT)
  {
    mant <<= 1;
    exp--;
  }

  /* Gradual underflow: shift into subnormal range *before* rounding, so the
   * rounding decision is made at the precision the result will actually have. */
  if (exp <= 0)
  {
    mant = sfp_shr_sticky32(mant, 1 - exp);
    exp = 0;
  }

  /* Round to nearest, ties to even. */
  lsb = (mant >> SFP_GRS) & 1;
  rem = mant & ((1u << SFP_GRS) - 1);
  if (rem > (1u << (SFP_GRS - 1)) || (rem == (1u << (SFP_GRS - 1)) && lsb))
    mant += (1u << SFP_GRS);
  mant >>= SFP_GRS;

  /* A round-up can carry into the next binade -- and for a subnormal that is
   * exactly how it becomes the smallest normal. */
  if (exp == 0)
  {
    if (mant & FLOAT_IMPLICIT_BIT)
      exp = 1;
  }
  else if (mant & (FLOAT_IMPLICIT_BIT << 1))
  {
    mant >>= 1;
    exp++;
  }

  if (exp >= 0xFF)
    return make_float(sign, 0xFF, 0); /* overflow to infinity */

  return make_float(sign, exp, mant);
}

#define DOUBLE_NORM_BIT ((uint64_t)1 << (52 + SFP_GRS))

static inline uint64_t sfp_round_pack_double(int sign, int exp, uint64_t mant)
{
  uint64_t lsb, rem;

  if (mant == 0)
    return make_double(sign, 0, 0);

  while (mant >= (DOUBLE_NORM_BIT << 1))
  {
    mant = sfp_shr_sticky64(mant, 1);
    exp++;
  }
  while (mant < DOUBLE_NORM_BIT)
  {
    mant <<= 1;
    exp--;
  }

  if (exp <= 0)
  {
    mant = sfp_shr_sticky64(mant, 1 - exp);
    exp = 0;
  }

  lsb = (mant >> SFP_GRS) & 1;
  rem = mant & (((uint64_t)1 << SFP_GRS) - 1);
  if (rem > ((uint64_t)1 << (SFP_GRS - 1)) || (rem == ((uint64_t)1 << (SFP_GRS - 1)) && lsb))
    mant += ((uint64_t)1 << SFP_GRS);
  mant >>= SFP_GRS;

  if (exp == 0)
  {
    if (mant & DOUBLE_IMPLICIT_BIT)
      exp = 1;
  }
  else if (mant & (DOUBLE_IMPLICIT_BIT << 1))
  {
    mant >>= 1;
    exp++;
  }

  if (exp >= 0x7FF)
    return make_double(sign, 0x7FF, 0);

  return make_double(sign, exp, mant);
}

#endif /* SOFT_COMMON_H */
