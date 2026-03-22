/* nested_capture_write.c — Phase 2: Nested function writes parent local */
#include <stdio.h>

int main(void)
{
  int x = 10;

  void set_x(int val)
  {
    x = val;
  }

  printf("%d\n", x);
  set_x(42);
  printf("%d\n", x);
  set_x(0);
  printf("%d\n", x);
  return 0;
}
