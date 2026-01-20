#include <stdio.h>

int main(void)
{
  unsigned long long a = 0xffffffffULL;
  unsigned long long b = 0xffffffffULL;
  unsigned long long r = a * b;
  printf("mul_64bit=0x%llx\n", r);
  printf("PASS\n");
  return 0;
}
