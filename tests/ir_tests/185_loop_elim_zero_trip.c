/* Regression: symbolic-limit loop elimination must guard the zero-trip case.
 *
 * try_eliminate_loop_symbolic's fallback wrote an UNCONDITIONAL closed form
 * (counter = limit; acc = limit*step) for a symbolic limit, ignoring that a
 * top-tested `while`/`for` with limit <= init runs ZERO times.  So
 * `i = 0; while (i < n) i++; return i` was miscompiled to `return n` instead of
 * `max(n, 0)`, and an accumulator was returned as `limit*step` instead of 0.
 * The functions are non-static so the optimizer can't specialise them to the
 * call-site constants; the limit stays symbolic and the elimination fires.
 */
#include <stdio.h>

/* noinline so the symbolic-limit elimination runs on the function body itself
 * (an inlined copy would specialise to the call-site value and hide the bug). */
__attribute__((noinline)) int count(int n)
{
  int i = 0;
  while (i < n)
    i++;
  return i;
}

__attribute__((noinline)) int sum_to(int n)
{
  int s = 0, i;
  for (i = 0; i < n; i++)
    s += i;
  return s;
}

int main(void)
{
  /* volatile so the args are not constant-folded into the calls. */
  volatile int neg = -5, zero = 0, pos = 4;
  printf("count: %d %d %d\n", count(neg), count(zero), count(pos));
  printf("sum: %d %d %d\n", sum_to(neg), sum_to(zero), sum_to(pos));
  return 0;
}
