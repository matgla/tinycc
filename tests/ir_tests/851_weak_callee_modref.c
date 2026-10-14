/* A weak definition may be replaced by a strong one in another TU, so the
 * mod-ref summary of its (empty) body must not be trusted. */
#include <stdio.h>
struct S { int a, b, c, d, e; };
struct S GS;
__attribute__((weak, noinline)) void helper(void) {}
__attribute__((noinline)) int test(void)
{
  struct S x = GS;
  helper();
  return x.e != GS.e;
}
int main(void)
{
  printf("test %d\n", test());
  return 0;
}
