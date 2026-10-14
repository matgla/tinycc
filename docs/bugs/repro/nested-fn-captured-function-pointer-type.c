/* docs/bugs/nested-fn-captured-function-pointer-type.md
 * A nested function calling through its parent's function-pointer local
 * fails to compile: "too few arguments to function" (every level).
 * Expected (gcc): 8 */
#include <stdio.h>
int g(int x) { return x + 1; }
__attribute__((noinline)) int f4(int k)
{
  int (*sp)(int) = g;
  int n(int y) { return sp(y) + k; }
  return n(4);
}
int main(void) { printf("%d\n", f4(3)); return 0; }
