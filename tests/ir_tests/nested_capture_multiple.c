/* nested_capture_multiple.c — Phase 2: Multiple captured variables */
#include <stdio.h>

int main(void)
{
  int a = 1, b = 2, c = 3;

  int sum(void)
  {
    return a + b + c;
  }
  void rotate(void)
  {
    int t = a;
    a = b;
    b = c;
    c = t;
  }

  printf("%d %d %d sum=%d\n", a, b, c, sum());
  rotate();
  printf("%d %d %d sum=%d\n", a, b, c, sum());
  rotate();
  printf("%d %d %d sum=%d\n", a, b, c, sum());
  return 0;
}
