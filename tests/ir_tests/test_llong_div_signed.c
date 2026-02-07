#include <stdio.h>

static int check_s64(const char *name, long long got, long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%lld exp=%lld\n", name, got, exp);
    return 1;
  }
  return 0;
}

static long long div_s(long long a, long long b)
{
  return a / b;
}

int main(void)
{
  printf("Testing signed long long div\n");

  if (check_s64("10e10/10", div_s(10000000000LL, 10LL), 1000000000LL))
    return 1;
  if (check_s64("neg", div_s(-10000000000LL, 10LL), -1000000000LL))
    return 1;
  if (check_s64("toward0", div_s(7LL, -3LL), -2LL))
    return 1;
  if (check_s64("toward0b", div_s(-7LL, 3LL), -2LL))
    return 1;
  if (check_s64("pow", div_s((1LL << 40), (1LL << 8)), (1LL << 32)))
    return 1;

  printf("PASS\n");
  return 0;
}
