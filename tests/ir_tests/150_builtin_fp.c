/* Test IEEE FP builtins: __builtin_isnan, __builtin_inf, __builtin_nan,
 * __builtin_huge_val, __builtin_fabs, __builtin_isunordered,
 * __builtin_isless, __builtin_isgreater, __builtin_islessequal,
 * __builtin_isgreaterequal, __builtin_islessgreater */
#include <stdio.h>

int main(void)
{
  /* __builtin_inf / __builtin_inff */
  double inf_d = __builtin_inf();
  float inf_f = __builtin_inff();
  printf("inf_d > 1e308: %d\n", inf_d > 1e308);
  printf("inf_f > 1e38f: %d\n", inf_f > 1e38f);

  /* __builtin_huge_val / __builtin_huge_valf */
  double huge_d = __builtin_huge_val();
  float huge_f = __builtin_huge_valf();
  printf("huge_d > 1e308: %d\n", huge_d > 1e308);
  printf("huge_f > 1e38f: %d\n", huge_f > 1e38f);

  /* __builtin_nan / __builtin_nanf */
  double nan_d = __builtin_nan("");
  float nan_f = __builtin_nanf("");
  printf("nan_d != nan_d: %d\n", nan_d != nan_d);
  printf("nan_f != nan_f: %d\n", nan_f != nan_f);

  /* __builtin_isnan */
  printf("isnan(nan_d): %d\n", __builtin_isnan(nan_d) != 0);
  printf("isnan(1.0): %d\n", __builtin_isnan(1.0) != 0);
  printf("isnan(inf_d): %d\n", __builtin_isnan(inf_d) != 0);
  printf("isnanf(nan_f): %d\n", __builtin_isnanf(nan_f) != 0);
  printf("isnanf(1.0f): %d\n", __builtin_isnanf(1.0f) != 0);

  /* __builtin_isinf */
  printf("isinf(inf_d): %d\n", __builtin_isinf(inf_d) != 0);
  printf("isinf(nan_d): %d\n", __builtin_isinf(nan_d) != 0);
  printf("isinf(1.0): %d\n", __builtin_isinf(1.0) != 0);

  /* __builtin_fabs / __builtin_fabsf */
  double fabs_d = __builtin_fabs(-3.14);
  float fabs_f = __builtin_fabsf(-2.5f);
  printf("fabs(-3.14): %f\n", fabs_d);
  printf("fabsf(-2.5f): %f\n", (double)fabs_f);

  /* __builtin_isunordered */
  printf("isunordered(1.0, 2.0): %d\n", __builtin_isunordered(1.0, 2.0));
  printf("isunordered(nan_d, 1.0): %d\n", __builtin_isunordered(nan_d, 1.0) != 0);
  printf("isunordered(1.0, nan_d): %d\n", __builtin_isunordered(1.0, nan_d) != 0);

  /* __builtin_isless etc. */
  volatile double a = 1.0, b = 2.0, c = 1.0;
  printf("isless(1.0, 2.0): %d\n", __builtin_isless(a, b) != 0);
  printf("isless(2.0, 1.0): %d\n", __builtin_isless(b, a) != 0);
  printf("isgreater(2.0, 1.0): %d\n", __builtin_isgreater(b, a) != 0);
  printf("isgreater(1.0, 2.0): %d\n", __builtin_isgreater(a, b) != 0);
  printf("islessequal(1.0, 1.0): %d\n", __builtin_islessequal(a, c) != 0);
  printf("isgreaterequal(1.0, 1.0): %d\n", __builtin_isgreaterequal(a, c) != 0);

  /* __builtin_signbit */
  printf("signbit(-1.0): %d\n", __builtin_signbit(-1.0) != 0);
  printf("signbit(1.0): %d\n", __builtin_signbit(1.0) != 0);

  /* __builtin_copysign */
  double cs = __builtin_copysign(3.14, -1.0);
  printf("copysign(3.14, -1.0): %f\n", cs);

  return 0;
}
