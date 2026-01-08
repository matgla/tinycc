#include <stdio.h>

int main()
{
  unsigned long long u = 8;
  printf("u = %llu\n", u);
  u *= 7;
  printf("u *= 7 = %llu\n", u);
  if (u == 56)
  {
    printf("PASS\n");
    return 0;
  }
  else
  {
    printf("FAIL: expected 56, got %llu\n", u);
    return 1;
  }
}
