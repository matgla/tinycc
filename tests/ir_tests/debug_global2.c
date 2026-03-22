extern int printf(const char *, ...);

/* Test: global imaginary constant init */
_Complex double g_imag = 1.0i;

/* Test: global real+imag constant init */
_Complex double g_both = 3.0 + 1.0i;

/* Test: global complex float */
_Complex float g_float_imag = 1.0fi;

int main(void)
{
  printf("g_imag: (%f, %f)\n", __real__ g_imag, __imag__ g_imag);
  printf("g_both: (%f, %f)\n", __real__ g_both, __imag__ g_both);
  printf("g_float_imag: (%f, %f)\n", (double)__real__ g_float_imag, (double)__imag__ g_float_imag);
  return 0;
}
