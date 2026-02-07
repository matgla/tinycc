#include <stdio.h>

int main(void)
{
  volatile double a = 2.0;
  volatile double b = 0.5;
  volatile double c = a * b + 1.0;
  if (c < 1.9 || c > 2.1)
  {
    printf("FAIL\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
