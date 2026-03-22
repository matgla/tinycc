/* nested_shadowing.c — Phase 2: Nested function shadows parent variable name */
#include <stdio.h>

int main(void)
{
  int x = 10;

  int shadow_test(int x)
  {
    /* This 'x' is the parameter, NOT the parent's 'x'. */
    return x + 1;
  }

  printf("%d\n", shadow_test(5));
  printf("%d\n", x); /* parent's x unchanged */

  /* Also test a nested function that captures parent x
     AND has its own local x. */
  int capture_and_shadow(void)
  {
    int x = 99; /* local x shadows captured x */
    return x;   /* should be 99, not 10 */
  }

  printf("%d\n", capture_and_shadow());
  printf("%d\n", x); /* parent's x still unchanged */
  return 0;
}
