#include <stdio.h>

int main(void)
{
  unsigned long long a = 0x100000002ULL;
  unsigned long long b = 0x300000004ULL;
  unsigned long long r = a * b;
  printf("mul_parts=0x%llx\n", r);
  printf("PASS\n");
  return 0;
}
