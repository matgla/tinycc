/* Regression: __builtin_signbit / __builtin_signbitf parsed their argument in
 * "no code" mode (the nc flag used by __builtin_constant_p and
 * __builtin_classify_type, whose operand is never evaluated).  signbit has a
 * run-time path that stores the value and tests bit 31, so the IR computing a
 * non-trivial argument was suppressed and that path tested a stale value
 * instead: `a * b` tested the sign of `a`, `x - 10.0f` tested the sign of `x`,
 * side effects in the argument were dropped, and an argument with no value at
 * all (a call) made the backend die at -O0.
 *
 * Every arm below has a negative result, so a correct signbit is non-zero.
 */
#include <stdio.h>

volatile double va = -2.0, vb = 3.0;
__attribute__((noinline)) double g(void) { return -1.5; }

int main(void)
{
  double a = vb, b = va; /* 3.0 * -2.0 = -6.0 */
  if (__builtin_signbit(a * b) == 0) return 1;

  float fa = 1.0f;
  if (__builtin_signbitf(fa - 10.0f) == 0) return 2;

  /* an argument with no SValue at all used to fail to compile */
  if (__builtin_signbit(g()) == 0) return 3;

  /* side effects in the argument must survive */
  int side = 0;
  double y = -1.0;
  if (__builtin_signbit(y * (side = 1)) == 0) return 4;
  if (side != 1) return 5;

  printf("OK\n");
  return 0;
}
