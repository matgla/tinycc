/* A pointer VAR walked over a local array has two definitions -- the start
 * and the increment -- and the loads through it must read each element.
 * entry_store_prop counted definitions skipping every lvalue destination,
 * which also skips a VAR's own (a STACKOFF lvalue), took the walker for a
 * single-definition pointer and forwarded the last entry-block store into
 * every iteration: 3+4 summed as 4+4. */
#include <stdio.h>

__attribute__((noinline)) static int walk(void)
{
  int x[2] = {1, 2};
  char gap[64] __attribute__((unused));
  int y[2] = {3, 4};
  const int *e = y + 2;
  int s = 0;
  for (const int *p = y; p != e; p++)
    s += *p;
  return s * 100 + x[0] * 10 + x[1];
}

__attribute__((noinline)) static int walk_back(void)
{
  int y[2] = {7, 9};
  int s = 0;
  const int *p = &y[1];
  const int *const lo = y;
  while (p >= lo)
  {
    s = s * 10 + *p;
    p--;
  }
  return s;
}

int main(void)
{
  printf("walk=%d back=%d\n", walk(), walk_back());
  return 0;
}
