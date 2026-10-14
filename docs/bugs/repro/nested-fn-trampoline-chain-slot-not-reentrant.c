/* docs/bugs/nested-fn-trampoline-chain-slot-not-reentrant.md
 * Each activation of rec makes a trampoline for its own get; every call
 * through one reads the frame of the latest activation.
 * Expected (gcc): 60    tcc, every level: 0 */
#include <stdio.h>
__attribute__((noinline)) int call(int (*f)(void)) { return f(); }
__attribute__((noinline)) int rec(int d)
{
  int mine = d;
  int get(void) { return mine; }
  int (*fp)(void) = get;
  int below = d > 0 ? rec(d - 1) : 0;
  return call(fp) * 10 + below;
}
int main(void) { printf("%d\n", rec(3)); return 0; }
