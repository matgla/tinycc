#include <stdio.h>

int main(void)
{
  unsigned long long v = 1ULL;
  v <<= 40;
  printf("shl40=0x%llx\n", v);
  v >>= 8;
  printf("shr8=0x%llx\n", v);
  printf("PASS\n");
  return 0;
}
