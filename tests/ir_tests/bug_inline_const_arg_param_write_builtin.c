/* always_inline const-arg substitution must not fold builtins on a written parameter.
 * Pass: inline_subst_const_arg (inline_const_args); fix: skip recording params the body writes. */
/*
 * TCC copyright block placeholder
 */
#include <stdio.h>

#define AI static inline __attribute__((always_inline))

AI double assign_fabs(double x) { x = x * 2; return __builtin_fabs(x); }
AI double compound_fabs(double x) { x += 1.0; return __builtin_fabs(x); }
AI double incr_fabs(double x) { x++; return __builtin_fabs(x); }
AI int assign_isinf(double x) { x = x * 1e300 * 1e300; return __builtin_isinf(x) != 0; }
AI double ptr_fabs(double x) { double *p = &x; *p = -*p - 10.0; return __builtin_fabs(x); }
/* Pinned: unwritten parameters still fold. */
AI double plain_fabs(double x) { return __builtin_fabs(x); }
AI int plain_isinf(double x) { return __builtin_isinf(x) != 0; }
AI double other_written(double x, double y) { y = -y; return __builtin_fabs(x) + __builtin_fabs(y); }

int main(void)
{
  printf("%g\n", assign_fabs(-3.0));
  printf("%g\n", compound_fabs(-3.0));
  printf("%g\n", incr_fabs(-3.0));
  printf("%d\n", assign_isinf(1.0));
  printf("%g\n", ptr_fabs(-3.0));
  printf("%g\n", plain_fabs(-3.0));
  printf("%d\n", plain_isinf(1.0));
  printf("%g\n", other_written(-2.0, 5.0));
  return 0;
}
