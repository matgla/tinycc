#include <stdio.h>

extern double __aeabi_ddiv(double a, double b);

int main(void)
{
  double r1 = __aeabi_ddiv(6.0, 3.0);
  double r2 = __aeabi_ddiv(5.0, 2.0);
  printf("lib 6/3=%.6f\n", r1);
  printf("lib 5/2=%.6f\n", r2);
  printf("PASS\n");
  return 0;
}
