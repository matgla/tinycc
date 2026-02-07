#include <stdio.h>

int main(void)
{
  double a = 1.5;
  double b = 2.0;
  printf("sum=%.6f\n", a + b);
  printf("diff=%.6f\n", a - b);
  printf("prod=%.6f\n", a * b);
  printf("div=%.6f\n", a / b);
  printf("PASS\n");
  return 0;
}
