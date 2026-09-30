/* nested_capture_two_level.c — a grandchild capturing the grandparent.
 *
 * inner() reaches `e` by hopping: R10 points at mid's frame top, and mid's
 * incoming chain is read from [chain-4].  mid reserved those four bytes
 * (gen_function's `loc -= 4`), but the backend's initial min-local-offset scan
 * raised `loc` back to 0 because the chain slot is not an IR operand and mid
 * has no locals of its own.  mid then stored the chain BELOW its own SP, where
 * inner's own push landed on it.
 */
#include <stdio.h>

__attribute__((noinline)) static int two_level(int a, int b, int c, int d, int e)
{
  __attribute__((noinline)) int mid(void)
  {
    __attribute__((noinline)) int inner(void) { return e * 2; }
    return inner() + 1;
  }
  return mid() + a + b + c + d;
}

/* The same, capturing a grandparent LOCAL, and writing it from the grandchild. */
__attribute__((noinline)) static int two_level_write(int a)
{
  int e = 5;
  __attribute__((noinline)) int mid(void)
  {
    __attribute__((noinline)) void inner(void) { e += 10; }
    inner();
    return e;
  }
  int m = mid();
  return m * 100 + e + a;
}

int main(void)
{
  printf("two_level=%d\n", two_level(1, 2, 3, 4, 5));
  printf("two_level_write=%d\n", two_level_write(1));
  return 0;
}
