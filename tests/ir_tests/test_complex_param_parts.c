/* Writing one half of a complex parameter: __imag__ x = ... must land in the
 * imaginary half whether x arrived wholly on the stack or split across r2-r3
 * and the stack (the IR names both halves through x's vreg, the imaginary one
 * at a sub-offset of its slot). */
#include <stdio.h>

__attribute__((noinline)) static double tw(double v) { return v * 2.0 + 1.0; }

__attribute__((noinline)) static _Complex double on_stack(int a, int b, int c, int d, _Complex double x)
{
  __imag__ x = tw(__imag__ x) + d;
  __real__ x = tw(__real__ x) + a + b + c;
  return x;
}

__attribute__((noinline)) static _Complex double split(_Complex double x)
{
  __imag__ x = tw(__imag__ x);
  return x;
}

__attribute__((noinline)) static _Complex float split_f(int a, int b, int c, _Complex float x)
{
  __imag__ x = (float)tw(__imag__ x) + a;
  __real__ x = (float)tw(__real__ x) + b + c;
  return x;
}

int main(void)
{
  _Complex double r = on_stack(1, 2, 3, 4, 5.0 + 7.0i);
  printf("%d %d\n", (int)__real__ r, (int)__imag__ r);
  r = split(3.0 + 9.0i);
  printf("%d %d\n", (int)__real__ r, (int)__imag__ r);
  _Complex float f = split_f(1, 2, 3, 6.0f + 8.0fi);
  printf("%d %d\n", (int)__real__ f, (int)__imag__ f);
  return 0;
}
