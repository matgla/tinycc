#include <stdio.h>

int main(void)
{
  unsigned long long a = 10ULL;
  unsigned long long b = 20ULL;
  printf("lt=%d\n", a < b);
  printf("eq=%d\n", a == b);
  printf("gt=%d\n", a > b);
  printf("PASS\n");
  return 0;
}
