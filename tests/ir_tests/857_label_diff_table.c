/* &&l1 - &&l0 in a static initializer: only the difference itself is a
 * load-time value, and it must not leak into later initializers. */
#include <stdio.h>

__attribute__((noinline)) int f(int x)
{
  static const int tab[] = { 0, &&l1 - &&l0, &&l2 - &&l0 };
  goto *(&&l0 + tab[x]);
l0:
  return 10;
l1:
  return 20;
l2:
  return 30;
}

__attribute__((noinline)) int h(int x)
{
  static const short tab[] = { 0, &&l1 - &&l0, &&l2 - &&l0 };
  goto *(&&l0 + tab[x]);
l0:
  return 10;
l1:
  return 20;
l2:
  return 30;
}

__attribute__((noinline)) int g(int x)
{
  static const int tab[] = { 0, 4, 8 };
  return tab[x] + 1;
}

int main(void)
{
  printf("f: %d %d %d\n", f(0), f(1), f(2));
  printf("h: %d %d %d\n", h(0), h(1), h(2));
  printf("g: %d %d %d\n", g(0), g(1), g(2));
  return 0;
}
