#include <stdio.h>

int main(void)
{
  unsigned long long a = 0x100000000ULL;
  unsigned long long b = 0x100000000ULL;
  unsigned long long r = a * b;
  printf("mul64wide2=0x%llx\n", r);
  printf("PASS\n");
  return 0;
}
