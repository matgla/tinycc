#include <stdint.h>
#include <stdio.h>

/* Regression test: __aeabi_dmul must produce correct IEEE-754 results.
 * This avoids %f / dtoa paths by checking raw bits.
 */

typedef union
{
  double d;
  uint64_t u;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} dbl_u;

/* Provided by lib/fp/soft/dmul.c (or hard-float variants). */
double __aeabi_dmul(double a, double b);

static int check_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got != exp)
  {
    dbl_u g, e;
    g.u = got;
    e.u = exp;
    printf("FAIL %s got=0x%08x%08x exp=0x%08x%08x\n", name, g.w.hi, g.w.lo, e.w.hi, e.w.lo);
    return 1;
  }
  return 0;
}

int main(void)
{
  /* 3.14 = 0x40091eb851eb851f */
  dbl_u a;
  a.u = 0x40091eb851eb851fULL;
  /* 2.0 = 0x4000000000000000 */
  dbl_u b;
  b.u = 0x4000000000000000ULL;

  double r = __aeabi_dmul(a.d, b.d);
  dbl_u out;
  out.d = r;

  /* 6.28 = 3.14 * 2.0 => exponent +1, mantissa unchanged */
  if (check_u64("3.14*2.0", out.u, 0x40191eb851eb851fULL))
    return 1;

  printf("PASS\n");
  return 0;
}
