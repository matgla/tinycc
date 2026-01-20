#include <stdio.h>

int main(void)
{
  double x = -2.0;
  double y = 4.0;
  double z = (x * y) + (y / 2.0) - 1.0;
  printf("z=%.6f\n", z);
  printf("PASS\n");
  return 0;
}
