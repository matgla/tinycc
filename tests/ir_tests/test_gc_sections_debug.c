#include <stdio.h>

static int used_fn(int v)
{
  return v * 2;
}

static int unused_fn(int v)
{
  return v * 3;
}

int main(void)
{
  int v = used_fn(7);
  printf("used=%d\n", v);
  printf("PASS\n");
  return 0;
}
