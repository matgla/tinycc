/* Regression: __builtin_fabs / __builtin_fmax take a double parameter, so a
 * non-floating argument must be converted to it before the builtin folds its
 * constant or calls the libm helper.  unary_builtin_fp2 widened only a float
 * operand, so an int / long long argument reached __fabs raw: the integer
 * value arrived in r0 with r1 left over from earlier (garbage high word), and
 * a constant integer was not folded at all.
 *
 * The volatile globals force the runtime path; the literal forms pin folding.
 * Results are compared numerically so the test does not depend on how printf
 * renders a double.
 */
#include <stdio.h>

volatile int vi = -3, vp = 3;
volatile long long vl = -5;
volatile double vd = 2.5;

int main(void)
{
  int i = vi, p = vp;
  long long l = vl;
  double d = vd;

  /* fabs of an integer argument: the value, not its bit pattern. */
  if (__builtin_fabs(i) != 3.0) return 1;
  if (__builtin_fabs(-7) != 7.0) return 2;
  if (__builtin_fabs(l) != 5.0) return 3;

  /* fabsf takes float: an int argument must be converted to float. */
  if (__builtin_fabsf(i) != 3.0f) return 4;

  /* fmax / fmin: every operand is a double parameter.  The integer operand
   * decides the result here, so an unconverted argument cannot answer right. */
  if (__builtin_fmax(p, d) != 3.0) return 5;
  if (__builtin_fmin(i, d) != -3.0) return 6;
  if (__builtin_fmax(-4, 2.0) != 2.0) return 7;

  printf("OK\n");
  return 0;
}
