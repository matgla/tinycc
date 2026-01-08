/* Simple long long / unsigned long long arithmetic test.
 *
 * Keep this test self-contained and avoid UB (e.g., signed overflow).
 */

#include <stdio.h>

static int failures;

#define CHECK(expr)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
    {                                                                                                                  \
      ++failures;                                                                                                      \
      printf("FAIL:%s:%d: %s\n", __FILE__, __LINE__, #expr);                                                           \
    }                                                                                                                  \
  } while (0)

static void test_basic_signed(void)
{
  long long a = 1234567890123LL;
  long long b = -987654321LL;

  CHECK(a + b == 1233580235802LL);
  CHECK(a - b == 1235555544444LL);
  CHECK(-b == 987654321LL);

  /* Multiplication within range */
  CHECK(3000000LL * 7000000LL == 21000000000000LL);

  /* Division and modulo (C99+ truncates toward 0) */
  CHECK(7LL / 3LL == 2LL);
  CHECK(7LL % 3LL == 1LL);
  CHECK(-7LL / 3LL == -2LL);
  CHECK(-7LL % 3LL == -1LL);
  CHECK(7LL / -3LL == -2LL);
  CHECK(7LL % -3LL == 1LL);
  CHECK(-7LL / -3LL == 2LL);
  CHECK(-7LL % -3LL == -1LL);

  CHECK((long long)0 == 0LL);
  CHECK((long long)1 == 1LL);
}

static void test_basic_unsigned(void)
{
  unsigned long long u = 0ULL;
  CHECK(u == 0ULL);
  u = 1ULL;
  CHECK(u + 1ULL == 2ULL);

  /* Well-defined wraparound */
  CHECK(0ULL - 1ULL > 0ULL);

  /* Constant folding and large literal handling */
  CHECK(0x1122334455667788ULL == 1234605616436508552ULL);
}

static void test_shifts_and_bitops(void)
{
  unsigned long long u;

  u = 1ULL;
  CHECK((u << 0) == 1ULL);
  CHECK((u << 1) == 2ULL);
  CHECK((u << 63) == 0x8000000000000000ULL);
  CHECK((0x8000000000000000ULL >> 63) == 1ULL);

  /* Bitwise ops */
  CHECK((0xF0ULL & 0xCCULL) == 0xC0ULL);
  CHECK((0xF0ULL | 0x0FULL) == 0xFFULL);
  CHECK((0xAAULL ^ 0xFFULL) == 0x55ULL);

  /* Mix signed/unsigned cautiously (cast to avoid surprises) */
  CHECK(((unsigned long long)(-1LL)) == ~0ULL);
}

static void test_compares_and_casts(void)
{
  long long s1 = -1LL;
  long long s2 = 0LL;
  unsigned long long u1 = 1ULL;

  CHECK(s1 < s2);
  CHECK(!(s2 < s1));
  CHECK(u1 > 0ULL);

  /* Cast behavior */
  CHECK((unsigned long long)s1 == ~0ULL);
  CHECK((long long)(unsigned long long)s1 == -1LL);

  /* Ensure relational ops on 64-bit values work */
  CHECK(9223372036854775807LL > 0LL);
  CHECK(9223372036854775807LL >= 9223372036854775807LL);
  CHECK(0LL <= 9223372036854775807LL);
}

static void test_compound_ops(void)
{
  unsigned long long u = 3ULL;
  u += 5ULL;
  CHECK(u == 8ULL);
  u *= 7ULL;
  CHECK(u == 56ULL);
  u >>= 3;
  CHECK(u == 7ULL);

  long long s = -10LL;
  s -= 25LL;
  CHECK(s == -35LL);
  s /= 7LL;
  CHECK(s == -5LL);
}

int main(void)
{
  test_basic_signed();
  test_basic_unsigned();
  test_shifts_and_bitops();
  test_compares_and_casts();
  test_compound_ops();

  if (failures)
  {
    printf("llong_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("llong_test: OK\n");
  return 0;
}
