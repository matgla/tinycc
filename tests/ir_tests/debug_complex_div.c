#include <stdio.h>

unsigned char g;

unsigned char foo(_Complex unsigned c)
{
  unsigned char v = g;
  _Complex unsigned t = 3;
  t /= c;
  return v + t;
}

unsigned char bar(_Complex unsigned c)
{
  unsigned char v = g;
  _Complex unsigned t = 42;
  t /= c;
  return v + t;
}

int main()
{
  printf("foo(7) = %d\n", foo(7));
  printf("bar(7) = %d\n", bar(7));

  // Also test basic complex division
  _Complex unsigned a = 42;
  _Complex unsigned b = 7;
  _Complex unsigned r = a / b;
  printf("42 / 7 complex: real=%u imag=%u\n", __real__ r, __imag__ r);

  _Complex unsigned c2 = 3;
  _Complex unsigned d = 7;
  _Complex unsigned r2 = c2 / d;
  printf("3 / 7 complex: real=%u imag=%u\n", __real__ r2, __imag__ r2);

  return 0;
}
