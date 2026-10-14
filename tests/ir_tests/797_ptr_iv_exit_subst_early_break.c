/* ptr_iv_exit_subst replaced reads of the pointer IV after the loop with the
 * full-trip address even when a break reaches the same exit target with a
 * smaller pointer. */
#include <stdio.h>
int __attribute__((noinline)) f(const int *src)
{
  int buf[16];
  int *p = buf;
  for (int i = 0; i < 10; i++) {
    if (src[i] < 0)
      break;
    *p = src[i];
    p++;
  }
  return (int)(p - buf);
}
int main(void)
{
  int a[10] = {1, 2, 3, -1, 5, 6, 7, 8, 9, 10};
  printf("count=%d\n", f(a));
  return 0;
}
