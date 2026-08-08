/*
 * ssa:branch folds a compare of a provably non-negative value against 0.0.
 * fabs() is the one source in that set that can still hand back a NaN, so only
 * the half of the directions that agree with the unordered answer may fold:
 *
 *   fabs(x) <  0.0   false for every x, NaN included   -- folds
 *   fabs(x) >= 0.0   false for a NaN                   -- must NOT fold
 *
 * gcc.c-torture/execute/20020720-1.c pins the first (its link_error() has to be
 * optimized away); this pins the second, which is only visible at runtime.
 * Both the __aeabi_c[df]cmple libcall shape and the inline hardware compare
 * (-mfpu=rp2350) go through the same decision, and they spell "unordered" with
 * different tokens, so run it under whichever the target picked.
 */
#include <stdio.h>

extern double fabs(double);
extern float fabsf(float);

static volatile double zero_d = 0.0;
static volatile float zero_f = 0.0f;

int main(void)
{
  volatile double nan_d = zero_d / zero_d;
  volatile float nan_f = zero_f / zero_f;
  volatile double val_d = -3.5;
  volatile float val_f = -3.5f;

  /* NaN: every relational compare is false, whichever way round it is. */
  printf("nan_lt=%d\n", fabs(nan_d) < 0.0);
  printf("nan_ge=%d\n", fabs(nan_d) >= 0.0);
  printf("nan_gt0=%d\n", 0.0 > fabs(nan_d));
  printf("nan_le0=%d\n", 0.0 <= fabs(nan_d));
  printf("nanf_lt=%d\n", fabsf(nan_f) < 0.0f);
  printf("nanf_ge=%d\n", fabsf(nan_f) >= 0.0f);
  printf("nanf_gt0=%d\n", 0.0f > fabsf(nan_f));
  printf("nanf_le0=%d\n", 0.0f <= fabsf(nan_f));

  /* An ordinary value still answers the way the fold claims. */
  printf("val_lt=%d\n", fabs(val_d) < 0.0);
  printf("val_ge=%d\n", fabs(val_d) >= 0.0);
  printf("val_gt0=%d\n", 0.0 > fabs(val_d));
  printf("val_le0=%d\n", 0.0 <= fabs(val_d));
  printf("valf_lt=%d\n", fabsf(val_f) < 0.0f);
  printf("valf_ge=%d\n", fabsf(val_f) >= 0.0f);
  printf("valf_gt0=%d\n", 0.0f > fabsf(val_f));
  printf("valf_le0=%d\n", 0.0f <= fabsf(val_f));

  /* The branch form, which is what actually folds. */
  if (fabs(nan_d) >= 0.0)
    printf("FAIL: fabs(NaN) >= 0.0\n");
  if (fabs(nan_d) < 0.0)
    printf("FAIL: fabs(NaN) < 0.0\n");
  if (!(fabs(val_d) >= 0.0))
    printf("FAIL: fabs(-3.5) >= 0.0\n");
  if (fabs(val_d) < 0.0)
    printf("FAIL: fabs(-3.5) < 0.0\n");

  printf("PASS\n");
  return 0;
}
