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

static unsigned long long mod_u(unsigned long long a, unsigned long long b)
{
  return a % b;
}

int main(void)
{
  printf("Testing unsigned long long mod\n");

  if (check_u64("10e10+1", mod_u(10000000001ULL, 10ULL), 1ULL))
    return 1;
  if (check_u64("allones%ffff", mod_u(0xffffffffffffffffULL, 0xffffffffULL), 0ULL))
    return 1;
  if (check_u64("odd%2", mod_u(0x8000000000000001ULL, 2ULL), 1ULL))
    return 1;

  printf("PASS\n");
  return 0;
}
