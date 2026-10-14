/* Imaginary int/float literals: the unused half of the CValue was left
 * uninitialised and reached the emitted constants (primary.c TOK_CINT_I /
 * TOK_CFLOAT_I). Fix: zero the whole CValue before packing; int-complex constants converted
 * to float complex now unpack by element size (7i read as one double). */
#include <stdio.h>

__attribute__((noinline)) _Complex double zero_i(void) { return 0i; }
__attribute__((noinline)) _Complex double three_plus_0i(void) { return 3.0 + 0i; }
__attribute__((noinline)) _Complex double seven_i(void) { return 7i; }
__attribute__((noinline)) _Complex float f_i(void) { return 2.5fi; }
__attribute__((noinline)) _Complex float f_zero(void) { return 1.5f + 0i; }
__attribute__((noinline)) _Complex int int_i(void) { return 200i; }
__attribute__((noinline)) _Complex long long ll_i(void) { return 9i; }
__attribute__((noinline)) _Complex double three_plus_7i(void) { return 3.0 + 7i; }
__attribute__((noinline)) _Complex double d_i(void) { return 1.25i; }

int main(void)
{
  _Complex double a = zero_i(), b = three_plus_0i(), c = seven_i(), d = d_i();
  _Complex float e = f_i(), f = f_zero();
  _Complex int g = int_i();
  _Complex long long h = ll_i();
  printf("%g %g\n", __real__ a, __imag__ a);
  printf("%g %g\n", __real__ b, __imag__ b);
  printf("%g %g\n", __real__ c, __imag__ c);
  _Complex double k = three_plus_7i();
  printf("%g %g\n", __real__ k, __imag__ k);
  printf("%g %g\n", __real__ d, __imag__ d);
  printf("%g %g\n", (double)__real__ e, (double)__imag__ e);
  printf("%g %g\n", (double)__real__ f, (double)__imag__ f);
  printf("%d %d\n", __real__ g, __imag__ g);
  printf("%lld %lld\n", __real__ h, __imag__ h);
  return 0;
}
