/* nested_capture_read.c — Phase 2: Nested function reads parent local */
#include <stdio.h>

int main(void)
{
  int x = 42;

  int get_x(void)
  {
    return x;
  }

  printf("%d\n", get_x());

  x = 99;
  printf("%d\n", get_x());
  return 0;
}
