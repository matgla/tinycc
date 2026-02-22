/* nested_multiple.c — Phase 1+2: Multiple nested functions in one parent */
#include <stdio.h>

int main(void)
{
  int base = 100;

  int inc(int x)
  {
    return base + x;
  }
  int dec(int x)
  {
    return base - x;
  }

  printf("%d\n", inc(5));
  printf("%d\n", dec(5));

  base = 200;
  printf("%d\n", inc(5));
  printf("%d\n", dec(5));
  return 0;
}
