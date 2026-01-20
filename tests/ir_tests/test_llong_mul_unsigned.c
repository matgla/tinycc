#include <stdio.h>

static int check_u64(const char *name, unsigned long long got, unsigned long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%llx exp=%llx\n", name, got, exp);
    return 1;
  }
  return 0;
}

static unsigned long long mul_u(unsigned long long a, unsigned long long b)
{
  return a * b;
}

int main(void)
{
  printf("Testing unsigned long long mul\n");

  if (check_u64("cross32", mul_u((1ULL << 32), 10ULL), (1ULL << 32) * 10ULL))
    return 1;
  if (check_u64("ffff*ffff", mul_u(0xffffffffULL, 0xffffffffULL), 0xfffffffe00000001ULL))
    return 1;
  if (check_u64("hi+lo", mul_u(0x100000003ULL, 7ULL), 0x700000015ULL))
    return 1;

  printf("PASS\n");
  return 0;
}
