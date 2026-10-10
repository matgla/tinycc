#include <stdio.h>
__attribute__((noinline)) int f(int a) { switch (!a) { case 0: return 10; case 1: return 20; } return 0; }
__attribute__((noinline)) int g(int a) { switch (a == 3) { case 1: return 30; default: return 40; } }
__attribute__((noinline)) int h(int a, int b) { switch (a < b) { case 0: return 10; case 1: return 20; } return 0; }
__attribute__((noinline)) int k(int a, int b) { switch (a && b) { case 0: return 30; default: return 40; } }
int main(void)
{
  printf("%d %d %d %d %d %d %d\n", f(0), f(5), g(3), g(1), h(1, 2), h(2, 1), k(0, 1));
  return 0;
}
