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

static long long mul_s(long long a, long long b)
{
  return a * b;
}

int main(void)
{
  printf("Testing signed long long mul\n");

  if (check_s64("small", mul_s(3LL, 4LL), 12LL))
    return 1;
  if (check_s64("cross32", mul_s((1LL << 32), 10LL), (1LL << 32) * 10LL))
    return 1;
  if (check_s64("neg", mul_s(-123456789LL, 1000LL), -123456789000LL))
    return 1;
  if (check_s64("mix", mul_s((1LL << 33) + 7LL, 9LL), ((1LL << 33) + 7LL) * 9LL))
    return 1;

  printf("PASS\n");
  return 0;
}
