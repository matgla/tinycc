/* entry_store_prop forwarded an entry-block constant across an inline-asm
 * "=m" output naming the same slot.  Fix: any asm/nonlocal writer after the
 * entry block invalidates every entry-stored slot. */
#include <stdio.h>

__attribute__((noinline)) int f(int c)
{
  int x[2];
  x[0] = 5;
  x[1] = 0;
  if (c)
    __asm__ volatile("movs r3, #9\n\tstr r3, %0" : "=m"(x[0]) : : "r3");
  return x[0];
}

__attribute__((noinline)) int g(int c)
{
  int x[2];
  x[0] = 5;
  x[1] = 7;
  if (c)
    x[1] = 3;
  return x[0] + x[1];
}

int main(void)
{
  printf("%d %d\n", f(1), f(0));
  printf("%d %d\n", g(1), g(0));
  return 0;
}
