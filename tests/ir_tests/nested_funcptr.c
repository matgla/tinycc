/* nested_funcptr.c — Phase 3: Address-of nested function, call via pointer */
#include <stdio.h>

int main(void)
{
  int factor = 10;

  int multiply(int x)
  {
    return x * factor;
  }

  int (*fp)(int) = multiply;

  printf("%d\n", fp(5));
  factor = 3;
  printf("%d\n", fp(5));
  return 0;
}
