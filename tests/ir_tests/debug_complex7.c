extern int printf(const char *, ...);

_Complex double v = 3.0 + 1.0iF;

_Complex double bar(void)
{
  return v;
}

int main(void)
{
  printf("v = %.1f + %.1fi\n", __real__ v, __imag__ v);

  _Complex double result = bar();
  printf("result = %.1f + %.1fi\n", __real__ result, __imag__ result);

  return 0;
}
