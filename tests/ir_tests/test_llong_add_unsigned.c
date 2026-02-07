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

static unsigned long long add_u(unsigned long long a, unsigned long long b)
{
  return a + b;
}

int main(void)
{
  printf("Testing unsigned long long add\n");

  if (check_u64("carry32", add_u(0xffffffffULL, 1ULL), 0x100000000ULL))
    return 1;
  if (check_u64("wrap64", add_u(0xffffffffffffffffULL, 1ULL), 0ULL))
    return 1;
  if (check_u64("wrapcarry", add_u(0x8000000000000000ULL, 0x8000000000000000ULL), 0ULL))
    return 1;
  if (check_u64("hiword", add_u(0x100000000ULL, 5ULL), 0x100000005ULL))
    return 1;

  printf("PASS\n");
  return 0;
}
