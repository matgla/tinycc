extern void abort(void);
extern int printf(const char *, ...);

_Complex double v = 3.0 + 1.0iF;

void foo(_Complex double z, int *x)
{
  double zr = __real__ z;
  double zi = __imag__ z;
  double vr = __real__ v;
  double vi = __imag__ v;
  printf("foo: z = (%f, %f), v = (%f, %f)\n", zr, zi, vr, vi);
  if (z != v)
  {
    printf("MISMATCH!\n");
    abort();
  }
}

_Complex double bar(_Complex double z) __attribute__((pure));
_Complex double bar(_Complex double z)
{
  double vr = __real__ v;
  double vi = __imag__ v;
  printf("bar: returning v = (%f, %f)\n", vr, vi);
  return v;
}

int baz(void)
{
  int a, i;
  for (i = 0; i < 6; i++)
  {
    _Complex double bval = bar(1.0iF * i);
    double br = __real__ bval;
    double bi = __imag__ bval;
    printf("baz: i=%d, bar returned (%f, %f)\n", i, br, bi);
    foo(bval, &a);
  }
  return 0;
}

int main()
{
  double vr = __real__ v;
  double vi = __imag__ v;
  printf("main: v = (%f, %f)\n", vr, vi);
  baz();
  printf("PASS\n");
  return 0;
}
