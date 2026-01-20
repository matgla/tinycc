/*
 * Comprehensive host-side tests for all soft-float aeabi functions
 * Compile with: gcc -O2 -DHOST_TEST test_aeabi_all.c -o test_aeabi_all -lm && ./test_aeabi_all
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <float.h>

#ifdef HOST_TEST

/* ===== COMMON DEFINITIONS ===== */

#define DOUBLE_SIGN_BIT (1ULL << 63)
#define DOUBLE_EXP_MASK 0x7FF0000000000000ULL
#define DOUBLE_MANT_MASK 0x000FFFFFFFFFFFFFULL
#define DOUBLE_EXP_BIAS 1023
#define DOUBLE_EXP_SHIFT 52
#define DOUBLE_IMPLICIT_BIT (1ULL << 52)

#define FLOAT_SIGN_BIT (1U << 31)
#define FLOAT_EXP_MASK 0x7F800000U
#define FLOAT_MANT_MASK 0x007FFFFFU
#define FLOAT_EXP_BIAS 127
#define FLOAT_IMPLICIT_BIT (1U << 23)

typedef union {
  uint64_t u;
  struct {
    uint32_t lo;
    uint32_t hi;
  } w;
} u64_words;

/* Double helpers */
static inline int double_sign(uint64_t bits) {
  u64_words v; v.u = bits;
  return (v.w.hi >> 31) & 1;
}

static inline int double_exp(uint64_t bits) {
  u64_words v; v.u = bits;
  return (v.w.hi >> 20) & 0x7FF;
}

static inline uint64_t double_mant(uint64_t bits) {
  u64_words v; v.u = bits;
  v.w.hi &= 0xFFFFF;
  return v.u;
}

static inline int is_nan_bits(uint64_t bits) {
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) != 0);
}

static inline int is_inf_bits(uint64_t bits) {
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) == 0);
}

static inline int is_zero_bits(uint64_t bits) {
  return (double_exp(bits) == 0) && (double_mant(bits) == 0);
}

static inline uint64_t make_double(int sign, int exp, uint64_t mant) {
  u64_words v;
  u64_words m;
  m.u = mant;
  v.w.lo = m.w.lo;
  v.w.hi = ((uint32_t)sign << 31) | ((uint32_t)exp << 20) | (m.w.hi & 0xFFFFF);
  return v.u;
}

static inline int clz32(uint32_t x) {
  int n = 0;
  if (x == 0) return 32;
  if ((x & 0xFFFF0000U) == 0) { n += 16; x <<= 16; }
  if ((x & 0xFF000000U) == 0) { n += 8; x <<= 8; }
  if ((x & 0xF0000000U) == 0) { n += 4; x <<= 4; }
  if ((x & 0xC0000000U) == 0) { n += 2; x <<= 2; }
  if ((x & 0x80000000U) == 0) { n += 1; }
  return n;
}

static inline int clz64(uint64_t x) {
  u64_words v; v.u = x;
  if (v.w.hi != 0) return clz32(v.w.hi);
  return 32 + clz32(v.w.lo);
}

/* Float helpers */
static inline int float_sign(uint32_t bits) { return (bits >> 31) & 1; }
static inline int float_exp(uint32_t bits) { return (bits >> 23) & 0xFF; }
static inline uint32_t float_mant(uint32_t bits) { return bits & FLOAT_MANT_MASK; }
static inline int is_nan_f(uint32_t bits) { return (float_exp(bits) == 0xFF) && (float_mant(bits) != 0); }
static inline int is_inf_f(uint32_t bits) { return (float_exp(bits) == 0xFF) && (float_mant(bits) == 0); }
static inline int is_zero_f(uint32_t bits) { return (float_exp(bits) == 0) && (float_mant(bits) == 0); }
static inline uint32_t make_float(int sign, int exp, uint32_t mant) {
  return ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | (mant & FLOAT_MANT_MASK);
}

/* ===== 64-bit multiply helpers for dmul ===== */
static inline uint32_t add32_c(uint32_t a, uint32_t b, uint32_t cin, uint32_t *cout) {
  uint32_t s = a + b;
  uint32_t c = (s < a);
  uint32_t s2 = s + cin;
  c |= (s2 < s);
  *cout = c;
  return s2;
}

static inline void add64_shift32(uint32_t *w1, uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi) {
  uint32_t c;
  *w1 = add32_c(*w1, lo, 0, &c);
  *w2 = add32_c(*w2, hi, c, &c);
  *w3 = add32_c(*w3, 0, c, &c);
}

static inline void add64_shift64(uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi) {
  uint32_t c;
  *w2 = add32_c(*w2, lo, 0, &c);
  *w3 = add32_c(*w3, hi, c, &c);
}

static inline void mul32wide_u32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi) {
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

static inline void mul64wide(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  uint32_t a0 = (uint32_t)a;
  uint32_t a1 = (uint32_t)(a >> 32);
  uint32_t b0 = (uint32_t)b;
  uint32_t b1 = (uint32_t)(b >> 32);
  uint32_t p0_lo, p0_hi, p1_lo, p1_hi, p2_lo, p2_hi, p3_lo, p3_hi;
  mul32wide_u32(a0, b0, &p0_lo, &p0_hi);
  mul32wide_u32(a0, b1, &p1_lo, &p1_hi);
  mul32wide_u32(a1, b0, &p2_lo, &p2_hi);
  mul32wide_u32(a1, b1, &p3_lo, &p3_hi);
  uint32_t w0 = p0_lo, w1 = p0_hi, w2 = 0, w3 = 0;
  add64_shift32(&w1, &w2, &w3, p1_lo, p1_hi);
  add64_shift32(&w1, &w2, &w3, p2_lo, p2_hi);
  add64_shift64(&w2, &w3, p3_lo, p3_hi);
  *lo = ((uint64_t)w1 << 32) | (uint64_t)w0;
  *hi = ((uint64_t)w3 << 32) | (uint64_t)w2;
}

/* ===== DOUBLE PRECISION IMPLEMENTATIONS ===== */

double __aeabi_dadd(double a, double b) {
  union { double d; uint64_t u; } ua, ub, ur;
  ua.d = a; ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = double_sign(a_bits), b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits), b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits), b_mant = double_mant(b_bits);

  if (is_nan_bits(a_bits)) { ur.u = a_bits; return ur.d; }
  if (is_nan_bits(b_bits)) { ur.u = b_bits; return ur.d; }
  if (is_inf_bits(a_bits)) {
    if (is_inf_bits(b_bits) && (a_sign != b_sign)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = a_bits; return ur.d;
  }
  if (is_inf_bits(b_bits)) { ur.u = b_bits; return ur.d; }
  if (is_zero_bits(a_bits)) { ur.u = b_bits; return ur.d; }
  if (is_zero_bits(b_bits)) { ur.u = a_bits; return ur.d; }

  if (a_exp != 0) a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= DOUBLE_IMPLICIT_BIT;

  int exp_diff = a_exp - b_exp;
  int result_exp;
  uint64_t result_mant;
  int result_sign;

  if (exp_diff > 0) {
    if (exp_diff < 64) b_mant >>= exp_diff; else b_mant = 0;
    result_exp = a_exp;
  } else if (exp_diff < 0) {
    if (-exp_diff < 64) a_mant >>= -exp_diff; else a_mant = 0;
    result_exp = b_exp;
  } else {
    result_exp = a_exp;
  }

  if (a_sign == b_sign) {
    result_mant = a_mant + b_mant;
    result_sign = a_sign;
    if (result_mant & (DOUBLE_IMPLICIT_BIT << 1)) { result_mant >>= 1; result_exp++; }
  } else {
    if (a_mant >= b_mant) { result_mant = a_mant - b_mant; result_sign = a_sign; }
    else { result_mant = b_mant - a_mant; result_sign = b_sign; }
    if (result_mant == 0) { ur.u = 0; return ur.d; }
    while (!(result_mant & DOUBLE_IMPLICIT_BIT) && result_exp > 0) { result_mant <<= 1; result_exp--; }
  }

  if (result_exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
  if (result_exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }

  result_mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}

double __aeabi_dsub(double a, double b) {
  union { double d; uint64_t u; } ub; ub.d = b;
  ub.u ^= DOUBLE_SIGN_BIT;
  return __aeabi_dadd(a, ub.d);
}

double __aeabi_dneg(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  ua.u ^= DOUBLE_SIGN_BIT;
  return ua.d;
}

double __aeabi_dmul(double a, double b) {
  union { double d; uint64_t u; } ua, ub, ur;
  ua.d = a; ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = double_sign(a_bits), b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits), b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits), b_mant = double_mant(b_bits);
  int result_sign = a_sign ^ b_sign;

  if (is_nan_bits(a_bits)) { ur.u = a_bits; return ur.d; }
  if (is_nan_bits(b_bits)) { ur.u = b_bits; return ur.d; }
  if (is_inf_bits(a_bits)) {
    if (is_zero_bits(b_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0); return ur.d;
  }
  if (is_inf_bits(b_bits)) {
    if (is_zero_bits(a_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0); return ur.d;
  }
  if (is_zero_bits(a_bits) || is_zero_bits(b_bits)) {
    ur.u = make_double(result_sign, 0, 0); return ur.d;
  }

  if (a_exp != 0 && b_exp != 0) {
    if (a_mant == 0) {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
      if (exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }
      ur.u = make_double(result_sign, exp, b_mant); return ur.d;
    }
    if (b_mant == 0) {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
      if (exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }
      ur.u = make_double(result_sign, exp, a_mant); return ur.d;
    }
  }

  if (a_exp != 0) a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= DOUBLE_IMPLICIT_BIT;
  int result_exp = a_exp + b_exp - DOUBLE_EXP_BIAS;

  uint64_t prod_hi, prod_lo;
  mul64wide(a_mant, b_mant, &prod_hi, &prod_lo);

  const uint64_t bit105_mask = 1ULL << (105 - 64);
  int shift = 52;
  if (prod_hi & bit105_mask) { shift = 53; result_exp++; }

  const uint32_t prod_lo_lo = (uint32_t)prod_lo;
  const uint32_t prod_lo_hi = (uint32_t)(prod_lo >> 32);
  const uint32_t prod_hi_lo = (uint32_t)prod_hi;
  const uint32_t prod_hi_hi = (uint32_t)(prod_hi >> 32);

  uint32_t mant_lo32, mant_hi32;
  int guard, sticky;
  if (shift == 52) {
    mant_lo32 = (prod_hi_lo << 12) | (prod_lo_hi >> 20);
    mant_hi32 = (prod_hi_hi << 12) | (prod_hi_lo >> 20);
    guard = (int)((prod_lo_hi >> 19) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 19) - 1u)) != 0);
  } else {
    mant_lo32 = (prod_hi_lo << 11) | (prod_lo_hi >> 21);
    mant_hi32 = (prod_hi_hi << 11) | (prod_hi_lo >> 21);
    guard = (int)((prod_lo_hi >> 20) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 20) - 1u)) != 0);
  }

  uint64_t mant = ((uint64_t)mant_hi32 << 32) | (uint64_t)mant_lo32;
  if (guard && (sticky || (mant & 1ULL))) mant++;
  if (mant & (DOUBLE_IMPLICIT_BIT << 1)) { mant >>= 1; result_exp++; }
  if (result_exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
  if (result_exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }

  mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, mant);
  return ur.d;
}

double __aeabi_ddiv(double a, double b) {
  union { double d; uint64_t u; } ua, ub, ur;
  ua.d = a; ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = double_sign(a_bits), b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits), b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits), b_mant = double_mant(b_bits);
  int result_sign = a_sign ^ b_sign;

  if (is_nan_bits(a_bits)) { ur.u = a_bits; return ur.d; }
  if (is_nan_bits(b_bits)) { ur.u = b_bits; return ur.d; }
  if (is_inf_bits(a_bits)) {
    if (is_inf_bits(b_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0); return ur.d;
  }
  if (is_inf_bits(b_bits)) { ur.u = make_double(result_sign, 0, 0); return ur.d; }
  if (is_zero_bits(b_bits)) {
    if (is_zero_bits(a_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0); return ur.d;
  }
  if (is_zero_bits(a_bits)) { ur.u = make_double(result_sign, 0, 0); return ur.d; }

  if (a_exp != 0) a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= DOUBLE_IMPLICIT_BIT;
  int result_exp = a_exp - b_exp + DOUBLE_EXP_BIAS;

  uint64_t dividend = a_mant, divisor = b_mant, quotient = 0;
  if (dividend < divisor) { dividend <<= 1; result_exp--; }

  for (int i = 0; i < 54; i++) {
    quotient <<= 1;
    if (!(dividend < divisor)) { dividend -= divisor; quotient |= 1; }
    dividend <<= 1;
  }

  uint64_t guard = quotient & 1;
  quotient >>= 1;
  if (guard && dividend) quotient++;

  while (!(quotient < (DOUBLE_IMPLICIT_BIT << 1))) { quotient >>= 1; result_exp++; }
  while (quotient && !(quotient & DOUBLE_IMPLICIT_BIT)) { quotient <<= 1; result_exp--; }

  if (result_exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
  if (result_exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }

  uint64_t result_mant = quotient & DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}

/* Double comparisons */
static int dcmp_core(double a, double b) {
  union { double d; uint64_t u; } ua, ub;
  ua.d = a; ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;
  if (is_nan_bits(a_bits) || is_nan_bits(b_bits)) return 2;
  if (is_zero_bits(a_bits) && is_zero_bits(b_bits)) return 0;
  int a_sign = double_sign(a_bits), b_sign = double_sign(b_bits);
  if (a_sign != b_sign) return a_sign ? -1 : 1;
  uint64_t a_mag = a_bits & ~DOUBLE_SIGN_BIT, b_mag = b_bits & ~DOUBLE_SIGN_BIT;
  if (a_mag == b_mag) return 0;
  int mag_cmp = (a_mag > b_mag) ? 1 : -1;
  return a_sign ? -mag_cmp : mag_cmp;
}

int __aeabi_dcmpeq(double a, double b) { return dcmp_core(a, b) == 0 ? 1 : 0; }
int __aeabi_dcmplt(double a, double b) { return dcmp_core(a, b) == -1 ? 1 : 0; }
int __aeabi_dcmple(double a, double b) { int r = dcmp_core(a, b); return (r == -1 || r == 0) ? 1 : 0; }
int __aeabi_dcmpgt(double a, double b) { return dcmp_core(a, b) == 1 ? 1 : 0; }
int __aeabi_dcmpge(double a, double b) { int r = dcmp_core(a, b); return (r == 1 || r == 0) ? 1 : 0; }
int __aeabi_dcmpun(double a, double b) { return dcmp_core(a, b) == 2 ? 1 : 0; }

/* Double conversions */
double __aeabi_i2d(int a) {
  union { double d; uint64_t u; } ur;
  if (a == 0) { ur.u = 0; return ur.d; }
  int sign = 0;
  uint32_t abs_a;
  if (a < 0) { sign = 1; abs_a = (uint32_t)(-a); } else { abs_a = (uint32_t)a; }
  int leading_zeros = clz32(abs_a);
  int msb_pos = 31 - leading_zeros;
  int exp = DOUBLE_EXP_BIAS + msb_pos;
  uint64_t mant = ((uint64_t)abs_a << (52 - msb_pos)) & DOUBLE_MANT_MASK;
  ur.u = make_double(sign, exp, mant);
  return ur.d;
}

double __aeabi_ui2d(unsigned int a) {
  union { double d; uint64_t u; } ur;
  if (a == 0) { ur.u = 0; return ur.d; }
  int leading_zeros = clz32(a);
  int msb_pos = 31 - leading_zeros;
  int exp = DOUBLE_EXP_BIAS + msb_pos;
  uint64_t mant = ((uint64_t)a << (52 - msb_pos)) & DOUBLE_MANT_MASK;
  ur.u = make_double(0, exp, mant);
  return ur.d;
}

int __aeabi_d2iz(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  uint64_t bits = ua.u;
  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);
  if (exp == 0x7FF) return 0;
  if (exp == 0) return 0;
  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 31) return sign ? (int)0x80000000U : 0x7FFFFFFF;
  int shift = actual_exp - 52;
  uint32_t result;
  if (shift >= 0) result = (uint32_t)(mant << shift); else result = (uint32_t)(mant >> (-shift));
  return sign ? -(int)result : (int)result;
}

unsigned int __aeabi_d2uiz(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  uint64_t bits = ua.u;
  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);
  if (sign) return 0;
  if (exp == 0x7FF) return 0;
  if (exp == 0) return 0;
  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 32) return 0xFFFFFFFFU;
  int shift = actual_exp - 52;
  if (shift >= 0) return (uint32_t)(mant << shift);
  return (uint32_t)(mant >> (-shift));
}

long long __aeabi_d2lz(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  uint64_t bits = ua.u;
  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);
  if (exp == 0x7FF) return 0;
  if (exp == 0) return 0;
  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 63) return sign ? (long long)0x8000000000000000ULL : (long long)0x7FFFFFFFFFFFFFFFULL;
  int shift = actual_exp - 52;
  unsigned long long magnitude;
  if (shift >= 0) magnitude = (unsigned long long)(mant << shift); else magnitude = (unsigned long long)(mant >> (-shift));
  return sign ? -(long long)magnitude : (long long)magnitude;
}

unsigned long long __aeabi_d2ulz(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  uint64_t bits = ua.u;
  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);
  if (sign) return 0;
  if (exp == 0x7FF) return 0;
  if (exp == 0) return 0;
  mant |= DOUBLE_IMPLICIT_BIT;
  int actual_exp = exp - DOUBLE_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 64) return ~0ULL;
  int shift = actual_exp - 52;
  if (shift >= 0) return (unsigned long long)(mant << shift);
  return (unsigned long long)(mant >> (-shift));
}

/* ===== SINGLE PRECISION IMPLEMENTATIONS ===== */

float __aeabi_fadd(float a, float b) {
  union { float f; uint32_t u; } ua = {.f = a}, ub = {.f = b}, ur;
  uint32_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = float_sign(a_bits), b_sign = float_sign(b_bits);
  int a_exp = float_exp(a_bits), b_exp = float_exp(b_bits);
  uint32_t a_mant = float_mant(a_bits), b_mant = float_mant(b_bits);

  if (is_nan_f(a_bits)) { ur.u = a_bits; return ur.f; }
  if (is_nan_f(b_bits)) { ur.u = b_bits; return ur.f; }
  if (is_inf_f(a_bits)) {
    if (is_inf_f(b_bits) && (a_sign != b_sign)) { ur.u = 0x7FC00000U; return ur.f; }
    ur.u = a_bits; return ur.f;
  }
  if (is_inf_f(b_bits)) { ur.u = b_bits; return ur.f; }
  if (is_zero_f(a_bits)) { ur.u = b_bits; return ur.f; }
  if (is_zero_f(b_bits)) { ur.u = a_bits; return ur.f; }

  if (a_exp != 0) a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= FLOAT_IMPLICIT_BIT;

  int exp_diff = a_exp - b_exp;
  int result_exp;
  uint32_t result_mant;
  int result_sign;

  if (exp_diff > 0) {
    if (exp_diff < 32) b_mant >>= exp_diff; else b_mant = 0;
    result_exp = a_exp;
  } else if (exp_diff < 0) {
    if (-exp_diff < 32) a_mant >>= -exp_diff; else a_mant = 0;
    result_exp = b_exp;
  } else {
    result_exp = a_exp;
  }

  if (a_sign == b_sign) {
    result_mant = a_mant + b_mant;
    result_sign = a_sign;
    if (result_mant & (FLOAT_IMPLICIT_BIT << 1)) { result_mant >>= 1; result_exp++; }
  } else {
    if (a_mant >= b_mant) { result_mant = a_mant - b_mant; result_sign = a_sign; }
    else { result_mant = b_mant - a_mant; result_sign = b_sign; }
    if (result_mant == 0) { ur.u = 0; return ur.f; }
    while (!(result_mant & FLOAT_IMPLICIT_BIT) && result_exp > 0) { result_mant <<= 1; result_exp--; }
  }

  if (result_exp >= 0xFF) { ur.u = make_float(result_sign, 0xFF, 0); return ur.f; }
  if (result_exp <= 0) { ur.u = make_float(result_sign, 0, 0); return ur.f; }

  result_mant &= FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}

float __aeabi_fsub(float a, float b) {
  union { float f; uint32_t u; } ub = {.f = b};
  ub.u ^= FLOAT_SIGN_BIT;
  return __aeabi_fadd(a, ub.f);
}

float __aeabi_fmul(float a, float b) {
  union { float f; uint32_t u; } ua = {.f = a}, ub = {.f = b}, ur;
  uint32_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = float_sign(a_bits), b_sign = float_sign(b_bits);
  int a_exp = float_exp(a_bits), b_exp = float_exp(b_bits);
  uint32_t a_mant = float_mant(a_bits), b_mant = float_mant(b_bits);
  int result_sign = a_sign ^ b_sign;

  if (is_nan_f(a_bits)) { ur.u = a_bits; return ur.f; }
  if (is_nan_f(b_bits)) { ur.u = b_bits; return ur.f; }
  if (is_inf_f(a_bits)) {
    if (is_zero_f(b_bits)) { ur.u = 0x7FC00000U; return ur.f; }
    ur.u = make_float(result_sign, 0xFF, 0); return ur.f;
  }
  if (is_inf_f(b_bits)) {
    if (is_zero_f(a_bits)) { ur.u = 0x7FC00000U; return ur.f; }
    ur.u = make_float(result_sign, 0xFF, 0); return ur.f;
  }
  if (is_zero_f(a_bits) || is_zero_f(b_bits)) { ur.u = make_float(result_sign, 0, 0); return ur.f; }

  if (a_exp != 0) a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= FLOAT_IMPLICIT_BIT;

  int result_exp = a_exp + b_exp - FLOAT_EXP_BIAS;
  uint64_t product = (uint64_t)a_mant * (uint64_t)b_mant;
  if (product & (1ULL << 47)) { product >>= 1; result_exp++; }
  uint32_t result_mant = (uint32_t)(product >> 23);

  if (result_exp >= 0xFF) { ur.u = make_float(result_sign, 0xFF, 0); return ur.f; }
  if (result_exp <= 0) { ur.u = make_float(result_sign, 0, 0); return ur.f; }

  result_mant &= FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}

float __aeabi_fdiv(float a, float b) {
  union { float f; uint32_t u; } ua = {.f = a}, ub = {.f = b}, ur;
  uint32_t a_bits = ua.u, b_bits = ub.u;
  int a_sign = float_sign(a_bits), b_sign = float_sign(b_bits);
  int a_exp = float_exp(a_bits), b_exp = float_exp(b_bits);
  uint32_t a_mant = float_mant(a_bits), b_mant = float_mant(b_bits);
  int result_sign = a_sign ^ b_sign;

  if (is_nan_f(a_bits)) { ur.u = a_bits; return ur.f; }
  if (is_nan_f(b_bits)) { ur.u = b_bits; return ur.f; }
  if (is_inf_f(a_bits)) {
    if (is_inf_f(b_bits)) { ur.u = 0x7FC00000U; return ur.f; }
    ur.u = make_float(result_sign, 0xFF, 0); return ur.f;
  }
  if (is_inf_f(b_bits)) { ur.u = make_float(result_sign, 0, 0); return ur.f; }
  if (is_zero_f(b_bits)) {
    if (is_zero_f(a_bits)) { ur.u = 0x7FC00000U; return ur.f; }
    ur.u = make_float(result_sign, 0xFF, 0); return ur.f;
  }
  if (is_zero_f(a_bits)) { ur.u = make_float(result_sign, 0, 0); return ur.f; }

  if (a_exp != 0) a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= FLOAT_IMPLICIT_BIT;

  int result_exp = a_exp - b_exp + FLOAT_EXP_BIAS;

  /* Use same algorithm as ddiv: restoring division */
  uint32_t dividend = a_mant;
  uint32_t divisor = b_mant;
  uint32_t quotient = 0;

  if (dividend < divisor) { dividend <<= 1; result_exp--; }

  for (int i = 0; i < 25; i++) {
    quotient <<= 1;
    if (dividend >= divisor) { dividend -= divisor; quotient |= 1; }
    dividend <<= 1;
  }

  uint32_t guard = quotient & 1;
  quotient >>= 1;
  if (guard && dividend) quotient++;

  while (quotient >= (FLOAT_IMPLICIT_BIT << 1)) { quotient >>= 1; result_exp++; }
  while (quotient && !(quotient & FLOAT_IMPLICIT_BIT)) { quotient <<= 1; result_exp--; }

  if (result_exp >= 0xFF) { ur.u = make_float(result_sign, 0xFF, 0); return ur.f; }
  if (result_exp <= 0) { ur.u = make_float(result_sign, 0, 0); return ur.f; }

  uint32_t result_mant = quotient & FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}

/* Float comparisons */
static int fcmp_core(float a, float b) {
  union { float f; uint32_t u; } ua = {.f = a}, ub = {.f = b};
  uint32_t a_bits = ua.u, b_bits = ub.u;
  if (is_nan_f(a_bits) || is_nan_f(b_bits)) return 2;
  if (is_zero_f(a_bits) && is_zero_f(b_bits)) return 0;
  int a_sign = float_sign(a_bits), b_sign = float_sign(b_bits);
  if (a_sign != b_sign) return a_sign ? -1 : 1;
  uint32_t a_mag = a_bits & ~FLOAT_SIGN_BIT, b_mag = b_bits & ~FLOAT_SIGN_BIT;
  if (a_mag == b_mag) return 0;
  int mag_cmp = (a_mag > b_mag) ? 1 : -1;
  return a_sign ? -mag_cmp : mag_cmp;
}

int __aeabi_fcmpeq(float a, float b) { return fcmp_core(a, b) == 0 ? 1 : 0; }
int __aeabi_fcmplt(float a, float b) { return fcmp_core(a, b) == -1 ? 1 : 0; }
int __aeabi_fcmple(float a, float b) { int r = fcmp_core(a, b); return (r == -1 || r == 0) ? 1 : 0; }
int __aeabi_fcmpgt(float a, float b) { return fcmp_core(a, b) == 1 ? 1 : 0; }
int __aeabi_fcmpge(float a, float b) { int r = fcmp_core(a, b); return (r == 1 || r == 0) ? 1 : 0; }
int __aeabi_fcmpun(float a, float b) { return fcmp_core(a, b) == 2 ? 1 : 0; }

/* Float conversions */
int __aeabi_f2iz(float a) {
  union { float f; uint32_t u; } ua = {.f = a};
  uint32_t bits = ua.u;
  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);
  if (exp == 0xFF) return 0;
  if (exp == 0) return 0;
  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 31) return sign ? (int)0x80000000U : 0x7FFFFFFF;
  int shift = actual_exp - 23;
  uint32_t result;
  if (shift >= 0) result = mant << shift; else result = mant >> (-shift);
  return sign ? -(int)result : (int)result;
}

unsigned int __aeabi_f2uiz(float a) {
  union { float f; uint32_t u; } ua = {.f = a};
  uint32_t bits = ua.u;
  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);
  if (sign) return 0;
  if (exp == 0xFF) return 0;
  if (exp == 0) return 0;
  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 32) return 0xFFFFFFFFU;
  int shift = actual_exp - 23;
  if (shift >= 0) return mant << shift;
  return mant >> (-shift);
}

long long __aeabi_f2lz(float a) {
  union { float f; uint32_t u; } ua = {.f = a};
  uint32_t bits = ua.u;
  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);
  if (exp == 0xFF) return 0;
  if (exp == 0) return 0;
  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 63) return sign ? (long long)0x8000000000000000ULL : (long long)0x7FFFFFFFFFFFFFFFULL;
  int shift = actual_exp - 23;
  unsigned long long magnitude;
  if (shift >= 0) magnitude = (unsigned long long)mant << shift; else magnitude = (unsigned long long)mant >> (-shift);
  return sign ? -(long long)magnitude : (long long)magnitude;
}

unsigned long long __aeabi_f2ulz(float a) {
  union { float f; uint32_t u; } ua = {.f = a};
  uint32_t bits = ua.u;
  int sign = float_sign(bits);
  int exp = float_exp(bits);
  uint32_t mant = float_mant(bits);
  if (sign) return 0;
  if (exp == 0xFF) return 0;
  if (exp == 0) return 0;
  mant |= FLOAT_IMPLICIT_BIT;
  int actual_exp = exp - FLOAT_EXP_BIAS;
  if (actual_exp < 0) return 0;
  if (actual_exp >= 64) return ~0ULL;
  int shift = actual_exp - 23;
  if (shift >= 0) return (unsigned long long)mant << shift;
  return (unsigned long long)mant >> (-shift);
}

float __aeabi_i2f(int a) {
  union { float f; uint32_t u; } ur;
  if (a == 0) { ur.u = 0; return ur.f; }
  int sign = 0;
  uint32_t abs_a;
  if (a < 0) { sign = 1; abs_a = (uint32_t)(-a); } else { abs_a = (uint32_t)a; }
  int leading_zeros = clz32(abs_a);
  int msb_pos = 31 - leading_zeros;
  int exp = FLOAT_EXP_BIAS + msb_pos;
  uint32_t mant;
  if (msb_pos > 23) mant = abs_a >> (msb_pos - 23); else mant = abs_a << (23 - msb_pos);
  mant &= FLOAT_MANT_MASK;
  ur.u = ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | mant;
  return ur.f;
}

float __aeabi_ui2f(unsigned int a) {
  union { float f; uint32_t u; } ur;
  if (a == 0) { ur.u = 0; return ur.f; }
  int leading_zeros = clz32(a);
  int msb_pos = 31 - leading_zeros;
  int exp = FLOAT_EXP_BIAS + msb_pos;
  uint32_t mant;
  if (msb_pos > 23) mant = a >> (msb_pos - 23); else mant = a << (23 - msb_pos);
  mant &= FLOAT_MANT_MASK;
  ur.u = ((uint32_t)exp << 23) | mant;
  return ur.f;
}

/* Float <-> Double conversions */
double __aeabi_f2d(float a) {
  union { float f; uint32_t u; } ua = {.f = a};
  union { double d; uint64_t u; } ur;
  uint32_t bits = ua.u;
  int sign = (bits >> 31) & 1;
  int exp = (bits >> 23) & 0xFF;
  uint32_t mant = bits & FLOAT_MANT_MASK;

  if (exp == 0xFF) {
    ur.u = ((uint64_t)sign << 63) | 0x7FF0000000000000ULL | ((uint64_t)mant << 29);
    return ur.d;
  }
  if (exp == 0 && mant == 0) {
    ur.u = (uint64_t)sign << 63;
    return ur.d;
  }
  int new_exp = exp - FLOAT_EXP_BIAS + DOUBLE_EXP_BIAS;
  ur.u = ((uint64_t)sign << 63) | ((uint64_t)new_exp << 52) | ((uint64_t)mant << 29);
  return ur.d;
}

float __aeabi_d2f(double a) {
  union { double d; uint64_t u; } ua; ua.d = a;
  union { float f; uint32_t u; } ur;
  uint64_t bits = ua.u;
  int sign = double_sign(bits);
  int exp = double_exp(bits);
  uint64_t mant = double_mant(bits);

  if (exp == 0x7FF) {
    ur.u = ((uint32_t)sign << 31) | 0x7F800000U | ((uint32_t)(mant >> 29) & FLOAT_MANT_MASK);
    return ur.f;
  }
  if (exp == 0 && mant == 0) {
    ur.u = (uint32_t)sign << 31;
    return ur.f;
  }
  int new_exp = exp - DOUBLE_EXP_BIAS + FLOAT_EXP_BIAS;
  if (new_exp >= 0xFF) { ur.u = ((uint32_t)sign << 31) | 0x7F800000U; return ur.f; }
  if (new_exp <= 0) { ur.u = (uint32_t)sign << 31; return ur.f; }

  /* Round to nearest, ties to even */
  uint32_t new_mant = (uint32_t)(mant >> 29);
  uint32_t guard = (mant >> 28) & 1;
  uint32_t sticky = (mant & ((1ULL << 28) - 1)) != 0;

  if (guard && (sticky || (new_mant & 1))) {
    new_mant++;
    if (new_mant >= (1U << 23)) {
      new_mant >>= 1;
      new_exp++;
      if (new_exp >= 0xFF) { ur.u = ((uint32_t)sign << 31) | 0x7F800000U; return ur.f; }
    }
  }

  ur.u = ((uint32_t)sign << 31) | ((uint32_t)new_exp << 23) | (new_mant & FLOAT_MANT_MASK);
  return ur.f;
}

#endif /* HOST_TEST */

/* ===== TEST FRAMEWORK ===== */

typedef union { double d; uint64_t u; } dbl_u;
typedef union { float f; uint32_t u; } flt_u;

static int test_count = 0;
static int fail_count = 0;

#define TEST_D_OP(name, op, a_val, b_val) do { \
    dbl_u a, b, got, exp; \
    a.d = a_val; b.d = b_val; \
    got.d = __aeabi_##name(a.d, b.d); \
    exp.d = a.d op b.d; \
    test_count++; \
    if (got.u != exp.u && !(isnan(got.d) && isnan(exp.d))) { \
        printf("FAIL d" #name "(%g, %g): got=0x%016llX (%g) exp=0x%016llX (%g)\n", \
               a.d, b.d, (unsigned long long)got.u, got.d, \
               (unsigned long long)exp.u, exp.d); \
        fail_count++; \
    } \
} while(0)

#define TEST_F_OP(name, op, a_val, b_val) do { \
    flt_u a, b, got, exp; \
    a.f = a_val; b.f = b_val; \
    got.f = __aeabi_##name(a.f, b.f); \
    exp.f = a.f op b.f; \
    test_count++; \
    if (got.u != exp.u && !(isnanf(got.f) && isnanf(exp.f))) { \
        printf("FAIL f" #name "(%g, %g): got=0x%08X (%g) exp=0x%08X (%g)\n", \
               (double)a.f, (double)b.f, got.u, (double)got.f, exp.u, (double)exp.f); \
        fail_count++; \
    } \
} while(0)

#define TEST_D_CMP(name, op, a_val, b_val) do { \
    double a = a_val, b = b_val; \
    int got = __aeabi_##name(a, b); \
    int exp = (a op b) ? 1 : 0; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL d" #name "(%g, %g): got=%d exp=%d\n", a, b, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_F_CMP(name, op, a_val, b_val) do { \
    float a = a_val, b = b_val; \
    int got = __aeabi_##name(a, b); \
    int exp = (a op b) ? 1 : 0; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL f" #name "(%g, %g): got=%d exp=%d\n", (double)a, (double)b, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_D2I(a_val) do { \
    double a = a_val; \
    int got = __aeabi_d2iz(a); \
    int exp = (int)a; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL d2iz(%g): got=%d exp=%d\n", a, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_D2UI(a_val) do { \
    double a = a_val; \
    unsigned int got = __aeabi_d2uiz(a); \
    unsigned int exp = (unsigned int)a; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL d2uiz(%g): got=%u exp=%u\n", a, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_I2D(a_val) do { \
    int a = a_val; \
    dbl_u got, exp; \
    got.d = __aeabi_i2d(a); \
    exp.d = (double)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL i2d(%d): got=0x%016llX (%g) exp=0x%016llX (%g)\n", \
               a, (unsigned long long)got.u, got.d, (unsigned long long)exp.u, exp.d); \
        fail_count++; \
    } \
} while(0)

#define TEST_UI2D(a_val) do { \
    unsigned int a = a_val; \
    dbl_u got, exp; \
    got.d = __aeabi_ui2d(a); \
    exp.d = (double)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL ui2d(%u): got=0x%016llX (%g) exp=0x%016llX (%g)\n", \
               a, (unsigned long long)got.u, got.d, (unsigned long long)exp.u, exp.d); \
        fail_count++; \
    } \
} while(0)

#define TEST_F2I(a_val) do { \
    float a = a_val; \
    int got = __aeabi_f2iz(a); \
    int exp = (int)a; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL f2iz(%g): got=%d exp=%d\n", (double)a, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_F2UI(a_val) do { \
    float a = a_val; \
    unsigned int got = __aeabi_f2uiz(a); \
    unsigned int exp = (unsigned int)a; \
    test_count++; \
    if (got != exp) { \
        printf("FAIL f2uiz(%g): got=%u exp=%u\n", (double)a, got, exp); \
        fail_count++; \
    } \
} while(0)

#define TEST_I2F(a_val) do { \
    int a = a_val; \
    flt_u got, exp; \
    got.f = __aeabi_i2f(a); \
    exp.f = (float)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL i2f(%d): got=0x%08X (%g) exp=0x%08X (%g)\n", \
               a, got.u, (double)got.f, exp.u, (double)exp.f); \
        fail_count++; \
    } \
} while(0)

#define TEST_UI2F(a_val) do { \
    unsigned int a = a_val; \
    flt_u got, exp; \
    got.f = __aeabi_ui2f(a); \
    exp.f = (float)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL ui2f(%u): got=0x%08X (%g) exp=0x%08X (%g)\n", \
               a, got.u, (double)got.f, exp.u, (double)exp.f); \
        fail_count++; \
    } \
} while(0)

#define TEST_F2D(a_val) do { \
    float a = a_val; \
    dbl_u got, exp; \
    got.d = __aeabi_f2d(a); \
    exp.d = (double)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL f2d(%g): got=0x%016llX (%g) exp=0x%016llX (%g)\n", \
               (double)a, (unsigned long long)got.u, got.d, (unsigned long long)exp.u, exp.d); \
        fail_count++; \
    } \
} while(0)

#define TEST_D2F(a_val) do { \
    double a = a_val; \
    flt_u got, exp; \
    got.f = __aeabi_d2f(a); \
    exp.f = (float)a; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL d2f(%g): got=0x%08X (%g) exp=0x%08X (%g)\n", \
               a, got.u, (double)got.f, exp.u, (double)exp.f); \
        fail_count++; \
    } \
} while(0)

static void test_double_arithmetic(void) {
    printf("--- Double Arithmetic ---\n");

    /* Addition */
    TEST_D_OP(dadd, +, 1.5, 2.0);
    TEST_D_OP(dadd, +, -1.5, 2.0);
    TEST_D_OP(dadd, +, 1.5, -2.0);
    TEST_D_OP(dadd, +, -1.5, -2.0);
    TEST_D_OP(dadd, +, 1e10, 1e-10);
    TEST_D_OP(dadd, +, 0.0, 5.0);
    TEST_D_OP(dadd, +, 5.0, 0.0);
    TEST_D_OP(dadd, +, 1.0, 1.0);
    TEST_D_OP(dadd, +, 1e308, 1e308);

    /* Subtraction */
    TEST_D_OP(dsub, -, 5.0, 3.0);
    TEST_D_OP(dsub, -, 3.0, 5.0);
    TEST_D_OP(dsub, -, -5.0, 3.0);
    TEST_D_OP(dsub, -, 5.0, -3.0);
    TEST_D_OP(dsub, -, 1.0, 1.0);
    TEST_D_OP(dsub, -, 1e10, 1e10);

    /* Multiplication */
    TEST_D_OP(dmul, *, 1.5, 2.0);
    TEST_D_OP(dmul, *, 2.0, 3.0);
    TEST_D_OP(dmul, *, 3.0, 3.0);
    TEST_D_OP(dmul, *, 1.5, 1.5);
    TEST_D_OP(dmul, *, -2.0, 3.0);
    TEST_D_OP(dmul, *, -2.0, -3.0);
    TEST_D_OP(dmul, *, 1.0, 0.0);
    TEST_D_OP(dmul, *, 1e100, 1e100);
    TEST_D_OP(dmul, *, 1e-100, 1e-100);
    TEST_D_OP(dmul, *, 1.125, 1.125);
    TEST_D_OP(dmul, *, 10.0, 10.0);

    /* Division */
    TEST_D_OP(ddiv, /, 6.0, 2.0);
    TEST_D_OP(ddiv, /, 6.0, 3.0);
    TEST_D_OP(ddiv, /, 9.0, 3.0);
    TEST_D_OP(ddiv, /, 10.0, 3.0);
    TEST_D_OP(ddiv, /, 1.0, 3.0);
    TEST_D_OP(ddiv, /, -10.0, 3.0);
    TEST_D_OP(ddiv, /, 10.0, -3.0);
    TEST_D_OP(ddiv, /, -10.0, -3.0);
    TEST_D_OP(ddiv, /, 1.0, 7.0);
    TEST_D_OP(ddiv, /, 22.0, 7.0);
    TEST_D_OP(ddiv, /, 1e308, 2.0);
}

static void test_double_comparisons(void) {
    printf("--- Double Comparisons ---\n");

    TEST_D_CMP(dcmpeq, ==, 1.0, 1.0);
    TEST_D_CMP(dcmpeq, ==, 1.0, 2.0);
    TEST_D_CMP(dcmpeq, ==, 0.0, -0.0);

    TEST_D_CMP(dcmplt, <, 1.0, 2.0);
    TEST_D_CMP(dcmplt, <, 2.0, 1.0);
    TEST_D_CMP(dcmplt, <, -1.0, 1.0);
    TEST_D_CMP(dcmplt, <, 1.0, 1.0);

    TEST_D_CMP(dcmple, <=, 1.0, 2.0);
    TEST_D_CMP(dcmple, <=, 1.0, 1.0);
    TEST_D_CMP(dcmple, <=, 2.0, 1.0);

    TEST_D_CMP(dcmpgt, >, 2.0, 1.0);
    TEST_D_CMP(dcmpgt, >, 1.0, 2.0);
    TEST_D_CMP(dcmpgt, >, 1.0, 1.0);

    TEST_D_CMP(dcmpge, >=, 2.0, 1.0);
    TEST_D_CMP(dcmpge, >=, 1.0, 1.0);
    TEST_D_CMP(dcmpge, >=, 1.0, 2.0);
}

static void test_double_conversions(void) {
    printf("--- Double Conversions ---\n");

    TEST_I2D(0);
    TEST_I2D(1);
    TEST_I2D(-1);
    TEST_I2D(100);
    TEST_I2D(-100);
    TEST_I2D(2147483647);
    TEST_I2D(-2147483647);

    TEST_UI2D(0);
    TEST_UI2D(1);
    TEST_UI2D(100);
    TEST_UI2D(4294967295U);

    TEST_D2I(0.0);
    TEST_D2I(1.0);
    TEST_D2I(-1.0);
    TEST_D2I(1.5);
    TEST_D2I(-1.5);
    TEST_D2I(100.9);
    TEST_D2I(-100.9);
    TEST_D2I(2147483647.0);

    TEST_D2UI(0.0);
    TEST_D2UI(1.0);
    TEST_D2UI(100.5);
    TEST_D2UI(4294967295.0);
}

static void test_float_arithmetic(void) {
    printf("--- Float Arithmetic ---\n");

    /* Addition */
    TEST_F_OP(fadd, +, 1.5f, 2.0f);
    TEST_F_OP(fadd, +, -1.5f, 2.0f);
    TEST_F_OP(fadd, +, 1.5f, -2.0f);
    TEST_F_OP(fadd, +, 0.0f, 5.0f);
    TEST_F_OP(fadd, +, 1e10f, 1e-10f);

    /* Subtraction */
    TEST_F_OP(fsub, -, 5.0f, 3.0f);
    TEST_F_OP(fsub, -, 3.0f, 5.0f);
    TEST_F_OP(fsub, -, 1.0f, 1.0f);

    /* Multiplication */
    TEST_F_OP(fmul, *, 1.5f, 2.0f);
    TEST_F_OP(fmul, *, 2.0f, 3.0f);
    TEST_F_OP(fmul, *, 3.0f, 3.0f);
    TEST_F_OP(fmul, *, -2.0f, 3.0f);
    TEST_F_OP(fmul, *, 1.0f, 0.0f);
    TEST_F_OP(fmul, *, 1e20f, 1e10f);

    /* Division */
    TEST_F_OP(fdiv, /, 6.0f, 2.0f);
    TEST_F_OP(fdiv, /, 6.0f, 3.0f);
    TEST_F_OP(fdiv, /, 10.0f, 3.0f);
    TEST_F_OP(fdiv, /, 1.0f, 3.0f);
    TEST_F_OP(fdiv, /, -10.0f, 3.0f);
}

static void test_float_comparisons(void) {
    printf("--- Float Comparisons ---\n");

    TEST_F_CMP(fcmpeq, ==, 1.0f, 1.0f);
    TEST_F_CMP(fcmpeq, ==, 1.0f, 2.0f);
    TEST_F_CMP(fcmpeq, ==, 0.0f, -0.0f);

    TEST_F_CMP(fcmplt, <, 1.0f, 2.0f);
    TEST_F_CMP(fcmplt, <, 2.0f, 1.0f);
    TEST_F_CMP(fcmplt, <, -1.0f, 1.0f);

    TEST_F_CMP(fcmple, <=, 1.0f, 2.0f);
    TEST_F_CMP(fcmple, <=, 1.0f, 1.0f);
    TEST_F_CMP(fcmple, <=, 2.0f, 1.0f);

    TEST_F_CMP(fcmpgt, >, 2.0f, 1.0f);
    TEST_F_CMP(fcmpgt, >, 1.0f, 2.0f);

    TEST_F_CMP(fcmpge, >=, 2.0f, 1.0f);
    TEST_F_CMP(fcmpge, >=, 1.0f, 1.0f);
}

static void test_float_conversions(void) {
    printf("--- Float Conversions ---\n");

    TEST_I2F(0);
    TEST_I2F(1);
    TEST_I2F(-1);
    TEST_I2F(100);
    TEST_I2F(-100);
    TEST_I2F(16777215);  /* Max exact int in float */

    TEST_UI2F(0);
    TEST_UI2F(1);
    TEST_UI2F(100);
    TEST_UI2F(16777215);

    TEST_F2I(0.0f);
    TEST_F2I(1.0f);
    TEST_F2I(-1.0f);
    TEST_F2I(1.5f);
    TEST_F2I(-1.5f);
    TEST_F2I(100.9f);

    TEST_F2UI(0.0f);
    TEST_F2UI(1.0f);
    TEST_F2UI(100.5f);
}

static void test_float_double_conversions(void) {
    printf("--- Float <-> Double Conversions ---\n");

    TEST_F2D(0.0f);
    TEST_F2D(1.0f);
    TEST_F2D(-1.0f);
    TEST_F2D(1.5f);
    TEST_F2D(1e30f);
    TEST_F2D(1e-30f);

    TEST_D2F(0.0);
    TEST_D2F(1.0);
    TEST_D2F(-1.0);
    TEST_D2F(1.5);
    TEST_D2F(1e30);
    TEST_D2F(1e-30);
}

int main(void) {
    printf("=== Comprehensive AEABI Soft-Float Host Tests ===\n\n");

    test_double_arithmetic();
    test_double_comparisons();
    test_double_conversions();
    test_float_arithmetic();
    test_float_comparisons();
    test_float_conversions();
    test_float_double_conversions();

    printf("\n=== Results: %d/%d tests passed ===\n", test_count - fail_count, test_count);

    if (fail_count == 0) {
        printf("ALL TESTS PASSED!\n");
        return 0;
    } else {
        printf("FAILURES: %d\n", fail_count);
        return 1;
    }
}
