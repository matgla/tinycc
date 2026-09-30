/* nested_capture_param_mixed.c — several stack-passed parameters captured at
 * once, and a captured parameter the parent reads as well.
 *
 * The parent's own reads have to go through the frame home too: reading them as
 * parameters would add offset_to_args to an offset that is already
 * frame-relative, landing in the saved-register area.
 */
#include <stdio.h>

__attribute__((noinline)) static int multi(int a, int b, int c, int d, int e, short f, char g)
{
  __attribute__((noinline)) int sum(void) { return e + f + g; }
  return sum() * 10 + a + b + c + d;
}

__attribute__((noinline)) static int both_read(int a, int b, int c, int d, int e)
{
  __attribute__((noinline)) int h(void) { return e; }
  int s = 0;
  for (int i = 0; i < 3; i++)
    s += e + h();
  return s + a + b + c + d;
}

int main(void)
{
  printf("multi=%d\n", multi(1, 2, 3, 4, 5, 6, 7));
  printf("both_read=%d\n", both_read(1, 2, 3, 4, 5));
  return 0;
}
