#include <stdio.h>

static int add(int a, int b)
{
  return a + b;
}

static int sub(int a, int b)
{
  return a - b;
}

int main(void)
{
  int v1 = add(10, 5);
  int v2 = sub(10, 5);
  printf("add=%d sub=%d\n", v1, v2);
  printf("PASS\n");
  return 0;
}
