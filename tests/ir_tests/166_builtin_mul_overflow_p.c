/* Test __builtin_add_overflow_p, __builtin_sub_overflow_p, __builtin_mul_overflow_p */
#include <stdio.h>
#include <limits.h>

int main(void)
{
  int errors = 0;

  /* === __builtin_add_overflow_p (signed int) === */
  
  /* 3 + 4 should NOT overflow */
  if (__builtin_add_overflow_p(3, 4, (int)0)) {
    printf("FAIL: __builtin_add_overflow_p(3, 4) should be false\n");
    errors++;
  } else {
    printf("PASS: __builtin_add_overflow_p(3, 4) = false\n");
  }

  /* INT_MAX + 1 should overflow */
  if (__builtin_add_overflow_p(INT_MAX, 1, (int)0)) {
    printf("PASS: __builtin_add_overflow_p(INT_MAX, 1) = true\n");
  } else {
    printf("FAIL: __builtin_add_overflow_p(INT_MAX, 1) should be true\n");
    errors++;
  }

  /* === __builtin_sub_overflow_p (signed int) === */

  /* 10 - 3 should NOT overflow */
  if (__builtin_sub_overflow_p(10, 3, (int)0)) {
    printf("FAIL: __builtin_sub_overflow_p(10, 3) should be false\n");
    errors++;
  } else {
    printf("PASS: __builtin_sub_overflow_p(10, 3) = false\n");
  }

  /* INT_MIN - 1 should overflow */
  if (__builtin_sub_overflow_p(INT_MIN, 1, (int)0)) {
    printf("PASS: __builtin_sub_overflow_p(INT_MIN, 1) = true\n");
  } else {
    printf("FAIL: __builtin_sub_overflow_p(INT_MIN, 1) should be true\n");
    errors++;
  }

  /* === __builtin_mul_overflow_p (signed int) === */

  /* 6 * 7 should NOT overflow */
  if (__builtin_mul_overflow_p(6, 7, (int)0)) {
    printf("FAIL: __builtin_mul_overflow_p(6, 7) should be false\n");
    errors++;
  } else {
    printf("PASS: __builtin_mul_overflow_p(6, 7) = false\n");
  }

  /* INT_MAX * 2 should overflow */
  if (__builtin_mul_overflow_p(INT_MAX, 2, (int)0)) {
    printf("PASS: __builtin_mul_overflow_p(INT_MAX, 2) = true\n");
  } else {
    printf("FAIL: __builtin_mul_overflow_p(INT_MAX, 2) should be true\n");
    errors++;
  }

  /* 0 * INT_MAX should NOT overflow */
  if (__builtin_mul_overflow_p(0, INT_MAX, (int)0)) {
    printf("FAIL: __builtin_mul_overflow_p(0, INT_MAX) should be false\n");
    errors++;
  } else {
    printf("PASS: __builtin_mul_overflow_p(0, INT_MAX) = false\n");
  }

  /* === Unsigned int tests === */

  /* UINT_MAX + 1 should overflow (unsigned) */
  if (__builtin_add_overflow_p(UINT_MAX, 1u, (unsigned int)0)) {
    printf("PASS: __builtin_add_overflow_p(UINT_MAX, 1u) = true\n");
  } else {
    printf("FAIL: __builtin_add_overflow_p(UINT_MAX, 1u) should be true\n");
    errors++;
  }

  /* 0 - 1 should overflow (unsigned) */
  if (__builtin_sub_overflow_p(0u, 1u, (unsigned int)0)) {
    printf("PASS: __builtin_sub_overflow_p(0u, 1u) = true\n");
  } else {
    printf("FAIL: __builtin_sub_overflow_p(0u, 1u) should be true\n");
    errors++;
  }

  /* === Long long tests === */

  /* LLONG_MAX + 1 should overflow */
  long long ll_max = LLONG_MAX;
  if (__builtin_add_overflow_p(ll_max, 1LL, (long long)0)) {
    printf("PASS: __builtin_add_overflow_p(LLONG_MAX, 1LL) = true\n");
  } else {
    printf("FAIL: __builtin_add_overflow_p(LLONG_MAX, 1LL) should be true\n");
    errors++;
  }

  /* LLONG_MIN - 1 should overflow */
  long long ll_min = LLONG_MIN;
  if (__builtin_sub_overflow_p(ll_min, 1LL, (long long)0)) {
    printf("PASS: __builtin_sub_overflow_p(LLONG_MIN, 1LL) = true\n");
  } else {
    printf("FAIL: __builtin_sub_overflow_p(LLONG_MIN, 1LL) should be true\n");
    errors++;
  }

  /* LLONG_MAX * 2 should overflow */
  if (__builtin_mul_overflow_p(ll_max, 2LL, (long long)0)) {
    printf("PASS: __builtin_mul_overflow_p(LLONG_MAX, 2LL) = true\n");
  } else {
    printf("FAIL: __builtin_mul_overflow_p(LLONG_MAX, 2LL) should be true\n");
    errors++;
  }

  /* === Unsigned long long tests === */

  /* ULLONG_MAX + 1 should overflow */
  unsigned long long ull_max = ULLONG_MAX;
  if (__builtin_add_overflow_p(ull_max, 1ULL, (unsigned long long)0)) {
    printf("PASS: __builtin_add_overflow_p(ULLONG_MAX, 1ULL) = true\n");
  } else {
    printf("FAIL: __builtin_add_overflow_p(ULLONG_MAX, 1ULL) should be true\n");
    errors++;
  }

  /* ULLONG_MAX * 2 should overflow */
  if (__builtin_mul_overflow_p(ull_max, 2ULL, (unsigned long long)0)) {
    printf("PASS: __builtin_mul_overflow_p(ULLONG_MAX, 2ULL) = true\n");
  } else {
    printf("FAIL: __builtin_mul_overflow_p(ULLONG_MAX, 2ULL) should be true\n");
    errors++;
  }

  /* === Edge cases === */

  /* -1 * LLONG_MIN should overflow (absolute value of LLONG_MIN doesn't fit) */
  if (__builtin_mul_overflow_p(-1LL, ll_min, (long long)0)) {
    printf("PASS: __builtin_mul_overflow_p(-1, LLONG_MIN) = true\n");
  } else {
    printf("FAIL: __builtin_mul_overflow_p(-1, LLONG_MIN) should be true\n");
    errors++;
  }

  /* Test with variables */
  int a = 100, b = 200;
  if (__builtin_mul_overflow_p(a, b, (int)0)) {
    printf("FAIL: __builtin_mul_overflow_p(100, 200) should be false\n");
    errors++;
  } else {
    printf("PASS: __builtin_mul_overflow_p(100, 200) = false\n");
  }

  if (errors == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed!\n", errors);
    return 1;
  }
}
