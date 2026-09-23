/* nested_capture_param_byref.c — GNU nested functions capture by REFERENCE, and
 * that has to hold for parameters too, in both directions.
 *
 * A captured parameter therefore cannot be snapshotted at the child's
 * declaration: it needs a real home in the parent's frame that every parent
 * read and write goes through, so a store on either side is seen by the other.
 */
#include <stdio.h>

/* The parent writes a captured stack-passed parameter after declaring the
 * child; the child must see the new value. */
__attribute__((noinline)) static int wr_parent(int a, int b, int c, int d, int e)
{
  __attribute__((noinline)) int get(void) { return e; }
  int before = get();
  e = 100;
  return before * 1000 + get() + a + b + c + d;
}

/* The child writes a captured stack-passed parameter; the parent must see it. */
__attribute__((noinline)) static int wr_child(int a, int b, int c, int d, int e)
{
  __attribute__((noinline)) void bump(int by) { e += by; }
  bump(7);
  bump(7);
  return e + a + b + c + d;
}

/* The same, for register-passed parameters and a noinline child. */
__attribute__((noinline)) static int wr_reg(int a, int b)
{
  __attribute__((noinline)) int f(void) { a += b; return a; }
  int x = f();
  b = 100;
  return x * 1000 + f();
}

int main(void)
{
  printf("wr_parent=%d\n", wr_parent(1, 2, 3, 4, 5));
  printf("wr_child=%d\n", wr_child(1, 2, 3, 4, 5));
  printf("wr_reg=%d\n", wr_reg(7, 9));
  return 0;
}
