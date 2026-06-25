/* Diagnostic test for long long arithmetic on ARM.
 * Prints intermediate values to pinpoint where 64-bit handling breaks.
 * Compile with: tcc 136_llong_diag.c -o diag
 * Also cross-compile with: armv8m-tcc 136_llong_diag.c -o diag_cross
 */
#include <stdio.h>

static int failures;

static void print_ll(const char *name, long long val)
{
  unsigned int lo = (unsigned int)(val & 0xFFFFFFFFU);
  unsigned int hi = (unsigned int)((unsigned long long)val >> 32);
  printf("%s = 0x%08x_%08x (%lld)\n", name, hi, lo, val);
}

static void print_ull(const char *name, unsigned long long val)
{
  unsigned int lo = (unsigned int)(val & 0xFFFFFFFFU);
  unsigned int hi = (unsigned int)(val >> 32);
  printf("%s = 0x%08x_%08x (%llu)\n", name, hi, lo, val);
}

static void check_eq_ll(const char *expr, long long got, long long expected, int line)
{
  if (got != expected)
  {
    unsigned int got_lo = (unsigned int)(got & 0xFFFFFFFFU);
    unsigned int got_hi = (unsigned int)((unsigned long long)got >> 32);
    unsigned int exp_lo = (unsigned int)(expected & 0xFFFFFFFFU);
    unsigned int exp_hi = (unsigned int)((unsigned long long)expected >> 32);
    printf("FAIL line %d: %s\n  got:      0x%08x_%08x (%lld)\n  expected: 0x%08x_%08x (%lld)\n", line, expr, got_hi,
           got_lo, got, exp_hi, exp_lo, expected);
    failures++;
  }
}

static void check_eq_ull(const char *expr, unsigned long long got, unsigned long long expected, int line)
{
  if (got != expected)
  {
    unsigned int got_lo = (unsigned int)(got & 0xFFFFFFFFU);
    unsigned int got_hi = (unsigned int)(got >> 32);
    unsigned int exp_lo = (unsigned int)(expected & 0xFFFFFFFFU);
    unsigned int exp_hi = (unsigned int)(expected >> 32);
    printf("FAIL line %d: %s\n  got:      0x%08x_%08x (%llu)\n  expected: 0x%08x_%08x (%llu)\n", line, expr, got_hi,
           got_lo, got, exp_hi, exp_lo, expected);
    failures++;
  }
}

int main(void)
{
  printf("=== Long long diagnostic ===\n");

  /* Test 1: Variable initialization with large constants */
  printf("\n--- Test 1: Variable init ---\n");
  long long a = 1234567890123LL;
  long long b = -987654321LL;
  print_ll("a", a);
  print_ll("b", b);

  /* Test 2: Negation */
  printf("\n--- Test 2: Negation ---\n");
  long long neg_b = -b;
  print_ll("-b", neg_b);
  check_eq_ll("-b == 987654321LL", neg_b, 987654321LL, __LINE__);

  /* Test 3: Addition */
  printf("\n--- Test 3: Addition ---\n");
  long long sum = a + b;
  print_ll("a + b", sum);
  check_eq_ll("a + b == 1233580235802LL", sum, 1233580235802LL, __LINE__);

  /* Test 4: Subtraction */
  printf("\n--- Test 4: Subtraction ---\n");
  long long diff = a - b;
  print_ll("a - b", diff);
  check_eq_ll("a - b == 1235555544444LL", diff, 1235555544444LL, __LINE__);

  /* Test 5: Shift left by large amount */
  printf("\n--- Test 5: Shift left ---\n");
  unsigned long long u = 1ULL;
  print_ull("u", u);
  unsigned long long shifted = u << 63;
  print_ull("u << 63", shifted);
  check_eq_ull("(u << 63) == 0x8000000000000000ULL", shifted, 0x8000000000000000ULL, __LINE__);

  /* Test 6: Shift left by small amounts (should pass) */
  printf("\n--- Test 6: Small shifts ---\n");
  unsigned long long s0 = u << 0;
  unsigned long long s1 = u << 1;
  unsigned long long s31 = u << 31;
  unsigned long long s32 = u << 32;
  print_ull("u << 0", s0);
  print_ull("u << 1", s1);
  print_ull("u << 31", s31);
  print_ull("u << 32", s32);
  check_eq_ull("u << 0", s0, 1ULL, __LINE__);
  check_eq_ull("u << 1", s1, 2ULL, __LINE__);
  check_eq_ull("u << 31", s31, 0x80000000ULL, __LINE__);
  check_eq_ull("u << 32", s32, 0x100000000ULL, __LINE__);

  /* Test 7: Constant folding (should always pass) */
  printf("\n--- Test 7: Constant folding ---\n");
  check_eq_ll("const add", 1234567890123LL + (-987654321LL), 1233580235802LL, __LINE__);
  check_eq_ull("const shift", 1ULL << 63, 0x8000000000000000ULL, __LINE__);

  /* Test 8: printf with %lld directly (tests argument passing) */
  printf("\n--- Test 8: printf arg passing ---\n");
  printf("a via printf: %lld\n", a);
  printf("b via printf: %lld\n", b);
  printf("a+b via printf: %lld\n", a + b);

  printf("\n=== Result: %d failures ===\n", failures);
  return failures;
}
