/* Test complex integer addition */
int main()
{
  _Complex unsigned a = 10;
  _Complex unsigned b = 3;
  _Complex unsigned r = a + b;
  unsigned real = __real__ r;
  unsigned imag = __imag__ r;
  // Expected: real=13, imag=0
  if (real != 13)
    return 1;
  if (imag != 0)
    return 2;
  return 0;
}
