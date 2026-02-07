/*
 * Host-side test for soft-float multiplication
 * Compile with: gcc -O2 -DHOST_TEST test_dmul_host.c -o test_dmul_host -lm && ./test_dmul_host
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#ifdef HOST_TEST
#include <stdint.h>

#define DOUBLE_SIGN_BIT (1ULL << 63)
#define DOUBLE_EXP_MASK 0x7FF0000000000000ULL
#define DOUBLE_MANT_MASK 0x000FFFFFFFFFFFFFULL
#define DOUBLE_EXP_BIAS 1023
#define DOUBLE_EXP_SHIFT 52
#define DOUBLE_IMPLICIT_BIT (1ULL << 52)

typedef union {
  uint64_t u;
  struct {
    uint32_t lo;
    uint32_t hi;
  } w;
} u64_words;

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

/* 64x64 -> 128 multiply helper functions */
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

  *lo = ((uint64_t)w1 << 32) | (uint64_t)w0;
  *hi = ((uint64_t)w3 << 32) | (uint64_t)w2;
}

double __aeabi_dmul(double a, double b) {
  union { double d; uint64_t u; } ua, ub, ur;
  ua.d = a; ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;

  int a_sign = double_sign(a_bits);
  int b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits);
  int b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits);
  uint64_t b_mant = double_mant(b_bits);

  int result_sign = a_sign ^ b_sign;

  if (is_nan_bits(a_bits)) { ur.u = a_bits; return ur.d; }
  if (is_nan_bits(b_bits)) { ur.u = b_bits; return ur.d; }

  if (is_inf_bits(a_bits)) {
    if (is_zero_bits(b_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_inf_bits(b_bits)) {
    if (is_zero_bits(a_bits)) { ur.u = 0x7FF8000000000000ULL; return ur.d; }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }

  if (is_zero_bits(a_bits) || is_zero_bits(b_bits)) {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Fast path for power-of-two */
  if (a_exp != 0 && b_exp != 0) {
    if (a_mant == 0) {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
      if (exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }
      ur.u = make_double(result_sign, exp, b_mant);
      return ur.d;
    }
    if (b_mant == 0) {
      int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;
      if (exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
      if (exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }
      ur.u = make_double(result_sign, exp, a_mant);
      return ur.d;
    }
  }

  if (a_exp != 0) a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0) b_mant |= DOUBLE_IMPLICIT_BIT;

  int result_exp = a_exp + b_exp - DOUBLE_EXP_BIAS;

  uint64_t prod_hi, prod_lo;
  mul64wide(a_mant, b_mant, &prod_hi, &prod_lo);

  const uint64_t bit105_mask = 1ULL << (105 - 64);
  int shift = 52;
  if (prod_hi & bit105_mask) {
    shift = 53;
    result_exp++;
  }

  const uint32_t prod_lo_lo = (uint32_t)prod_lo;
  const uint32_t prod_lo_hi = (uint32_t)(prod_lo >> 32);
  const uint32_t prod_hi_lo = (uint32_t)prod_hi;
  const uint32_t prod_hi_hi = (uint32_t)(prod_hi >> 32);

  uint32_t mant_lo32;
  uint32_t mant_hi32;
  int guard;
  int sticky;
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

  if (mant & (DOUBLE_IMPLICIT_BIT << 1)) {
    mant >>= 1;
    result_exp++;
  }

  if (result_exp >= 0x7FF) { ur.u = make_double(result_sign, 0x7FF, 0); return ur.d; }
  if (result_exp <= 0) { ur.u = make_double(result_sign, 0, 0); return ur.d; }

  mant &= DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, mant);
  return ur.d;
}

#endif /* HOST_TEST */

typedef union { double d; uint64_t u; } dbl_u;
static int test_count = 0;
static int fail_count = 0;

#define TEST_MUL(a_val, b_val) do { \
    dbl_u a, b, got, exp; \
    a.d = a_val; b.d = b_val; \
    got.d = __aeabi_dmul(a.d, b.d); \
    exp.d = a.d * b.d; \
    test_count++; \
    if (got.u != exp.u) { \
        printf("FAIL dmul(%g, %g): got=0x%016llX (%g) exp=0x%016llX (%g)\n", \
               a.d, b.d, (unsigned long long)got.u, got.d, \
               (unsigned long long)exp.u, exp.d); \
        fail_count++; \
    } \
} while(0)

int main(void) {
    printf("=== Testing soft-float multiplication on host ===\n\n");
    
    TEST_MUL(1.5, 2.0);
    TEST_MUL(2.0, 3.0);
    TEST_MUL(3.0, 3.0);    /* was failing */
    TEST_MUL(1.5, 1.5);    /* was failing */
    TEST_MUL(2.0, 2.0);
    TEST_MUL(0.5, 2.0);
    TEST_MUL(10.0, 10.0);
    TEST_MUL(1.0, 10.0);
    TEST_MUL(-2.0, 3.0);
    TEST_MUL(-2.0, -3.0);
    TEST_MUL(1.0, 0.0);
    TEST_MUL(1.25, 1.25);
    TEST_MUL(1.125, 1.125);
    
    printf("\n=== Results: %d/%d tests passed ===\n", test_count - fail_count, test_count);
    
    if (fail_count == 0) {
        printf("ALL TESTS PASSED!\n");
        return 0;
    } else {
        printf("FAILURES: %d\n", fail_count);
        return 1;
    }
}
