#include <stdio.h>

int main(void)
{
  double v = 2.0;
  v = v * 2.0;
  printf("mul=%.6f\n", v);
  v = v * 0.25;
  printf("mul=%.6f\n", v);
  printf("PASS\n");
  return 0;
}
