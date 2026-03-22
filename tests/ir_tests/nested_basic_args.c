/* nested_basic_args.c — Phase 1: Nested function with multiple parameters */
#include <stdio.h>

int main(void)
{
  int add(int a, int b)
  {
    return a + b;
  }
  int mul(int a, int b)
  {
    return a * b;
  }

  printf("%d\n", add(3, 4));
  printf("%d\n", mul(6, 7));
  printf("%d\n", add(mul(2, 3), mul(4, 5)));
  return 0;
}
