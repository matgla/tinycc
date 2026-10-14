/* A guard head (docs/inline_guard_heads.md) is built from the callee's
 * tokens after its scope is gone.  A parameter or local named like a
 * file-scope typedef made the scanner read the no-effect statement `T * x;`
 * as a declaration of a local x, so `x = x + T;` -- a write to the GLOBAL x
 * -- passed as an assignment to a head local and was replayed in the caller:
 * when the test held, x was incremented in the head and again by the call. */
#include <stdio.h>

typedef int T;
int x, y;
unsigned hits;
__attribute__((noinline)) void rec(int v) { hits = hits * 3 + v; }
#define WORK(v)                                                                                                        \
  rec(v);                                                                                                              \
  rec(1);                                                                                                              \
  rec(2);                                                                                                              \
  rec(3);                                                                                                              \
  rec(4);                                                                                                              \
  rec(5);                                                                                                              \
  rec(6);                                                                                                              \
  rec(7);                                                                                                              \
  rec(8)

/* A local shadows the typedef. */
static void bump(int *p)
{
  int T = 1;
  T *x;
  x = x + T;
  if (*p)
  {
    WORK(*p);
    *p = 0;
  }
}

/* A parameter shadows the typedef. */
static void bump2(int *p, int T)
{
  T *y;
  y = y + T;
  if (*p)
  {
    WORK(*p);
    *p = 0;
  }
}

int main(void)
{
  int a = 3, b = 0;
  bump(&a);
  bump(&b);
  bump(&a);
  bump(&b);
  printf("x=%d hits=%u a=%d\n", x, hits, a);
  a = 5;
  bump2(&a, 2);
  bump2(&b, 2);
  bump2(&a, 2);
  printf("y=%d hits=%u a=%d\n", y, hits, a);
  return 0;
}
