#include <stdio.h>
__attribute__((noinline)) int s(int x, int y)
{
  int r;
  if (y)
    goto L;
  switch (x)
  {
  case 0: r = 10; break;
  case 1: r = 15; break;
  case 2: L: r = 20; break;
  case 3: r = 30; break;
  case 4: r = 37; break;
  case 5: r = 41; break;
  default: r = 0; break;
  }
  return r;
}
int main(void)
{
  printf("%d %d %d\n", s(0, 1), s(9, 1), s(3, 0));
  return 0;
}
