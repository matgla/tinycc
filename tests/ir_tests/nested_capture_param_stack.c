/* nested_capture_param_stack.c — a nested function capturing a STACK-passed
 * parent parameter.
 *
 * The 5th int argument arrives in the caller's argument area, at an offset
 * measured from the parent's offset_to_args — a position the child cannot name,
 * because the children are compiled before the parent's prologue fixes it.  The
 * parameter had no home in the parent's own frame, so the capture resolved to
 * chain offset 0 and add() read `d` instead of `e` (24 instead of 25).
 * Fixed by giving a captured parameter a frame home the prologue writes into.
 */
#include <stdio.h>

__attribute__((noinline)) static int stk_cap(int a, int b, int c, int d, int e)
{
  int add(int x) { return x + e; }
  return add(10) + a + b + c + d;
}

int main(void)
{
  printf("stk_cap=%d\n", stk_cap(1, 2, 3, 4, 5));
  return 0;
}
