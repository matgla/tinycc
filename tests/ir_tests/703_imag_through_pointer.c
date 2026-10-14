#include <stdio.h>

/* __imag__ on an lvalue adds the element size to the SValue offset, which an
 * indirect lvalue (address in a register) ignored: `__imag__ *p` read the
 * real part. */
typedef struct { int k; _Complex float z; } H;
__attribute__((noinline)) double im(_Complex double *p) { return __imag__ *p; }
__attribute__((noinline)) double re(_Complex double *p) { return __real__ *p; }
__attribute__((noinline)) void setim(_Complex double *p, double v) { __imag__ *p = v; }
__attribute__((noinline)) int imi(_Complex int *p) { return __imag__ p[1]; }
__attribute__((noinline)) float imh(H *h) { return __imag__ h->z; }
__attribute__((noinline)) unsigned ims(_Complex unsigned short *p) { return __imag__ *p; }
_Complex double g = 5.0 + 6.0i;

int main(void)
{
  _Complex double z = 3.0 + 4.0 * 1.0i;
  _Complex int zi[2] = {1 + 2i, 7 + 9i};
  H h = {1, 2.0f + 8.0fi};
  _Complex unsigned short us;
  __real__ us = 3;
  __imag__ us = 40000;
  printf("%d %d\n", (int)re(&z), (int)im(&z));
  setim(&z, 11.0);
  printf("%d %d %d %d %d %u\n", (int)__real__ z, (int)__imag__ z, imi(zi), (int)imh(&h), (int)__imag__ g, ims(&us));
  return 0;
}
