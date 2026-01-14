#include <stdio.h>

int main(void)
{
  unsigned long long v = 0x100000003ULL;

  printf("v = 0x%08x%08x\n", (unsigned)(v >> 32), (unsigned)v);
  printf("expect: v = 0x0000000100000003\n");

  if (v == 0x100000003ULL)
  {
    printf("PASS\n");
  }
  else
  {
    printf("FAIL v=%llu expected %llu\n", v, 0x100000003ULL);
  }

  return 0;
}
