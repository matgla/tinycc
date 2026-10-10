/* A struct-returning weak function is not pure-except-for-sret: the strong
 * definition may have side effects, so a call whose result is unused stays.
 * Companion: bug_weak_pure_via_sret+.c. */
#include <stdio.h>

struct R { int a, b, c, d; };
int cnt;

__attribute__((weak, noinline)) struct R wk(int x)
{
  struct R r = {x, x, x, x};
  return r;
}

__attribute__((noinline)) int use3(int x)
{
  struct R t = wk(x);
  (void)t;
  return 3;
}

int main(void)
{
  int r = use3(1);
  printf("%d %d\n", r, cnt);
  return 0;
}
