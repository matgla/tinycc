/* nested_funcptr_indirect.c — Phase 3: Nested func pointer passed to another function */
#include <stdio.h>

static int call_fn(int (*fn)(int), int arg)
{
  return fn(arg);
}

int main(void)
{
  int addend = 100;

  int add_it(int x)
  {
    return x + addend;
  }

  printf("%d\n", call_fn(add_it, 5));
  addend = 200;
  printf("%d\n", call_fn(add_it, 5));
  return 0;
}
