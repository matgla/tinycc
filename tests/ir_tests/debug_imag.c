extern int printf(const char *, ...);

int main(void)
{
  _Complex double a = 1.0i;
  printf("(%f, %f)\n", __real__ a, __imag__ a);
  return 0;
}
