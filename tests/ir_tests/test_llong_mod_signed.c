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

static long long mod_s(long long a, long long b)
{
  return a % b;
}

int main(void)
{
  printf("Testing signed long long mod\n");

  if (check_s64("10e10+1", mod_s(10000000001LL, 10LL), 1LL))
    return 1;
  if (check_s64("neg", mod_s(-10000000001LL, 10LL), -1LL))
    return 1;
  if (check_s64("mix", mod_s(7LL, -3LL), 1LL))
    return 1;
  if (check_s64("mix2", mod_s(-7LL, 3LL), -1LL))
    return 1;
  if (check_s64("mix3", mod_s(-7LL, -3LL), -1LL))
    return 1;

  printf("PASS\n");
  return 0;
}
