/* Hard-float: a double read with va_arg passed straight on as a double
 * argument -- yaslibc's printf does `ofloat(fp, va_arg(ap, double), ...)`.
 * The argument is a dereference of the va_list pointer (T***DEREF***); the
 * d-register packing split it as if it were already a register pair and the
 * compiler failed ("mach_make_hi_half: 64-bit REG operand has invalid r1")
 * at every -O.  Returns 1 when the values arrive intact. */
#include <stdarg.h>

__attribute__((noinline)) static int take(double d, int w, int f, double e)
{
  return d == 2.5 && w == 3 && f == 7 && e == -0.125;
}

__attribute__((noinline)) static int fwd(int n, ...)
{
  va_list ap;
  int r;
  va_start(ap, n);
  r = take(va_arg(ap, double), n, 7, va_arg(ap, double));
  va_end(ap);
  return r;
}

__attribute__((noinline)) static int fwd_ld(int n, ...)
{
  va_list ap;
  int r;
  va_start(ap, n);
  r = take((double)va_arg(ap, long double), n, 7, -0.125);
  va_end(ap);
  return r;
}

int main(void)
{
  return fwd(3, 2.5, -0.125) && fwd_ld(3, (long double)2.5);
}
