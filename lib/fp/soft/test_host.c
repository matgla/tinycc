/*
 * Host-side test for soft-float division
 * Compile with: gcc -O2 -DHOST_TEST test_host.c -o test_host -lm && ./test_host
 *
 * Note: This file includes inline implementations of both ddiv and fdiv for testing
 * the algorithms on the host without requiring cross-compilation.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Provide standard headers for host compilation */
#ifdef HOST_TEST
#include <stdint.h>
#define tcc_stdint_h_included
#endif

/* Include soft_common.h but we need to work around the tcc_stdint.h include */

/* Minimal definitions needed for ddiv.c when HOST_TEST is defined */
#ifdef HOST_TEST

/* From soft_common.h */
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
    uint32_t lo;
    uint32_t hi;
  } w;
} u64_words;

static inline int double_sign(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  return (v.w.hi >> 31) & 1;
}

static inline int double_exp(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  return (v.w.hi >> 20) & 0x7FF;
}

static inline uint64_t double_mant(uint64_t bits)
{
  u64_words v;
  v.u = bits;
  v.w.hi &= 0xFFFFF;
  return v.u;
}

static inline int is_nan_bits(uint64_t bits)
{
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) != 0);
}

static inline int is_inf_bits(uint64_t bits)
{
  return (double_exp(bits) == 0x7FF) && (double_mant(bits) == 0);
}

static inline int is_zero_bits(uint64_t bits)
{
  return (double_exp(bits) == 0) && (double_mant(bits) == 0);
}

static inline uint64_t make_double(int sign, int exp, uint64_t mant)
{
  u64_words v;
  u64_words m;
  m.u = mant;
  v.w.lo = m.w.lo;
  v.w.hi = ((uint32_t)sign << 31) | ((uint32_t)exp << 20) | (m.w.hi & 0xFFFFF);
  return v.u;
}

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

static inline int clz64(uint64_t x)
{
  u64_words v;
  v.u = x;
  if (v.w.hi != 0)
    return clz32(v.w.hi);
  return 32 + clz32(v.w.lo);
}

#endif /* HOST_TEST */

/* Now implement ddiv inline for testing */
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

  int result_sign = a_sign ^ b_sign;

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

  if (is_inf_bits(a_bits))
  {
    if (is_inf_bits(b_bits))
    {
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_inf_bits(b_bits))
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  if (is_zero_bits(b_bits))
  {
    if (is_zero_bits(a_bits))
    {
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (is_zero_bits(a_bits))
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  if (a_exp != 0)
    a_mant |= DOUBLE_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= DOUBLE_IMPLICIT_BIT;

  int result_exp = a_exp - b_exp + DOUBLE_EXP_BIAS;

  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient = 0;

  if (dividend < divisor)
  {
    dividend <<= 1;
    result_exp--;
  }

  for (int i = 0; i < 54; i++)
  {
    quotient <<= 1;
    if (!(dividend < divisor))
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  uint64_t guard = quotient & 1;
  quotient >>= 1;
  if (guard && dividend)
    quotient++;

  while (quotient >= (DOUBLE_IMPLICIT_BIT << 1))
  {
    quotient >>= 1;
    result_exp++;
  }
  while (quotient && !(quotient & DOUBLE_IMPLICIT_BIT))
  {
    quotient <<= 1;
    result_exp--;
  }

  if (result_exp >= 0x7FF)
  {
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (result_exp <= 0)
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  uint64_t result_mant = quotient & DOUBLE_MANT_MASK;
  ur.u = make_double(result_sign, result_exp, result_mant);
  return ur.d;
}

/* ===== SINGLE PRECISION FDIV IMPLEMENTATION FOR TESTING ===== */

#define FLOAT_SIGN_BIT (1U << 31)
#define FLOAT_EXP_MASK 0x7F800000U
#define FLOAT_MANT_MASK 0x007FFFFFU
#define FLOAT_EXP_BIAS 127
#define FLOAT_IMPLICIT_BIT (1U << 23)

static inline int float_sign(uint32_t bits)
{
  return (bits >> 31) & 1;
}

static inline int float_exp(uint32_t bits)
{
  return (bits >> 23) & 0xFF;
}

static inline uint32_t float_mant(uint32_t bits)
{
  return bits & FLOAT_MANT_MASK;
}

static inline int is_nan_f(uint32_t bits)
{
  return (float_exp(bits) == 0xFF) && (float_mant(bits) != 0);
}

static inline int is_inf_f(uint32_t bits)
{
  return (float_exp(bits) == 0xFF) && (float_mant(bits) == 0);
}

static inline int is_zero_f(uint32_t bits)
{
  return (float_exp(bits) == 0) && (float_mant(bits) == 0);
}

static inline uint32_t make_float(int sign, int exp, uint32_t mant)
{
  return ((uint32_t)sign << 31) | ((uint32_t)exp << 23) | (mant & FLOAT_MANT_MASK);
}

/* Single-precision float division - for testing the algorithm */
float __aeabi_fdiv_test(float a, float b)
{
  union
  {
    float f;
    uint32_t u;
  } ua, ub, ur;
  ua.f = a;
  ub.f = b;
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
    if (is_inf_f(b_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (is_inf_f(b_bits))
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  /* Handle zero */
  if (is_zero_f(b_bits))
  {
    if (is_zero_f(a_bits))
    {
      ur.u = 0x7FC00000U;
      return ur.f;
    }
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (is_zero_f(a_bits))
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  /* Add implicit bit */
  if (a_exp != 0)
    a_mant |= FLOAT_IMPLICIT_BIT;
  if (b_exp != 0)
    b_mant |= FLOAT_IMPLICIT_BIT;

  /* Calculate result exponent */
  int result_exp = a_exp - b_exp + FLOAT_EXP_BIAS;

  /* Normalize for division */
  int a_shift = clz32(a_mant) - 8;
  int b_shift = clz32(b_mant) - 8;

  /* BUGGY IMPLEMENTATION - using shifts that cause overflow */
  /* uint64_t dividend = (uint64_t)a_mant << 32; */
  /* uint64_t divisor = (uint64_t)b_mant << (32 - 23); */
  /* result_exp += (b_shift - a_shift); */

  /* FIXED IMPLEMENTATION - use same algorithm as ddiv */
  uint64_t dividend = a_mant;
  uint64_t divisor = b_mant;
  uint64_t quotient = 0;

  /* Align dividend with divisor */
  if (dividend < divisor)
  {
    dividend <<= 1;
    result_exp--;
  }

  /* Perform division - need 25 bits (1 integer + 23 fraction + 1 guard) */
  for (int i = 0; i < 25; i++)
  {
    quotient <<= 1;
    if (dividend >= divisor)
    {
      dividend -= divisor;
      quotient |= 1;
    }
    dividend <<= 1;
  }

  /* Handle guard bit for rounding - round half up */
  uint32_t guard = quotient & 1;
  quotient >>= 1;
  if (guard && dividend)
    quotient++;

  /* Normalize - quotient should now be in [2^23, 2^24) */
  if (quotient >= (FLOAT_IMPLICIT_BIT << 1))
  {
    quotient >>= 1;
    result_exp++;
  }

  if (result_exp >= 0xFF)
  {
    ur.u = make_float(result_sign, 0xFF, 0);
    return ur.f;
  }
  if (result_exp <= 0)
  {
    ur.u = make_float(result_sign, 0, 0);
    return ur.f;
  }

  uint32_t result_mant = (uint32_t)quotient & FLOAT_MANT_MASK;
  ur.u = make_float(result_sign, result_exp, result_mant);
  return ur.f;
}

/* ===== TEST FRAMEWORK ===== */

typedef union
{
  double d;
  uint64_t u;
} dbl_u;
typedef union
{
  float f;
  uint32_t u;
} flt_u;

static int test_count = 0;
static int fail_count = 0;

#define TEST_DDIV(a_val, b_val)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    dbl_u a, b, got, exp;                                                                                              \
    a.d = a_val;                                                                                                       \
    b.d = b_val;                                                                                                       \
    got.d = __aeabi_ddiv(a.d, b.d);                                                                                    \
    exp.d = a.d / b.d;                                                                                                 \
    test_count++;                                                                                                      \
    if (got.u != exp.u)                                                                                                \
    {                                                                                                                  \
      printf("FAIL ddiv(%g, %g): got=0x%016llX exp=0x%016llX\n", a.d, b.d, (unsigned long long)got.u,                  \
             (unsigned long long)exp.u);                                                                               \
      fail_count++;                                                                                                    \
    }                                                                                                                  \
  } while (0)

#define TEST_FDIV(a_val, b_val)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    flt_u a, b, got, exp;                                                                                              \
    a.f = a_val;                                                                                                       \
    b.f = b_val;                                                                                                       \
    got.f = __aeabi_fdiv_test(a.f, b.f);                                                                               \
    exp.f = a.f / b.f;                                                                                                 \
    test_count++;                                                                                                      \
    if (got.u != exp.u)                                                                                                \
    {                                                                                                                  \
      printf("FAIL fdiv(%g, %g): got=0x%08X (%.7g) exp=0x%08X (%.7g)\n", a.f, b.f, got.u, got.f, exp.u, exp.f);        \
      fail_count++;                                                                                                    \
    }                                                                                                                  \
  } while (0)

int main(void)
{
  printf("=== Testing soft-float division on host ===\n\n");

  /* ===== DOUBLE PRECISION TESTS ===== */
  printf("--- Double precision (ddiv) ---\n");

  /* Basic divisions */
  TEST_DDIV(1.5, 2.0);    /* 0.75 */
  TEST_DDIV(6.0, 3.0);    /* 2.0 */
  TEST_DDIV(6.0, 2.0);    /* 3.0 */
  TEST_DDIV(9.0, 3.0);    /* 3.0 - was failing */
  TEST_DDIV(10.0, 3.0);   /* 3.333... - was failing */
  TEST_DDIV(1.0, 3.0);    /* 0.333... - was failing */
  TEST_DDIV(2.0, 3.0);    /* 0.666... - was failing */
  TEST_DDIV(4.0, 2.0);    /* 2.0 */
  TEST_DDIV(5.0, 2.0);    /* 2.5 */
  TEST_DDIV(7.0, 2.0);    /* 3.5 */
  TEST_DDIV(1.0, 7.0);    /* 0.142857... */
  TEST_DDIV(22.0, 7.0);   /* ~3.14... */
  TEST_DDIV(1.0, 10.0);   /* 0.1 */
  TEST_DDIV(100.0, 3.0);  /* 33.333... */
  TEST_DDIV(1e10, 3.0);   /* large / 3 */
  TEST_DDIV(1e-10, 3.0);  /* small / 3 */
  TEST_DDIV(1.0, 1e10);   /* 1 / large */
  TEST_DDIV(-10.0, 3.0);  /* negative dividend */
  TEST_DDIV(10.0, -3.0);  /* negative divisor */
  TEST_DDIV(-10.0, -3.0); /* both negative */

  /* Edge cases */
  TEST_DDIV(1.0, 1.0);   /* 1.0 */
  TEST_DDIV(2.0, 1.0);   /* 2.0 */
  TEST_DDIV(0.5, 1.0);   /* 0.5 */
  TEST_DDIV(1.0, 0.5);   /* 2.0 */
  TEST_DDIV(1e308, 2.0); /* large number */

  /* Powers of 2 */
  TEST_DDIV(8.0, 2.0);
  TEST_DDIV(16.0, 4.0);
  TEST_DDIV(32.0, 8.0);
  TEST_DDIV(1.0, 2.0);
  TEST_DDIV(1.0, 4.0);
  TEST_DDIV(1.0, 8.0);

  /* More division by 3 tests */
  TEST_DDIV(3.0, 3.0);
  TEST_DDIV(12.0, 3.0);
  TEST_DDIV(15.0, 3.0);
  TEST_DDIV(99.0, 3.0);
  TEST_DDIV(1000.0, 3.0);

  printf("\n--- Single precision (fdiv) ---\n");

  /* ===== SINGLE PRECISION TESTS ===== */
  /* This is the key failing test case from the benchmark */
  TEST_FDIV(10.0f, 3.0f);   /* 3.333... - THE BUG CASE */
  TEST_FDIV(1.0f, 3.0f);    /* 0.333... */
  TEST_FDIV(2.0f, 3.0f);    /* 0.666... */
  TEST_FDIV(1.5f, 2.0f);    /* 0.75 */
  TEST_FDIV(6.0f, 3.0f);    /* 2.0 */
  TEST_FDIV(6.0f, 2.0f);    /* 3.0 */
  TEST_FDIV(9.0f, 3.0f);    /* 3.0 */
  TEST_FDIV(4.0f, 2.0f);    /* 2.0 */
  TEST_FDIV(5.0f, 2.0f);    /* 2.5 */
  TEST_FDIV(7.0f, 2.0f);    /* 3.5 */
  TEST_FDIV(1.0f, 7.0f);    /* 0.142857... */
  TEST_FDIV(22.0f, 7.0f);   /* ~3.14... */
  TEST_FDIV(1.0f, 10.0f);   /* 0.1 */
  TEST_FDIV(100.0f, 3.0f);  /* 33.333... */
  TEST_FDIV(-10.0f, 3.0f);  /* negative dividend */
  TEST_FDIV(10.0f, -3.0f);  /* negative divisor */
  TEST_FDIV(-10.0f, -3.0f); /* both negative */

  /* Edge cases */
  TEST_FDIV(1.0f, 1.0f); /* 1.0 */
  TEST_FDIV(2.0f, 1.0f); /* 2.0 */
  TEST_FDIV(0.5f, 1.0f); /* 0.5 */
  TEST_FDIV(1.0f, 0.5f); /* 2.0 */

  /* Powers of 2 */
  TEST_FDIV(8.0f, 2.0f);
  TEST_FDIV(16.0f, 4.0f);
  TEST_FDIV(32.0f, 8.0f);
  TEST_FDIV(1.0f, 2.0f);
  TEST_FDIV(1.0f, 4.0f);
  TEST_FDIV(1.0f, 8.0f);

  printf("\n=== Results: %d/%d tests passed ===\n", test_count - fail_count, test_count);

  if (fail_count == 0)
  {
    printf("ALL TESTS PASSED!\n");
    return 0;
  }
  else
  {
    printf("FAILURES: %d\n", fail_count);
    return 1;
  }
}
