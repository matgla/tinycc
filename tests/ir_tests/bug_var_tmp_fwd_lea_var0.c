/* var_tmp_fwd sized its "VAR has a LEA" bitmap with 0 meaning none, so an
 * address-taken V0 (first local) was never guarded.  Fix: -1 sentinel. */
#include <stdio.h>

__attribute__((noinline)) int h(int *q, int a)
{
  int x;
  int *p = &x;
  if (q)
    p = q;
  x = a * 3 + 1;
  *p = 7;
  return x + 1;
}

int main(void)
{
  int y = 0;
  printf("%d %d\n", h(0, 4), h(&y, 4));
  return 0;
}
