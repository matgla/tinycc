#include <stdio.h>
volatile double vd = 2.5;
__attribute__((noinline)) _Complex double cd(void) { return 3.5 + 4.5i; }
__attribute__((noinline)) _Complex short cs(void) { _Complex short r = 0; __real__ r = 5; __imag__ r = -6; return r; }
__attribute__((noinline)) _Complex unsigned char cuc(void) { _Complex unsigned char r = 0; __real__ r = 250; __imag__ r = 200; return r; }
__attribute__((noinline)) _Complex signed char cc(void) { _Complex signed char r = 0; __real__ r = -3; __imag__ r = -128; return r; }
int main(void) {
  double d = vd;
  printf("%g %d\n", __imag__ d, (int)sizeof(__imag__ d));
  printf("%g %g %d %d\n", __real__ cd(), __imag__ cd(), __real__ cs(), __imag__ cs());
  printf("%d %d %d %d\n", __real__ cuc(), __imag__ cuc(), __real__ cc(), __imag__ cc());
  return 0;
}
