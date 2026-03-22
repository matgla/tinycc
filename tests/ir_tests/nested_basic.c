/* nested_basic.c — Phase 1: Simplest nested function, direct call, no capture */
#include <stdio.h>

int main(void)
{
  int add1(int x)
  {
    return x + 1;
  }

  printf("%d\n", add1(41));
  printf("%d\n", add1(0));
  printf("%d\n", add1(-1));
  return 0;
}
