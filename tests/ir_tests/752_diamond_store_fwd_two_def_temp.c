/* ssa:diamond_store_fwd made a two-def TEMP without a phi that sccp folded to
 * the else constant; volatile merge loads/stores were forwarded as well. */
#include <stdio.h>
__attribute__((noinline)) int g(int *r, int i, int c, int k)
{
  k = k * 3 + i;
  if (c)
    r[i] = 1;
  else
    r[i] = 2;
  return r[i] + k;
}
__attribute__((noinline)) int gv(volatile int *r, int i, int c, int k)
{
  k = k * 3 + i;
  if (c)
    r[i] = 1;
  else
    r[i] = 2;
  return r[i] + k;
}
int main(void)
{
  int buf[2] = {0, 0};
  printf("%d %d\n", g(buf, 0, 1, 0), g(buf, 1, 0, 2));
  printf("%d %d\n", gv(buf, 0, 1, 0), gv(buf, 1, 0, 2));
  return 0;
}
