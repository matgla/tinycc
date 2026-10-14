/* Inferred noreturn must not be published for a weak function: the strong
 * definition that replaces it at link time returns, so the callers' code after
 * the call has to survive.  Companion: bug_weak_inferred_noreturn+.c. */
#include <stdio.h>

int G;
int n;
void ext(int x) { n += x; }

__attribute__((weak, noinline)) void hook(void) { hook(); }
__attribute__((weak, noinline)) void hook2(void) { for (;;) G++; }

__attribute__((noinline)) int caller(int x)
{
  hook();
  ext(x);
  return x + 1;
}

__attribute__((noinline)) int caller2(int x)
{
  hook2();
  ext(x);
  return x + 2;
}

int main(void)
{
  int a = caller(3);
  int b = caller2(4);
  printf("%d %d %d\n", a, b, n);
  return 0;
}
