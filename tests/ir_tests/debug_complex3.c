extern int printf(const char *, ...);

int main(void)
{
  /* Test 1: Real-to-complex assignment (works) */
  _Complex float a = 1.0f;
  printf("test1: %.1f + %.1fi\n", (double)__real__ a, (double)__imag__ a);

  /* Test 2: Complex float with __real__ and __imag__ init */
  _Complex float b;
  __real__ b = 3.0f;
  __imag__ b = 1.0f;
  printf("test2: %.1f + %.1fi\n", (double)__real__ b, (double)__imag__ b);

  /* Test 3: Complex float addition */
  _Complex float c = a + b;
  printf("test3: %.1f + %.1fi\n", (double)__real__ c, (double)__imag__ c);

  /* Test 4: Complex double with __real__ and __imag__ */
  _Complex double d;
  __real__ d = 5.0;
  __imag__ d = 2.0;
  printf("test4: %.1f + %.1fi\n", __real__ d, __imag__ d);

  /* Test 5: Return complex from function (via global) */

  return 0;
}
