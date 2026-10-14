/* ssa:sccp assumed a variable-index store into a frame array reaches only its
 * first 64 bytes, so a load of a later element folded to the value stored
 * before the (possibly aliasing) indexed store. */
#include <stdio.h>

__attribute__((noinline)) int k(int n)
{
  int a[40];
  a[30] = 1;
  for (int i = 0; i < n; i++)
    a[i] = i;
  return a[30];
}

__attribute__((noinline)) int h3(int d, int i)
{
  int a[40];
  a[30] = 1;
  if (d)
    a[i] = 9;
  return a[30];
}

__attribute__((noinline)) int h2(int c, int d, int i)
{
  int a[40];
  int *p = &a[2];
  if (c)
  {
    a[30] = 1;
    if (d)
      p[i] = 9;
    return a[30];
  }
  return 0;
}

int main(void)
{
  printf("%d %d %d\n", k(40), h3(1, 30), h2(1, 1, 28));
  return 0;
}
