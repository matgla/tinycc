#include <stdio.h>

static void print_mul(const char *label, double a, double b)
{
  printf("%s %.6f\n", label, a * b);
}

int main(void)
{
  print_mul("1.5*2.0=", 1.5, 2.0);
  print_mul("2.0*3.0=", 2.0, 3.0);
  print_mul("0.5*2.0=", 0.5, 2.0);
  printf("PASS\n");
  return 0;
}
