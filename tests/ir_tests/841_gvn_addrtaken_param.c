/* ssa:gvn: a PARAM whose address is taken can be written through the pointer,
 * so `a + c` computed before the write must not be reused after it
 * (docs/bugs/ssa-gvn-address-taken-param-treated-stable, fixed). */
#include <stdio.h>

__attribute__((noinline)) void ext(int *p) { *p = 1000; }

__attribute__((noinline)) int f(int a, int c)
{
  int x = a + c;
  ext(&a);
  int y = a + c;
  return x * 100 + y;
}

__attribute__((noinline)) int g(int a, int c)
{
  int x = a + c;
  int *p = &a;
  if (c)
    *p = 7;
  int y = a + c;
  return x * 100 + y;
}

int main(void)
{
  printf("%d %d\n", f(1, 1), g(1, 1));
  printf("%d %d\n", f(2, 0), g(2, 0));
  return 0;
}
