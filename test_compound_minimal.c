#include <stdio.h>

int failures = 0;

#define CHECK(expr)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
    {                                                                                                                  \
      failures++;                                                                                                      \
      printf("FAIL:%s:%d: " #expr "\n", __FILE__, __LINE__);                                                           \
    }                                                                                                                  \
  } while (0)

void test_compound_ops(void)
{
  printf("Starting test_compound_ops\n");

  unsigned long long u = 3ULL;
  printf("u = %llu\n", u);

  u += 5ULL;
  printf("After +=5: u = %llu\n", u);
  CHECK(u == 8ULL);

  u *= 7ULL;
  printf("After *=7: u = %llu\n", u);
  CHECK(u == 56ULL);

  u >>= 3;
  printf("After >>=3: u = %llu\n", u);
  CHECK(u == 7ULL);

  printf("Before division test\n");

  long long s = -10LL;
  printf("s = %lld\n", s);

  s -= 25LL;
  printf("After -=25: s = %lld\n", s);
  CHECK(s == -35LL);

  printf("About to divide s by 7\n");
  s /= 7LL;
  printf("After /=7: s = %lld\n", s);
  CHECK(s == -5LL);

  printf("test_compound_ops complete\n");
}

int main(void)
{
  test_compound_ops();

  if (failures)
  {
    printf("Test: %d failure(s)\n", failures);
    return 1;
  }
  printf("Test: OK\n");
  return 0;
}
