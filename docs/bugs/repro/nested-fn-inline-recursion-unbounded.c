/* docs/bugs/nested-fn-inline-recursion-unbounded.md
 * A nested function that calls the nested function enclosing it: at -O2/-Os
 * the inliner expands l1 -> l2 -> l1 -> ... without a cycle or depth bound
 * and the compile does not finish.
 *
 *   timeout 60 ./armv8m-tcc -O2 -c docs/bugs/repro/nested-fn-inline-recursion-unbounded.c
 *
 * Expected (gcc, tcc -O0/-O1): 31        tcc -O2 / -Os: compile never ends */
#include <stdio.h>
__attribute__((noinline)) int multi(int a)
{
  int l1(int y)
  {
    int l2(int z) { return z + (y > 0 ? l1(y - 1) : 0); }
    return l2(y * 10);
  }
  return l1(2) + a;
}
int main(void) { printf("%d\n", multi(1)); return 0; }
