#include <stdio.h>

int main()
{
  // Test 1: basic complex division
  _Complex unsigned a = 42;
  _Complex unsigned b = 7;
  printf("a: real=%u imag=%u\n", __real__ a, __imag__ a);
  printf("b: real=%u imag=%u\n", __real__ b, __imag__ b);

  _Complex unsigned r = a / b;
  printf("42/7: real=%u imag=%u\n", __real__ r, __imag__ r);

  // Test 2: simple unsigned division (not complex)
  unsigned x = 42;
  unsigned y = 7;
  printf("simple 42/7 = %u\n", x / y);

  // Test 3: what does complex /= do
  _Complex unsigned t = 42;
  _Complex unsigned c = 7;
  printf("before /=: real=%u imag=%u\n", __real__ t, __imag__ t);
  t /= c;
  printf("after /=: real=%u imag=%u\n", __real__ t, __imag__ t);

  return 0;
}
