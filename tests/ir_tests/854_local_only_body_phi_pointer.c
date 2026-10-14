/* q is the frame buffer on one path and the caller's pointer on the other:
 * the store through q is not local-only. */
#include <stdio.h>

__attribute__((noinline)) void f(int *p, int c)
{
  int buf[4];
  int *q;
  if (c)
    q = buf;
  else
    q = p;
  *q = 5;
}

int main(void)
{
  int x = 1;
  f(&x, 0);
  printf("%d\n", x);
  return 0;
}
