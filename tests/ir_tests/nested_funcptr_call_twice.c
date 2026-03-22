/* nested_funcptr_call_twice.c — Phase 3: Call funcptr twice (chain slot stability) */
#include <stdio.h>

static int apply_twice(int (*fn)(int), int x)
{
  return fn(fn(x));
}

int main(void)
{
  int step = 10;

  int bump(int x)
  {
    return x + step;
  }

  printf("%d\n", apply_twice(bump, 0));
  step = 1;
  printf("%d\n", apply_twice(bump, 100));
  return 0;
}
