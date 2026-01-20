#include <stdint.h>
#include <stdio.h>

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

extern double __aeabi_i2d(int a);
extern double __aeabi_dmul(double a, double b);
extern double __aeabi_dadd(double a, double b);

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
  int i = 3;
  dbl_u x;
  x.d = 2.5;

  printf("stage=i i=%d\n", i);
  printf("stage=x bits=0x%08x%08x\n", x.w.hi, x.w.lo);
  fflush(stdout);

  dbl_u di;
  di.d = __aeabi_i2d(i);
  printf("stage=i2d bits=0x%08x%08x\n", di.w.hi, di.w.lo);
  fflush(stdout);
  if (check_u64("__aeabi_i2d(3)", di.u, 0x4008000000000000ULL))
    return 1;

  dbl_u prod;
  prod.d = __aeabi_dmul(x.d, di.d);
  printf("stage=dmul bits=0x%08x%08x\n", prod.w.hi, prod.w.lo);
  fflush(stdout);
  if (check_u64("__aeabi_dmul(2.5,3.0)", prod.u, 0x401e000000000000ULL))
    return 1;

  dbl_u y;
  y.d = __aeabi_dadd(prod.d, 0.5);
  printf("stage=dadd bits=0x%08x%08x\n", y.w.hi, y.w.lo);
  fflush(stdout);
  if (check_u64("__aeabi_dadd(7.5,0.5)", y.u, 0x4020000000000000ULL))
    return 1;

  printf("PASS\n");
  return 0;
}
