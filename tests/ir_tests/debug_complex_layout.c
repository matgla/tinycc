extern int printf(const char *, ...);

/* Verify __real__ and __imag__ offsets */
typedef struct
{
  double real;
  double imag;
} cdouble_t;
typedef struct
{
  float real;
  float imag;
} cfloat_t;

cdouble_t gcd = {0.0, 1.0};
cfloat_t gcf = {0.0f, 1.0f};

_Complex double gd;
_Complex float gf;

int main(void)
{
  /* Struct approach: verify memory layout */
  printf("struct double: real=%f imag=%f\n", gcd.real, gcd.imag);
  printf("struct float: real=%f imag=%f\n", (double)gcf.real, (double)gcf.imag);

  /* Manual init complex via memcpy at runtime */
  double parts_d[2] = {0.0, 1.0};
  double parts_f[2] = {0.0f, 1.0f};

  /* Copy {0.0, 1.0} directly to the complex double */
  void *pd = &gd;
  void *pf = &gf;

  /* Write real part = 0.0, imag part = 1.0 */
  double d_zero = 0.0, d_one = 1.0;
  float f_zero = 0.0f, f_one = 1.0f;

  /* Use pointer arithmetic to write directly */
  ((double *)pd)[0] = d_zero;
  ((double *)pd)[1] = d_one;
  printf("memcpy double: real=%f imag=%f\n", __real__ gd, __imag__ gd);

  ((float *)pf)[0] = f_zero;
  ((float *)pf)[1] = f_one;
  printf("memcpy float: real=%f imag=%f\n", (double)__real__ gf, (double)__imag__ gf);

  return 0;
}
