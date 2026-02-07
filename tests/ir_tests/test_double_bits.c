#include <stdint.h>
#include <stdio.h>

static void dump(const char *name, double x)
{
  union
  {
    double d;
    uint64_t u;
  } v;
  v.d = x;
  printf("%s=%.6f\n", name, x);
  printf("%s_bits=0x%08lx%08lx\n", name, (unsigned long)(v.u >> 32), (unsigned long)(v.u & 0xffffffffu));
  printf("%s_g=%.17g\n", name, x);
}

int main(void)
{
  double a = 1.5;
  double b = 2.0;

  dump("sum", a + b);
  dump("diff", a - b);
  dump("prod", a * b);
  dump("div", a / b);
  printf("PASS\n");
  return 0;
}
