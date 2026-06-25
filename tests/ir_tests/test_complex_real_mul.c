/* Regression test for scalar × _Complex multiplication.
 *
 * Before the fix, `double * _Complex double` (and the reverse) silently
 * materialised the scalar as (scalar, scalar) instead of (scalar, 0).
 * The full complex×complex multiplication formula then produced wrong
 * results in both the real and imaginary parts because the implicit
 * imaginary half was read from uninitialised memory after the DSE pass
 * killed the (non-existent) imaginary store.
 *
 * Covers: scalar*complex, complex*scalar, runtime values, constant folding,
 * and the gcc.c-torture pr49644 pattern of repeated loop multiplication.
 */
#include <stdio.h>

static int fail_count = 0;

static void check(const char *what, double got_r, double got_i, double want_r, double want_i)
{
  if (got_r != want_r || got_i != want_i)
  {
    printf("FAIL %s: got %.6f+%.6fi, want %.6f+%.6fi\n", what, got_r, got_i, want_r, want_i);
    fail_count++;
  }
}

static _Complex double mul_lhs(double d, _Complex double s) { return d * s; }
static _Complex double mul_rhs(_Complex double s, double d) { return s * d; }

int main(void)
{
  /* scalar * complex */
  _Complex double s = 3.0 + 1.0i;
  _Complex double r1 = 2.0 * s;
  check("2.0 * (3+1i)", __real__ r1, __imag__ r1, 6.0, 2.0);

  /* complex * scalar */
  _Complex double r2 = s * 4.0;
  check("(3+1i) * 4.0", __real__ r2, __imag__ r2, 12.0, 4.0);

  /* through a function (forces runtime values, no folding) */
  _Complex double r3 = mul_lhs(2.5, s);
  check("mul_lhs(2.5, 3+1i)", __real__ r3, __imag__ r3, 7.5, 2.5);

  _Complex double r4 = mul_rhs(s, 0.5);
  check("mul_rhs(3+1i, 0.5)", __real__ r4, __imag__ r4, 1.5, 0.5);

  /* pr49644 pattern: array of doubles times a complex constant, in a loop.
   * The verification reads back via printf so we don't trip the unrelated
   * __imag__ a[i]-with-variable-index frontend bug when used as a typed
   * function argument. */
  _Complex double a[6];
  double b[6] = {1, 2, 3, 4, 5, 6};
  for (int i = 0; i < 6; i++)
    a[i] = b[i] * s;
  /* Hardcoded checks so each a[N] uses a constant index. */
  check("a[0]", __real__ a[0], __imag__ a[0], 3.0, 1.0);
  check("a[1]", __real__ a[1], __imag__ a[1], 6.0, 2.0);
  check("a[2]", __real__ a[2], __imag__ a[2], 9.0, 3.0);
  check("a[3]", __real__ a[3], __imag__ a[3], 12.0, 4.0);
  check("a[4]", __real__ a[4], __imag__ a[4], 15.0, 5.0);
  check("a[5]", __real__ a[5], __imag__ a[5], 18.0, 6.0);

  /* complex * complex still works (full 4-mul formula) */
  _Complex double t = 2.0 + 3.0i;
  _Complex double r5 = s * t;
  /* (3+i)*(2+3i) = (6-3) + (9+2)i = 3 + 11i */
  check("(3+1i) * (2+3i)", __real__ r5, __imag__ r5, 3.0, 11.0);

  if (fail_count == 0)
    printf("OK\n");
  return fail_count;
}
