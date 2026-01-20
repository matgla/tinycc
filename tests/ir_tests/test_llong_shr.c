#include <stdio.h>

int main(void)
{
  unsigned long long v = 0x123456789ABCDEF0ULL;
  printf("shr4=0x%llx\n", v >> 4);
  printf("shr8=0x%llx\n", v >> 8);
  printf("PASS\n");
  return 0;
}
