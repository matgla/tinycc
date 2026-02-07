#include <stdio.h>

int main(void)
{
  unsigned int a = 12345U;
  unsigned int b = 6789U;
  unsigned int r = a * b;
  printf("mul32=%u\n", r);
  printf("PASS\n");
  return 0;
}
