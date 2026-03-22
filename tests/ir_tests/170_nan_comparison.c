/*
 * Test IEEE 754 NaN comparison semantics for soft-float.
 *
 * All comparisons involving NaN must return false, except != which
 * returns true. This exercises the GT/GE operand-swap fix in
 * ir/core.c that makes __aeabi_cdcmple work correctly for all
 * condition codes.
 */
#include <stdio.h>

static double get_nan(void)
{
  return 0.0 / 0.0;
}

static float get_nanf(void)
{
  return 0.0f / 0.0f;
}

int main(void)
{
  volatile double nan = get_nan();
  volatile double x = 1.0;
  volatile float nanf = get_nanf();
  volatile float xf = 1.0f;
  int ok = 1;

  /* Double NaN comparisons */
  if (nan == nan)  { printf("FAIL: nan == nan\n"); ok = 0; }
  if (!(nan != nan)) { printf("FAIL: nan != nan\n"); ok = 0; }
  if (nan < x)    { printf("FAIL: nan < x\n"); ok = 0; }
  if (nan > x)    { printf("FAIL: nan > x\n"); ok = 0; }
  if (nan <= x)   { printf("FAIL: nan <= x\n"); ok = 0; }
  if (nan >= x)   { printf("FAIL: nan >= x\n"); ok = 0; }
  if (x < nan)    { printf("FAIL: x < nan\n"); ok = 0; }
  if (x > nan)    { printf("FAIL: x > nan\n"); ok = 0; }
  if (x <= nan)   { printf("FAIL: x <= nan\n"); ok = 0; }
  if (x >= nan)   { printf("FAIL: x >= nan\n"); ok = 0; }

  /* Float NaN comparisons */
  if (nanf == nanf) { printf("FAIL: nanf == nanf\n"); ok = 0; }
  if (!(nanf != nanf)) { printf("FAIL: nanf != nanf\n"); ok = 0; }
  if (nanf < xf)  { printf("FAIL: nanf < xf\n"); ok = 0; }
  if (nanf > xf)  { printf("FAIL: nanf > xf\n"); ok = 0; }
  if (nanf <= xf) { printf("FAIL: nanf <= xf\n"); ok = 0; }
  if (nanf >= xf) { printf("FAIL: nanf >= xf\n"); ok = 0; }
  if (xf < nanf)  { printf("FAIL: xf < nanf\n"); ok = 0; }
  if (xf > nanf)  { printf("FAIL: xf > nanf\n"); ok = 0; }
  if (xf <= nanf) { printf("FAIL: xf <= nanf\n"); ok = 0; }
  if (xf >= nanf) { printf("FAIL: xf >= nanf\n"); ok = 0; }

  /* Normal comparisons still work */
  volatile double a = 3.0, b = 5.0;
  if (!(a < b))   { printf("FAIL: 3.0 < 5.0\n"); ok = 0; }
  if (!(a <= b))  { printf("FAIL: 3.0 <= 5.0\n"); ok = 0; }
  if (a > b)      { printf("FAIL: 3.0 > 5.0\n"); ok = 0; }
  if (a >= b)     { printf("FAIL: 3.0 >= 5.0\n"); ok = 0; }
  if (!(b > a))   { printf("FAIL: 5.0 > 3.0\n"); ok = 0; }
  if (!(b >= a))  { printf("FAIL: 5.0 >= 3.0\n"); ok = 0; }

  /* -0.0 == +0.0 */
  volatile double pz = 0.0, nz = -0.0;
  if (!(pz == nz)) { printf("FAIL: 0.0 == -0.0\n"); ok = 0; }
  if (nz < pz)    { printf("FAIL: -0.0 < 0.0\n"); ok = 0; }

  if (ok)
    printf("all nan comparison tests passed\n");
  return ok ? 0 : 1;
}
