#include <stdio.h>

int main(void)
{
  int i = 3;
  double x = 2.5;
  double y = x * i + 0.5;
  printf("i=%d x=%.6f y=%.6f\n", i, x, y);
  printf("PASS\n");
  return 0;
}
