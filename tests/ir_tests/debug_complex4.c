extern int printf(const char *, ...);

int main(void)
{
  _Complex float a;
  __real__ a = 1.0f;
  __imag__ a = 0.0f;

  _Complex float b;
  __real__ b = 3.0f;
  __imag__ b = 1.0f;

  printf("a: %.1f + %.1fi\n", (double)__real__ a, (double)__imag__ a);
  printf("b: %.1f + %.1fi\n", (double)__real__ b, (double)__imag__ b);

  _Complex float c = a + b;
  printf("c: %.1f + %.1fi\n", (double)__real__ c, (double)__imag__ c);

  return 0;
}
