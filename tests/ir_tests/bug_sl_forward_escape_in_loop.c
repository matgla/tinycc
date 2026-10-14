#include <stdio.h>
int *gp;
__attribute__((noinline)) void ext(void)
{
  if (gp)
    *gp = 42;
}
__attribute__((noinline)) int f(int n)
{
  int x[2];
  int r = 0;
  for (int i = 0; i < n; i++)
  {
    x[0] = 1;
    ext();
    r += x[0];
    gp = x;
  }
  gp = 0;
  return r;
}
int main(void)
{
  printf("%d\n", f(2));
  return 0;
}
