/* dse deleted __builtin_setjmp when its result was unused; the longjmp then
 * jumped through an unset buffer (hard fault at -O1+). */
#include <stdio.h>
static void *jb[5];
__attribute__((noinline)) void jump(void) { __builtin_longjmp(jb, 1); }
__attribute__((noinline)) int f(void)
{
  static volatile int n;
  n = 0;
  __builtin_setjmp(jb); /* result unused */
  if (++n < 3)
    jump();
  return n;
}
int main(void)
{
  printf("%d\n", f());
  return 0;
}
