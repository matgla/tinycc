/* Test complex unsigned integer division */

unsigned char g;

__attribute__((noinline)) unsigned char foo(_Complex unsigned c)
{
  unsigned char v = g;
  _Complex unsigned t = 3;
  t /= c;
  return v + t;
}

__attribute__((noinline)) unsigned char bar(_Complex unsigned c)
{
  unsigned char v = g;
  _Complex unsigned t = 42;
  t /= c;
  return v + t;
}

__attribute__((noinline)) unsigned div_real(_Complex unsigned a, _Complex unsigned b)
{
  _Complex unsigned r = a / b;
  return __real__ r;
}

__attribute__((noinline)) unsigned div_imag(_Complex unsigned a, _Complex unsigned b)
{
  _Complex unsigned r = a / b;
  return __imag__ r;
}

int main()
{
  int ret = 0;

  unsigned r = div_real(42, 7);
  if (r != 6)
    ret = 1;

  unsigned i = div_imag(42, 7);
  if (i != 0)
    ret = 2;

  unsigned char x = foo(7);
  if (x != 0)
    ret = 3;

  unsigned char y = bar(7);
  if (y != 6)
    ret = 4;

  return ret;
}
