#include <stdio.h>

int main(void)
{
  double v = 1.0;
  for (int i = 1; i <= 5; ++i)
  {
    v *= 1.5;
    printf("step%d=%.6f\n", i, v);
  }
  printf("PASS\n");
  return 0;
}
