extern int printf(const char *, ...);
extern void abort(void);

_Complex double v = 3.0 + 1.0iF;

_Complex double bar(_Complex double z)
{
  return v;
}

void foo(_Complex double z, int *x)
{
  printf("foo: z = %.1f + %.1fi, v = %.1f + %.1fi\n", __real__ z, __imag__ z, __real__ v, __imag__ v);
  if (z != v)
  {
    printf("MISMATCH!\n");
    abort();
  }
}

int main(void)
{
  printf("v = %.1f + %.1fi\n", __real__ v, __imag__ v);

  _Complex double result = bar(0.0);
  printf("bar result = %.1f + %.1fi\n", __real__ result, __imag__ result);

  foo(result, (int *)0);
  printf("PASS\n");
  return 0;
}
