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

static long long add_s(long long a, long long b)
{
  return a + b;
}

static long long sub_s(long long a, long long b)
{
  return a - b;
}

int main(void)
{
  printf("Testing signed long long add/sub\n");

  if (check_s64("carry32", add_s(0x00000000ffffffffLL, 2LL), 0x0000000100000001LL))
    return 1;
  if (check_s64("hiword", add_s((1LL << 32), 5LL), (1LL << 32) + 5LL))
    return 1;
  if (check_s64("neg+pos", add_s(-(1LL << 40), 123456789LL), -(1LL << 40) + 123456789LL))
    return 1;
  if (check_s64("sub32", sub_s(0x0000000100000000LL, 1LL), 0x00000000ffffffffLL))
    return 1;
  if (check_s64("subneg", sub_s(-5LL, 7LL), -12LL))
    return 1;

  printf("PASS\n");
  return 0;
}
