#include <stdio.h>

int main(void)
{
  int sum = 0;
  for (int i = 1; i <= 10; ++i)
    sum += i;
  printf("sum=%d\n", sum);
  printf("PASS\n");
  return 0;
}
