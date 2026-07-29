/* The double->float libcall narrowing (ssa narrow_float_calls /
 * flat float_narrow) rewrites `(float)ceil((double)x)` into `ceilf(x)`.
 * Inside ceilf's OWN definition — exactly how libm implements the float
 * wrappers — that rewrite turns the body into `return ceilf(x);`, and
 * infinite_self_recursion then (consistently, by that point) collapses the
 * function to a relocation-less `b .` self-loop.  On-device every
 * ceilf/floorf/roundf/truncf call spun forever (fp_libm_exec hang; libc's
 * own floorf masked that one entry).  The narrowing must skip the rewrite
 * when its target IS the function being compiled.
 */
#include <stdio.h>

double ceil(double x)
{
  long ip = (long)x;
  if (x > 0 && x != (double)ip)
    ip++;
  return (double)ip;
}

double floor(double x)
{
  long ip = (long)x;
  if (x < 0 && x != (double)ip)
    ip--;
  return (double)ip;
}

double round(double x)
{
  return (x < 0) ? ceil(x - 0.5) : floor(x + 0.5);
}

double trunc(double x)
{
  return (x < 0.0) ? ceil(x) : floor(x);
}

/* The exact libm wrapper shape that used to collapse to `b .` */
float ceilf(float x) { return (float)ceil((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float roundf(float x) { return (float)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }

volatile float in_pos = 2.5f;
volatile float in_neg = -2.5f;

int main(void)
{
  printf("c=%d %d\n", (int)ceilf(in_pos), (int)ceilf(in_neg));
  printf("f=%d %d\n", (int)floorf(in_pos), (int)floorf(in_neg));
  printf("r=%d %d\n", (int)roundf(in_pos), (int)roundf(in_neg));
  printf("t=%d %d\n", (int)truncf(in_pos), (int)truncf(in_neg));

  if ((int)ceilf(in_pos) == 3 && (int)ceilf(in_neg) == -2 &&
      (int)floorf(in_pos) == 2 && (int)floorf(in_neg) == -3 &&
      (int)roundf(in_pos) == 3 && (int)roundf(in_neg) == -3 &&
      (int)truncf(in_pos) == 2 && (int)truncf(in_neg) == -2)
    printf("OK\n");
  else
    printf("FAIL\n");
  return 0;
}
