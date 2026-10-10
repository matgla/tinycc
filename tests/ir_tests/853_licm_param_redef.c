/* A loop that redefines a parameter must not have the (invariant) redefinition
 * hoisted above the first iteration's read of the incoming argument. */
#include <stdio.h>

volatile int vn = 3, vp = 100;

__attribute__((noinline)) int lp3(int p, int q, int n)
{
  int s = 0;
  int i = 0;
  do
  {
    s += p;
    p = q + 1;
    i++;
  } while (i < n);
  return s;
}

int main(void)
{
  printf("lp3 %d\n", lp3(vp, 4, vn));
  return 0;
}
