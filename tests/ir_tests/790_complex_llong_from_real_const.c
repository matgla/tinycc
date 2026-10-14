/* A real constant converted to __complex__ long long got the real part as its
 * imaginary part too: vstore unpacked the constant with `packed >> 64`, UB that
 * the x86 host compiler ran as a shift by 0.  Every -O level.  Found by the
 * UBSan sweep (scripts/asan_sweep.py) on gcc.c-torture/compile/20000804-1.c. */
#include <stdio.h>

__complex__ long long g;
__complex__ unsigned long long gu;

int main(void)
{
  __complex__ long long v = 5;
  g = 7;
  gu = 9u;
  printf("%lld %lld %lld %lld %llu %llu\n", __real__ v, __imag__ v, __real__ g, __imag__ g,
         __real__ gu, __imag__ gu);
  return 0;
}
