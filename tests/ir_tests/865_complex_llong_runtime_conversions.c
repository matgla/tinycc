#include <stdio.h>

typedef _Complex int ci;
typedef _Complex long long cll;

__attribute__((noinline)) static int seven(void)
{
  return 7;
}

__attribute__((noinline)) static void show(const char *name, cll value)
{
  printf("%s=%lld %lld\n", name, __real__ value, __imag__ value);
}

int main(void)
{
  ci u;
  cll z;
  ci q;
  cll v = seven();

  __real__ u = 5;
  __imag__ u = 6;
  __real__ z = 8;
  __imag__ z = 9;
  q = (ci)z;

  show("constant", (cll)7);
  show("real", (cll)seven());
  show("int", u);
  printf("narrow=%d %d\n", __real__ q, __imag__ q);
  printf("local=%lld %lld\n", __real__ v, __imag__ v);
  return 0;
}
