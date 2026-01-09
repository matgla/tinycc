#include <stdio.h>

void pass_many_args(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l, int m)
{
  printf("%d %d %d %d %d %d %d %d %d %d %d %d %d\n", a, b, c, d, e, f, g, h, i, j, k, l, m);
  return;
}

int main(void)
{
  pass_many_args(13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);

  return 0;
}
