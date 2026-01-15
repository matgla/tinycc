// Test __aeabi_f2d by checking raw bit representation
#include <stdint.h>
#include <stdio.h>

static union
{
  float f;
  uint32_t u;
} fu;

static union
{
  double d;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} du;

int main(void)
{
  printf("Testing __aeabi_f2d (bit check)\n");

  fu.f = 1.0f;
  du.d = (double)fu.f;

  // Print raw bits of input float
  printf("Float bits: 0x%08x\n", fu.u);

  // Print raw bits of output double
  printf("Double hi: 0x%08x\n", du.w.hi);
  printf("Double lo: 0x%08x\n", du.w.lo);

  // Expected: 1.0f = 0x3f800000
  // Expected: 1.0d = 0x3ff0000000000000
  // So hi=0x3ff00000, lo=0x00000000

  if (du.w.hi == 0x3ff00000 && du.w.lo == 0x00000000)
  {
    printf("PASS: Double value is correct!\n");
  }
  else
  {
    printf("FAIL: Expected 0x3ff00000:00000000\n");
    return 1;
  }

  return 0;
}
