#include <stdio.h>

static int depth_sum(int n)
{
  if (n <= 0)
    return 0;
  return n + depth_sum(n - 1);
}

int main(void)
{
  int v = depth_sum(5);
  printf("ehabi ok: %d\n", v);
  return 0;
}
