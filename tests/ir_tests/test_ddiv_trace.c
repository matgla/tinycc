#include <stdio.h>

int main(void)
{
  double a = 1.0;
  for (int i = 1; i <= 5; ++i)
  {
    a = a / 2.0;
    printf("step%d=%.6f\n", i, a);
  }
  printf("PASS\n");
  return 0;
}
