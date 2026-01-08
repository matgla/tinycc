#include <stdio.h>

static int check_u64(const char *name, unsigned long long got, unsigned long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%llu exp=%llu\n", name, got, exp);
    return 1;
  }
  return 0;
}

static unsigned long long div_u(unsigned long long a, unsigned long long b)
{
  return a / b;
}

int main(void)
{
  printf("Testing unsigned long long div\n");

  if (check_u64("10e10/10", div_u(10000000000ULL, 10ULL), 1000000000ULL))
    return 1;
  if (check_u64("allones/ffff", div_u(0xffffffffffffffffULL, 0xffffffffULL), 0x100000001ULL))
    return 1;
  if (check_u64("hi/2", div_u(0x8000000000000000ULL, 2ULL), 0x4000000000000000ULL))
    return 1;

  printf("PASS\n");
  return 0;
}
