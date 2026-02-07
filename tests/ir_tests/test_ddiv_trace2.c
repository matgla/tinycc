#include <stdio.h>

int main(void)
{
  double v = 10.0;
  for (int i = 1; i <= 3; ++i)
  {
    v = v / 3.0;
    printf("iter%d=%.6f\n", i, v);
  }
  printf("PASS\n");
  return 0;
}
