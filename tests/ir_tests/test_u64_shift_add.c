#include <stdint.h>
#include <stdio.h>

typedef union
{
  uint64_t u;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} u64_u;

static int check_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got != exp)
  {
    u64_u g, e;
    g.u = got;
    e.u = exp;
    printf("FAIL %s got=0x%08x%08x exp=0x%08x%08x\n", name, g.w.hi, g.w.lo, e.w.hi, e.w.lo);
    return 1;
  }
  return 0;
}

int main(void)
{
  /* These mirror the mantissa path for 7.5 + 0.5 inside soft __aeabi_dadd:
   * 7.5  bits=0x401e000000000000 => mant|implicit = 0x001e000000000000
   * 0.5  bits=0x3fe0000000000000 => mant|implicit = 0x0010000000000000
   * exp_diff = 3
   */
  const uint64_t DOUBLE_IMPLICIT_BIT = (1ULL << 52);
  const uint64_t CARRY_BIT = (DOUBLE_IMPLICIT_BIT << 1); /* bit 53 */

  uint64_t a_mant = 0x001e000000000000ULL; /* implicit|mant for 7.5 */
  uint64_t b_mant = 0x0010000000000000ULL; /* implicit for 0.5 */

  int exp = 1025; /* exponent for 7.5 */
  int exp_diff = 3;

  printf("stage=a_mant 0x%08x%08x\n", (uint32_t)(a_mant >> 32), (uint32_t)a_mant);
  printf("stage=b_mant 0x%08x%08x\n", (uint32_t)(b_mant >> 32), (uint32_t)b_mant);

  b_mant >>= exp_diff;
  printf("stage=b_shift 0x%08x%08x\n", (uint32_t)(b_mant >> 32), (uint32_t)b_mant);
  if (check_u64("b_mant>>3", b_mant, 0x0002000000000000ULL))
    return 1;

  uint64_t r = a_mant + b_mant;
  printf("stage=sum 0x%08x%08x\n", (uint32_t)(r >> 32), (uint32_t)r);
  if (check_u64("a_mant+b_mant", r, 0x0020000000000000ULL))
    return 1;

  if (r & CARRY_BIT)
  {
    r >>= 1;
    exp++;
  }

  printf("stage=norm r=0x%08x%08x exp=%d\n", (uint32_t)(r >> 32), (uint32_t)r, exp);
  if (check_u64("norm_r", r, 0x0010000000000000ULL))
    return 1;
  if (exp != 1026)
  {
    printf("FAIL exp got=%d exp=%d\n", exp, 1026);
    return 1;
  }

  printf("PASS\n");
  return 0;
}
