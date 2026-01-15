/*
 * Soft-float Common Helpers - Shared Utilities
 * IEEE 754 bit manipulation and helper functions
 * Used by all soft-float implementation files
 */

#ifndef SOFT_COMMON_H
#define SOFT_COMMON_H

#include "tcc_stdint.h"

/* ===== DOUBLE PRECISION (64-bit) ===== */

#define DOUBLE_SIGN_BIT (1ULL << 63)
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

/* Extract sign from double bits */
static inline int double_sign(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  return (v.w.hi >> 31) & 1;
}

/* Extract exponent from double bits */
static inline int double_exp(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  return (v.w.hi >> 20) & 0x7FF;
}

/* Extract mantissa from double bits */
static inline uint64_t double_mant(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  v.w.hi &= 0xFFFFF;
  return v.u;
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
  u64_words v;
  u64_words m;
  m.u = mant;
  v.w.lo = m.w.lo;
  v.w.hi = ((uint32_t)sign << 31) | ((uint32_t)exp << 20) | (m.w.hi & 0xFFFFF);
  return v.u;
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
  u64_words v;
  v.u = x;
  if (v.w.hi != 0)
    return clz32(v.w.hi);
  return 32 + clz32(v.w.lo);
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

#endif /* SOFT_COMMON_H */
