extern int printf(const char *, ...);

/* Test 1: basic imaginary literal */
int main(void)
{
  /* Test imaginary double */
  _Complex double a = 1.0i;
  printf("1.0i: (%f, %f)\n", __real__ a, __imag__ a);

  /* Test imaginary float */
  _Complex float b = 1.0fi;
  printf("1.0fi: (%f, %f)\n", (double)__real__ b, (double)__imag__ b);

  /* Test imaginary float (reversed suffix) */
  _Complex float c = 1.0iF;
  printf("1.0iF: (%f, %f)\n", (double)__real__ c, (double)__imag__ c);

  /* Test complex init with addition */
  _Complex double d = 3.0 + 1.0i;
  printf("3.0 + 1.0i: (%f, %f)\n", __real__ d, __imag__ d);

  _Complex double e = 3.0 + 1.0iF;
  printf("3.0 + 1.0iF: (%f, %f)\n", __real__ e, __imag__ e);

  return 0;
}
