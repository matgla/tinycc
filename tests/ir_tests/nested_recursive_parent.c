/* nested_recursive_parent.c — Phase 3: Recursive parent calls nested function */
#include <stdio.h>

int factorial_with_nested(int n)
{
  int result = 1;

  void accumulate(void)
  {
    result *= n;
  }

  if (n > 1)
  {
    accumulate();
    result = factorial_with_nested(n - 1) * n;
  }
  return result > 0 ? result : 1;
}

int main(void)
{
  /* Each recursive call has its own stack frame and 'result'. */
  printf("%d\n", factorial_with_nested(1));
  printf("%d\n", factorial_with_nested(5));
  return 0;
}
