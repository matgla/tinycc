#include <stdio.h>

int main(void)
{
  printf("Starting division test\n");

  long long s = -35LL;
  printf("s = %lld\n", s);

  printf("About to divide\n");
  s /= 7LL;
  printf("After division: s = %lld\n", s);

  if (s == -5LL)
  {
    printf("Division OK\n");
    return 0;
  }
  else
  {
    printf("Division FAIL: expected -5, got %lld\n", s);
    return 1;
  }
}
