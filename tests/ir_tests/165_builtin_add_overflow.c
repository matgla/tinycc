/* Test __builtin_add_overflow, __builtin_sub_overflow, __builtin_mul_overflow */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define LLONG_MIN_VAL (-9223372036854775807LL - 1)
#define LLONG_MAX_VAL 9223372036854775807LL
#define ULLONG_MAX_VAL 18446744073709551615ULL

int main(void)
{
  int errors = 0;
  int result;
  int overflow;

  /* ============================================================
   * 32-bit tests (signed int, unsigned int)
   * ============================================================ */

  /* === __builtin_add_overflow (signed int) === */

  /* No overflow: 3 + 4 = 7 */
  result = 0;
  overflow = __builtin_add_overflow(3, 4, &result);
  if (overflow != 0 || result != 7)
  {
    printf("FAIL: add(3,4) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* Signed overflow: INT_MAX + 1 */
  result = 0;
  overflow = __builtin_add_overflow(INT_MAX, 1, &result);
  if (overflow != 1)
  {
    printf("FAIL: add(INT_MAX,1) overflow=%d (expected 1)\n", overflow);
    errors++;
  }

  /* Signed overflow: INT_MIN + (-1) */
  result = 0;
  overflow = __builtin_add_overflow(INT_MIN, -1, &result);
  if (overflow != 1)
  {
    printf("FAIL: add(INT_MIN,-1) overflow=%d (expected 1)\n", overflow);
    errors++;
  }

  /* No overflow: INT_MAX + 0 */
  result = 0;
  overflow = __builtin_add_overflow(INT_MAX, 0, &result);
  if (overflow != 0 || result != INT_MAX)
  {
    printf("FAIL: add(INT_MAX,0) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* No overflow: negative + positive */
  result = 0;
  overflow = __builtin_add_overflow(-10, 20, &result);
  if (overflow != 0 || result != 10)
  {
    printf("FAIL: add(-10,20) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* === __builtin_sub_overflow (signed int) === */

  /* No overflow: 10 - 3 = 7 */
  result = 0;
  overflow = __builtin_sub_overflow(10, 3, &result);
  if (overflow != 0 || result != 7)
  {
    printf("FAIL: sub(10,3) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* Signed overflow: INT_MIN - 1 */
  result = 0;
  overflow = __builtin_sub_overflow(INT_MIN, 1, &result);
  if (overflow != 1)
  {
    printf("FAIL: sub(INT_MIN,1) overflow=%d (expected 1)\n", overflow);
    errors++;
  }

  /* Signed overflow: INT_MAX - (-1) */
  result = 0;
  overflow = __builtin_sub_overflow(INT_MAX, -1, &result);
  if (overflow != 1)
  {
    printf("FAIL: sub(INT_MAX,-1) overflow=%d (expected 1)\n", overflow);
    errors++;
  }

  /* === __builtin_mul_overflow (signed int) === */

  /* No overflow: 6 * 7 = 42 */
  result = 0;
  overflow = __builtin_mul_overflow(6, 7, &result);
  if (overflow != 0 || result != 42)
  {
    printf("FAIL: mul(6,7) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* Signed overflow: INT_MAX * 2 */
  result = 0;
  overflow = __builtin_mul_overflow(INT_MAX, 2, &result);
  if (overflow != 1)
  {
    printf("FAIL: mul(INT_MAX,2) overflow=%d (expected 1)\n", overflow);
    errors++;
  }

  /* No overflow: 0 * anything */
  result = 99;
  overflow = __builtin_mul_overflow(0, INT_MAX, &result);
  if (overflow != 0 || result != 0)
  {
    printf("FAIL: mul(0,INT_MAX) overflow=%d result=%d\n", overflow, result);
    errors++;
  }

  /* === __builtin_add_overflow with unsigned int result === */
  {
    unsigned int uresult;
    overflow = __builtin_add_overflow(3u, 4u, &uresult);
    if (overflow != 0 || uresult != 7u)
    {
      printf("FAIL: uadd(3,4) overflow=%d result=%u\n", overflow, uresult);
      errors++;
    }

    /* Unsigned overflow: UINT_MAX + 1 */
    overflow = __builtin_add_overflow(UINT_MAX, 1u, &uresult);
    if (overflow != 1)
    {
      printf("FAIL: uadd(UINT_MAX,1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }
  }

  /* ============================================================
   * 64-bit tests (signed long long, unsigned long long)
   * ============================================================ */

  /* === 64-bit signed add === */
  {
    long long r64;

    /* No overflow: 100 + 200 */
    overflow = __builtin_add_overflow(100LL, 200LL, &r64);
    if (overflow != 0 || r64 != 300LL)
    {
      printf("FAIL: add64(100,200) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MAX + 1 */
    overflow = __builtin_add_overflow(LLONG_MAX_VAL, 1LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: add64(LLONG_MAX,1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MIN + (-1) */
    overflow = __builtin_add_overflow(LLONG_MIN_VAL, -1LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: add64(LLONG_MIN,-1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: -10 + 20 */
    overflow = __builtin_add_overflow(-10LL, 20LL, &r64);
    if (overflow != 0 || r64 != 10LL)
    {
      printf("FAIL: add64(-10,20) overflow=%d\n", overflow);
      errors++;
    }

    /* No overflow: LLONG_MAX + 0 */
    overflow = __builtin_add_overflow(LLONG_MAX_VAL, 0LL, &r64);
    if (overflow != 0 || r64 != LLONG_MAX_VAL)
    {
      printf("FAIL: add64(LLONG_MAX,0) overflow=%d\n", overflow);
      errors++;
    }
  }

  /* === 64-bit signed sub === */
  {
    long long r64;

    /* No overflow: 100 - 30 */
    overflow = __builtin_sub_overflow(100LL, 30LL, &r64);
    if (overflow != 0 || r64 != 70LL)
    {
      printf("FAIL: sub64(100,30) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MIN - 1 */
    overflow = __builtin_sub_overflow(LLONG_MIN_VAL, 1LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: sub64(LLONG_MIN,1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MAX - (-1) */
    overflow = __builtin_sub_overflow(LLONG_MAX_VAL, -1LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: sub64(LLONG_MAX,-1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: 0 - 0 */
    overflow = __builtin_sub_overflow(0LL, 0LL, &r64);
    if (overflow != 0 || r64 != 0LL)
    {
      printf("FAIL: sub64(0,0) overflow=%d\n", overflow);
      errors++;
    }
  }

  /* === 64-bit unsigned add === */
  {
    unsigned long long ur64;

    /* No overflow */
    overflow = __builtin_add_overflow(100ULL, 200ULL, &ur64);
    if (overflow != 0 || ur64 != 300ULL)
    {
      printf("FAIL: uadd64(100,200) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: ULLONG_MAX + 1 */
    overflow = __builtin_add_overflow(ULLONG_MAX_VAL, 1ULL, &ur64);
    if (overflow != 1)
    {
      printf("FAIL: uadd64(ULLONG_MAX,1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: ULLONG_MAX + 0 */
    overflow = __builtin_add_overflow(ULLONG_MAX_VAL, 0ULL, &ur64);
    if (overflow != 0 || ur64 != ULLONG_MAX_VAL)
    {
      printf("FAIL: uadd64(ULLONG_MAX,0) overflow=%d\n", overflow);
      errors++;
    }
  }

  /* === 64-bit unsigned sub === */
  {
    unsigned long long ur64;

    /* No overflow */
    overflow = __builtin_sub_overflow(300ULL, 100ULL, &ur64);
    if (overflow != 0 || ur64 != 200ULL)
    {
      printf("FAIL: usub64(300,100) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: 0 - 1 */
    overflow = __builtin_sub_overflow(0ULL, 1ULL, &ur64);
    if (overflow != 1)
    {
      printf("FAIL: usub64(0,1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: 5 - 5 */
    overflow = __builtin_sub_overflow(5ULL, 5ULL, &ur64);
    if (overflow != 0 || ur64 != 0ULL)
    {
      printf("FAIL: usub64(5,5) overflow=%d\n", overflow);
      errors++;
    }
  }

  /* === 64-bit unsigned mul === */
  {
    unsigned long long ur64;

    /* No overflow */
    overflow = __builtin_mul_overflow(100ULL, 200ULL, &ur64);
    if (overflow != 0 || ur64 != 20000ULL)
    {
      printf("FAIL: umul64(100,200) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: ULLONG_MAX * 2 */
    overflow = __builtin_mul_overflow(ULLONG_MAX_VAL, 2ULL, &ur64);
    if (overflow != 1)
    {
      printf("FAIL: umul64(ULLONG_MAX,2) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: 0 * anything */
    overflow = __builtin_mul_overflow(0ULL, ULLONG_MAX_VAL, &ur64);
    if (overflow != 0 || ur64 != 0ULL)
    {
      printf("FAIL: umul64(0,ULLONG_MAX) overflow=%d\n", overflow);
      errors++;
    }

    /* No overflow: 1 * ULLONG_MAX */
    overflow = __builtin_mul_overflow(1ULL, ULLONG_MAX_VAL, &ur64);
    if (overflow != 0 || ur64 != ULLONG_MAX_VAL)
    {
      printf("FAIL: umul64(1,ULLONG_MAX) overflow=%d\n", overflow);
      errors++;
    }
  }

  /* === 64-bit signed mul === */
  {
    long long r64;

    /* No overflow: 6 * 7 */
    overflow = __builtin_mul_overflow(6LL, 7LL, &r64);
    if (overflow != 0 || r64 != 42LL)
    {
      printf("FAIL: smul64(6,7) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MAX * 2 */
    overflow = __builtin_mul_overflow(LLONG_MAX_VAL, 2LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: smul64(LLONG_MAX,2) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: 0 * anything */
    overflow = __builtin_mul_overflow(0LL, LLONG_MAX_VAL, &r64);
    if (overflow != 0 || r64 != 0LL)
    {
      printf("FAIL: smul64(0,LLONG_MAX) overflow=%d\n", overflow);
      errors++;
    }

    /* Overflow: -1 * LLONG_MIN (edge case) */
    overflow = __builtin_mul_overflow(-1LL, LLONG_MIN_VAL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: smul64(-1,LLONG_MIN) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* Overflow: LLONG_MIN * -1 (symmetric edge case) */
    overflow = __builtin_mul_overflow(LLONG_MIN_VAL, -1LL, &r64);
    if (overflow != 1)
    {
      printf("FAIL: smul64(LLONG_MIN,-1) overflow=%d (expected 1)\n", overflow);
      errors++;
    }

    /* No overflow: -1 * 5 */
    overflow = __builtin_mul_overflow(-1LL, 5LL, &r64);
    if (overflow != 0 || r64 != -5LL)
    {
      printf("FAIL: smul64(-1,5) overflow=%d\n", overflow);
      errors++;
    }

    /* No overflow: 1 * LLONG_MIN */
    overflow = __builtin_mul_overflow(1LL, LLONG_MIN_VAL, &r64);
    if (overflow != 0 || r64 != LLONG_MIN_VAL)
    {
      printf("FAIL: smul64(1,LLONG_MIN) overflow=%d\n", overflow);
      errors++;
    }
  }

  if (errors == 0)
    printf("OK\n");
  else
    printf("%d errors\n", errors);

  return errors;
}
