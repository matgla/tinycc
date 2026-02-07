#include <stdio.h>

int main()
{
  long long s = -35LL;
  printf("s = %lld\n", s);
  printf("About to divide\n");
  s /= 7LL;
  printf("After /=7: s = %lld\n", s);
  return 0;
}
