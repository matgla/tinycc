/* An unsigned add folded into a signed add must not inherit the signed add's
   no-overflow assumption.  add_reassoc merged `(int)(x + 1U) + 1` into
   `x + 2`, then ssa:cmp_offset_fold folded `x + 2 < x` to false, but
   x = INT_MAX gives INT_MIN + 1 < INT_MAX: true (gcc pr55137, reached once
   main stopped inlining foo). */
#include <stdio.h>

__attribute__((noinline)) int add_then_add(unsigned int x)
{
  return ((int)(x + 1U) + 1) < (int)x;
}

__attribute__((noinline)) int sub_then_sub(unsigned int x)
{
  return ((int)(x - 1U) - 1) > (int)x;
}

__attribute__((noinline)) int add_then_sub(unsigned int x)
{
  return ((int)(x + 3U) - 1) < (int)x;
}

int main(void)
{
  printf("%d %d %d\n", add_then_add(__INT_MAX__), sub_then_sub((unsigned int)__INT_MAX__ + 1U),
         add_then_sub(__INT_MAX__));
  printf("%d %d %d\n", add_then_add(5), sub_then_sub(5), add_then_sub(5));
  return 0;
}
