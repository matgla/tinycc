#include <stdio.h>

int main(void)
{
  unsigned long long a = 0x123456789ABCDEF0ULL;
  unsigned long long b = 0x10ULL;
  unsigned long long r = a * b;
  printf("mul64wide=0x%llx\n", r);
  printf("PASS\n");
  return 0;
}
