/* Regression: __builtin_constant_p(x) answered 1 for a local whose vreg had
 * exactly one definition assigning an immediate - among the instructions
 * emitted SO FAR.  A definition later in the loop body reaches the call on
 * the next trip through the back edge, so x is not fixed: the builtin must
 * report 0 there.  The body is parsed once, so the wrong 1 was baked into
 * both trips and the loop produced 11 instead of 00 / 10.
 *
 * The second trip must contribute 0; the first may report 0 or 1 (x really
 * is 5 on it).  A literal argument is still reported constant, so the last
 * arm guards against over-fixing the builtin to 0.
 */
#include <stdio.h>

volatile int v = 42;

int main(void)
{
  int x = 5, r = 0;
  for (int i = 0; i < 2; i++) {
    r = r * 10 + __builtin_constant_p(x);
    x = v;
  }
  if (r != 0 && r != 10) return 1;

  if (!__builtin_constant_p(7 + 1)) return 2;

  printf("OK\n");
  return 0;
}
