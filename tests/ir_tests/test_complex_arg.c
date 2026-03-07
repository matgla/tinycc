#include <stdio.h>
#include <stdlib.h>

void foo(__complex__ double x)
{
  double re = __real__ x;
  double im = __imag__ x;
  printf("foo: real=%f imag=%f\n", re, im);
  if (re != 1.0 || im != 2.0)
    abort();
}

void bar(__complex__ float x)
{
  float re = __real__ x;
  float im = __imag__ x;
  printf("bar: real=%f imag=%f\n", (double)re, (double)im);
  if (re != 3.0f || im != 4.0f)
    abort();
}

int main()
{
  __complex__ double x;
  __real__ x = 1.0;
  __imag__ x = 2.0;
  printf("main: about to call foo\n");
  foo(x);

  __complex__ float y;
  __real__ y = 3.0f;
  __imag__ y = 4.0f;
  printf("main: about to call bar\n");
  bar(y);

  printf("PASS\n");
  return 0;
}
