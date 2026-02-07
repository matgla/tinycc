#include <stdio.h>

int main(void)
{
  unsigned long long a = 123456789ULL;
  unsigned long long b = 987654321ULL;
  unsigned long long r = a * b;
  printf("mul_reg=%llu\n", r);
  printf("PASS\n");
  return 0;
}
