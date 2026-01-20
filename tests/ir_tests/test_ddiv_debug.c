#include <stdio.h>

static void print_div(const char *label, double a, double b)
{
  printf("%s %.6f\n", label, a / b);
}

int main(void)
{
  print_div("1.5/2.0=", 1.5, 2.0);
  print_div("10.0/4.0=", 10.0, 4.0);
  print_div("7.0/2.0=", 7.0, 2.0);
  printf("PASS\n");
  return 0;
}
