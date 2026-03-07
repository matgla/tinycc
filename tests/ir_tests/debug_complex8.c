extern int printf(const char *, ...);

_Complex float v = 3.0f + 1.0fi;

_Complex float bar(void)
{
  return v;
}

int main(void)
{
  printf("v = %.1f + %.1fi\n", (double)__real__ v, (double)__imag__ v);

  _Complex float result = bar();
  printf("result = %.1f + %.1fi\n", (double)__real__ result, (double)__imag__ result);

  return 0;
}
